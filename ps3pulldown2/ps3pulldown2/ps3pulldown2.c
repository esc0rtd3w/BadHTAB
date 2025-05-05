#include "include.h"

/////////////////////////////////////////////////////////////////////////////
// ——— pin definitions ——————————————————————————————————————————————

//static const int pulldown_pin_id     = 16;  // one-wire glitch line
static const int pulldown1_pin_id = 15;
static const int pulldown2_pin_id = 16;

static const int pwr_on_pin_id       = 10;  // GP10 → PS3 “power button” trace
static const int sb_uart_rx_pin      = 5;   // GP5 ← PS3 SB_UART TX (UART1 RX)
static const int standby_mon_pin_id  = 18;  // GP18 ← PS3 PSU standby sense

static const int error_led_pin       = 6;   // red error LED on GP6
static const int yellow_led_pin      = 2;   // yellow LED on GP2
static const int green_led_pin       = 21;  // green LED on GP21
static const int blue_led_pin        = 27;  // blue LED on GP27

/*#if HDD_ACTIVITY_MONITOR
static const int hdd_activity_pin    = 22;  // GP22 ← PS3 HDD activity LED (active HIGH)
#endif*/

/////////////////////////////////////////////////////////////////////////////
// ——— globals ——————————————————————————————————————————————————————

volatile bool do_glitch     = false;
volatile bool is_stopped    = false;
volatile bool glitch_error  = false;
volatile bool glitch_success = false;
volatile bool hdd_activity = false;

volatile bool error_detect  = false;
volatile bool yellow_detect = false;
volatile bool green_detect  = false;
volatile bool blue_detect  = false;

static bool  glitch_started = false;
static bool  set_alarm = false;

static bool uart0_ready = false;
static bool uart1_ready = false;
static bool set_voltage_ready = false;
static bool set_sys_clock_ready = false;
static bool release_glitch_pin_ready = false;

volatile bool os_booted     = false;
//volatile bool lv2_booted    = false;
volatile bool gameos_booted = false;
volatile bool linux_booted  = false;

static bool set_reset_pico = false;
static uint32_t main_loop_runs = 0;

// UART0 buffer
char uartBuf[8192];

// UART1 buffer
static char sbBuf[256];
static size_t sbBufIdx = 0;

//static volatile uint32_t last_uart0_rx_ms = 0;
//static volatile uint32_t last_uart1_rx_ms = 0;

// Set when we see the special syscon reply; cleared once we declare a crash
//volatile bool     syscon_reply_flag = false;
  
//static uint32_t total_attempts = 0;
//static uint32_t total_successes = 0;
//static uint32_t total_failures = 0;

/////////////////////////////////////////////////////////////////////////////
// ——— led control ——————————————————————————————————————————————————————

typedef enum {
    LED_RED,
    LED_YELLOW,
    LED_GREEN,
    LED_BLUE
} led_t;

static const int _led_pins[] = {
    [LED_RED]    = error_led_pin,
    [LED_YELLOW] = yellow_led_pin,
    [LED_GREEN]  = green_led_pin,
    [LED_BLUE]   = blue_led_pin,
};

// Blink a single LED
// param led     which LED to blink (LED_RED, LED_YELLOW, LED_GREEN)
// param on_ms   milliseconds the LED stays on each cycle
// param off_ms  milliseconds the LED stays off each cycle
// param count   how many on/off cycles to perform
void blink_led(led_t led, uint32_t on_ms, uint32_t off_ms, uint32_t count) {
    int pin = _led_pins[led];
    for (uint32_t i = 0; i < count; ++i) {
        gpio_put(pin, 1);
        sleep_ms(on_ms);
        gpio_put(pin, 0);
        sleep_ms(off_ms);
    }
}

// Blink multiple LEDs in unison
// param leds      array of LEDs to blink (LED_RED, LED_YELLOW, LED_GREEN, LED_BLUE)
// param num_leds  number of entries in the `leds` array
// param on_ms     milliseconds to keep them on each cycle
// param off_ms    milliseconds to keep them off each cycle
// param count     how many on/off cycles to perform
void blink_leds(const led_t leds[], size_t num_leds,
                uint32_t on_ms, uint32_t off_ms, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        // Turn all specified LEDs on
        for (size_t j = 0; j < num_leds; ++j) {
            gpio_put(_led_pins[leds[j]], 1);
        }
        sleep_ms(on_ms);
        // Turn them all off
        for (size_t j = 0; j < num_leds; ++j) {
            gpio_put(_led_pins[leds[j]], 0);
        }
        sleep_ms(off_ms);
    }
}

// Chase sequence: one LED at a time forward and back.
// param on_ms   ms each LED stays on
// param off_ms  ms between LEDs
// param cycles  how many full back-and-forth passes
void chase_leds(uint32_t on_ms, uint32_t off_ms, uint32_t cycles) {
    // Order: red → yellow → green
    const led_t seq[] = { LED_RED, LED_YELLOW, LED_GREEN, LED_BLUE };
    const size_t len = sizeof(seq) / sizeof(seq[0]);

    for (uint32_t c = 0; c < cycles; ++c) {
        // forward
        for (size_t i = 0; i < len; ++i) {
            int pin = _led_pins[seq[i]];
            gpio_put(pin, 1);
            sleep_ms(on_ms);
            gpio_put(pin, 0);
            sleep_ms(off_ms);
        }
        // backward (skip ends to avoid double-flash)
        for (int i = (int)len - 2; i > 0; --i) {
            int pin = _led_pins[seq[i]];
            gpio_put(pin, 1);
            sleep_ms(on_ms);
            gpio_put(pin, 0);
            sleep_ms(off_ms);
        }
    }
}

// Random-blink: pick a random LED each cycle.
// param on_ms   ms LED stays on
// param off_ms  ms off before next
// param count   total random blinks
void random_blink_leds(uint32_t on_ms, uint32_t off_ms, uint32_t count) {
    const led_t all[] = { LED_RED, LED_YELLOW, LED_GREEN, LED_BLUE };
    const size_t n = sizeof(all) / sizeof(all[0]);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = get_rand_32() % n;
        int pin = _led_pins[all[idx]];
        gpio_put(pin, 1);
        sleep_ms(on_ms);
        gpio_put(pin, 0);
        sleep_ms(off_ms);
    }
}

/////////////////////////////////////////////////////////////////////////////
// ——— helpers ——————————————————————————————————————————————————————

static inline void WaitInUs(uint32_t us) {
    busy_wait_us(us);
}

static inline uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static inline void reset_pico(void) {
    watchdog_enable(1, true); // 1ms, timeout true
    while(1); // wait for reset
}

static bool string_contains_any(const char *array, const char * const msgs[]) {
    for (const char * const *p = msgs; *p; ++p) {
        if (strstr(array, *p)) {
            return true;
        }
    }
    return false;
}

static bool string_contains_all(const char *array, const char * const msgs[]) {
    for (const char * const *p = msgs; *p; ++p) {
        if (!strstr(array, *p)) {
            return false;
        }
    }
    return true;
}

/*
static void log_printf(const char *fmt, ...) {
    char _buf[128];
    uint32_t _t = to_ms_since_boot(get_absolute_time());
    int _n = snprintf(_buf, sizeof(_buf), "[%08u] ", _t);
    va_list _ap;
    va_start(_ap, fmt);
    vsnprintf(_buf + _n, sizeof(_buf) - _n, fmt, _ap);
    va_end(_ap);
    UartPrint(_buf);
}
*/

#if UART_ENABLED

static void log_printf(const char *fmt, ...) {
    char _buf[160];
    // get ms since boot
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    uint32_t s  = ms / 1000;
    uint32_t hh = (s / 3600) % 24;
    uint32_t mm = (s / 60) % 60;
    uint32_t ss = s % 60;
    uint32_t mmm = ms % 1000;

    // write the time prefix
    int ofs = snprintf(_buf, sizeof(_buf), "[%02u:%02u:%02u.%03u] ",
                       hh, mm, ss, mmm);

    // append the user message
    va_list ap;
    va_start(ap, fmt);
    //vsnprintf(_buf + ofs, sizeof(_buf) - ofs, fmt, ap);
    int n = vsnprintf(_buf + ofs, sizeof(_buf) - ofs - 2, fmt, ap);
    va_end(ap);
    // append CRLF
    int end = ofs + (n < 0 ? 0 : n);
    _buf[end++] = '\r';
    _buf[end++] = '\n';
    _buf[end]   = '\0';

    UartPrint(_buf);
}

static void log_plain(const char *fmt, ...) {
    char _buf[160];
    va_list ap;
    va_start(ap, fmt);
    // leave room for CR+LF and NUL
    int n = vsnprintf(_buf, sizeof(_buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    // append CRLF
    int end = n;
    _buf[end++] = '\r';
    _buf[end++] = '\n';
    _buf[end]   = '\0';
    UartPrint(_buf);
}

#else

// When UART isn’t enabled, make log_printf a no-op.
#define log_printf(...) ((void)0)
#define log_plain(...) ((void)0)

#endif  // UART_ENABLED

/*
// Pulse the PS3 power-button until PSU standby line goes high.
// Blocks until gpio_get(standby_mon_pin_id) returns true.
static void retry_power_on(void) {
    while (!gpio_get(standby_mon_pin_id)) {
        sleep_ms(2000);
        // pulse ~0.5s
        gpio_set_dir(pwr_on_pin_id, GPIO_OUT);
        gpio_put(pwr_on_pin_id, 0);
        sleep_ms(500);
        gpio_set_dir(pwr_on_pin_id, GPIO_IN);
        // allow PSU to spin up a bit
        sleep_ms(3000);
    }
}
*/

/*#if HDD_ACTIVITY_MONITOR
// GPIO interrupt callback for HDD activity
static void gpio_irq_handler(uint gpio, uint32_t events) {
    if (gpio == hdd_activity_pin && (events & GPIO_IRQ_EDGE_RISE)) {
        hdd_activity = true;
        log_printf("** HDD activity detected");  // log when disk activity is first seen
    }
}
#endif*/

static void print_pin_status(void) {
    bool standby = gpio_get(standby_mon_pin_id);
    bool pwr_on  = gpio_get(pwr_on_pin_id);        // only valid when in GPIO_IN
    bool hdd     = false;
/*#if HDD_ACTIVITY_MONITOR
    hdd = gpio_get(hdd_activity_pin);
#endif*/

    log_printf(
        "STATUS: do_glitch=%d, error_detect=%d, os_booted=%d, glitch_started=%d\r\nstandby=%d, pwr_on=%d, HDD_activity=%d\r\n",
        do_glitch, error_detect, os_booted, glitch_started,
        standby,  pwr_on,  hdd
    );
}

/*static void print_success_rate(void) {
    uint32_t pct = 0;
    if (total_attempts == 0) {
        log_printf("Stats: no attempts yet\n");
    } else {
        uint32_t sp = (total_successes * 100) / total_attempts;
        uint32_t fp = (total_failures  * 100) / total_attempts;
        log_printf("Stats: atts=%u, succ=%u (%u%%), fail=%u (%u%%)\n",
                   total_attempts, total_successes, sp,
                   total_failures,  fp);
    }
}*/

/*static inline int Uart0GetChar(void) {
    if (uart_is_readable(UART_ID)) {
        return uart_getc(UART_ID);
    }
    return -1;
}*/

// Return the Pico’s current VREG setting in centi-volts
// (e.g. 130 → 1.30 V, 150 → 1.50 V, etc.)
/*static inline uint16_t get_voltage_cv(void) {
    // these enum values correspond 0–7, not mV directly
    static const uint16_t centivolts[] = {
        [VREG_VOLTAGE_0_75] =  75,
        [VREG_VOLTAGE_1_10] = 110,
        [VREG_VOLTAGE_1_20] = 120,
        [VREG_VOLTAGE_1_25] = 125,
        [VREG_VOLTAGE_1_30] = 130,
        [VREG_VOLTAGE_1_35] = 135,
        [VREG_VOLTAGE_1_40] = 140,
        [VREG_VOLTAGE_1_45] = 145,
    };
    enum vreg_voltage v = vreg_get_voltage();
    return centivolts[v];
}*/



/////////////////////////////////////////////////////////////////////////////
// ——— UART0 init ——————————————————————————————————————————————————

void UartInit(void) {
    uart_init(UART_ID, 2400);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    uart_set_baudrate(UART_ID, BAUD_RATE);
    uart_set_hw_flow(UART_ID, false, false);
    uart_set_format(UART_ID, DATA_BITS, STOP_BITS, PARITY);
    uart_set_fifo_enabled(UART_ID, false);
    uart0_ready = true;
}

// check for uart0 status and active psu standby to call a reboot
/*static void check_uart0_activity(void) {
    // only care if we’ve already started glitching
    if (!glitch_started) return;
    // only if PSU standby is still high (PS3 hasn’t powered off)
    if (!gpio_get(standby_mon_pin_id)) return;

    // poll UART0 for any received bytes and bump timestamp if we get one
    bool saw_byte = false;
    int c;
    while ((c = Uart0GetChar()) >= 0) {
        saw_byte = true;
    }
    if (saw_byte) {
        last_uart0_rx_ms = to_ms_since_boot(get_absolute_time());
    }

    // if it’s been 10 s of silence, trigger reset
    uint32_t now     = to_ms_since_boot(get_absolute_time());
    uint32_t silent  = now - last_uart0_rx_ms;
    if (silent < 10000) return;

    log_printf("!! UART0 silent %u ms & standby high — forcing full reset", silent);

    reset_ps3_sequence();

    // restart silence timer so we don’t immediately retrigger
    last_uart0_rx_ms = to_ms_since_boot(get_absolute_time());
}*/

/////////////////////////////////////////////////////////////////////////////
// ——— SB_UART (UART1) RX interrupt handler ——————————————————————————

void on_uart1_rx(void) {
    while (uart_is_readable(uart1)) {
        char c = uart_getc(uart1);
        sbBuf[sbBufIdx++] = c;
        if (sbBufIdx >= sizeof(sbBuf) - 1) {
            memmove(sbBuf, sbBuf + 1, sizeof(sbBuf) - 1);
            sbBufIdx = sizeof(sbBuf) - 1;
        }
        sbBuf[sbBufIdx] = '\0';

        // LV2 and other critical errors
        // doesnt catch this [ERROR]: nv_storage::write
        static const char * const error_patterns[] = {
            "Lv2 internal error",
            "Lv2 panic",
            "Lv-2 detected an interrupt",
            "Stack trace",
            "[ERROR]:",
            "[EH] ioif event",
            "storage fault handler",
            "lv2(2): # system software version",
            NULL
        };

        if (!error_detect && string_contains_any(sbBuf, error_patterns)) {
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! [CRITICAL] Lv2 error/panic detected");
        }
        
        /*if (!error_detect && (strstr(sbBuf, "Lv2 internal error")
        || strstr(sbBuf, "Lv2 panic")
        || strstr(sbBuf, "Lv-2 detected an interrupt")
        || strstr(sbBuf, "Stack trace")
        || strstr(sbBuf, "[ERROR]:")
        || strstr(sbBuf, "[EH] ioif event")
        || strstr(sbBuf, "storage fault handler")
        || strstr(sbBuf, "lv2(2): # system software version"))) {
            /*if (!do_glitch) {
                error_detect = true;
                gpio_put(error_led_pin, 1);
                log_printf("!! [CRITICAL] Lv2 error/panic detected");
            }
            else {
                log_printf("!! [CRITICAL] Lv2 error/panic detected, but glitch is still running. Skipping error trigger.");
            }
            
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! [CRITICAL] Lv2 error/panic detected");
        }*/
        
        /*if (strstr(sbBuf, "Lv2 internal error")
        || strstr(sbBuf, "Lv2 panic")
        || strstr(sbBuf, "Lv-2 detected an interrupt")
        || strstr(sbBuf, "Stack trace")
        || strstr(sbBuf, "[ERROR]:")
        || strstr(sbBuf, "[EH] ioif event")
        || strstr(sbBuf, "storage fault handler")) {
            if (!error_detect) {
                error_detect = true;
                gpio_put(error_led_pin, 1);
                log_printf("!! [CRITICAL] Lv2 error/panic detected"); }
        }*/
        /*if (!error_detect && strstr(sbBuf, "Lv2 internal error)) {
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! Lv2 internal error detected");
        }
        if (!error_detect && strstr(sbBuf, "Lv2 panic")) {
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! Lv2 panic detected");
        }
        if (!error_detect && strstr(sbBuf, "Lv-2 detected an interrupt")) {
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! Lv-2 detected an interrupt exception");
        }
        if (!error_detect && strstr(sbBuf, "Stack trace")) {
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! Stack trace detected");
        }
        if (!error_detect && strstr(sbBuf, "[ERROR]:")) {
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! [Error]: detected");
        }
        if (!error_detect && strstr(sbBuf, "[EH] ioif event")) {
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! [EH] ioif event detected");
        }
        /*if (!error_detect && strstr(sbBuf, "storage fault handler") {
            error_detect = true;
            gpio_put(error_led_pin, 1);
            log_printf("!! storage fault handler detected");
        }*/
        if (!green_detect && strstr(sbBuf, "Hello from lv1_puts")) {
            glitch_success = true;
            green_detect = true;
            gpio_put(green_led_pin, 1);
            log_printf("** Glitch is successful. lv1_test_puts() done.");
            //set_reset_pico = true;
        }
        // TODO: fix the string to something that is accurate
        /*if (!green_detect && strstr(sbBuf, "lv1_peek/poke now available")) {
            green_detect = true;
            gpio_put(green_led_pin, 1);
            log_printf("** lv1_peek/poke now available.");
        }*/
        if (!blue_detect && strstr(sbBuf, "lparmgr : boot completed")) {
            blue_detect = true;
            gpio_put(blue_led_pin, 1);
            log_printf("** LV2 Kernel Booted");
        }
        if (!gameos_booted && (strstr(sbBuf, "Cell OS Lv-2 32 bit version"))) {
            gameos_booted = true;
            blue_detect = true;
            gpio_put(blue_led_pin, 1);
            log_printf("** GameOS booted");
        }
        if (!os_booted && strstr(sbBuf, "initial system process done")) {
            os_booted = true;
            blue_detect = true;
            gpio_put(blue_led_pin, 1);
            log_printf("** OS boot complete");
        }
        // Linux/Custom LV2 Kernel
        static const char * const lparmgr_full_sequence[] = {
            "lv2(2): Prepare to shutdown lpar_event",
            "lparmgr : send shutdown command to PS3_LPAR",
            "lparmgr : shutdown PS3_LPAR partition... done",
            "lparmgr : start destructing partition.",
            "lparmgr : unload guestos... done",
            "lparmgr : destructing partition... done",
            "lparmgr : booting PS3_LPAR partition...",
            "lparmgr : load guestos... done",
            "lparmgr : boot completed",
            NULL
        };
        if (!gameos_booted && string_contains_all(sbBuf, lparmgr_full_sequence)) {
            linux_booted = true;
            blue_detect = true;
            green_detect = true;
            gpio_put(blue_led_pin, 1);
            gpio_put(green_led_pin, 1);
            log_printf("** Full LPAR shutdown→boot sequence detected");
            sleep_ms(500);
            log_printf("** Linux/Custom LV2 booted");
        }
        
        /*if (glitch_started && strstr(sbBuf, "timer: set alarm")) {
            set_alarm = true;
            log_printf("!! set alarm triggered");
        }*/
        /*if (glitch_started && !syscon_reply_flag && strstr(sbBuf, "from syscon (reply=1620)"))
        {
            syscon_reply_flag = true;
            log_printf("** syscon reply=1620 seen");
        }*/
    }
}

// init uart1 for ps3 sb_uart input
void SbUartInit(void) {
    uart_init(uart1, 115200);
    gpio_set_function(sb_uart_rx_pin, GPIO_FUNC_UART);
    irq_set_exclusive_handler(UART1_IRQ, on_uart1_rx);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irq_enables(uart1, true, false);
    uart1_ready = true;
}

// check for uart1 status and active psu standby to call a reboot
/*static void check_uart1_activity(void) {
    // only care during a glitch session, with PSU still on
    if (!glitch_started) return;
    if (!gpio_get(standby_mon_pin_id)) return;

    // how long since last char on UART1?
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t silence = now - last_uart1_rx_ms;
    if (silence < 10000) return;

    // it's been ≥10 s with no UART1 activity → assume hung PS3
    log_printf("!! UART1 silent %u ms & standby high — forcing full reset", silence);

    reset_ps3_sequence();

    // reset the timer so we won't keep triggering every loop
    last_uart1_rx_ms = now;
}*/

/////////////////////////////////////////////////////////////////////////////
// ——— USB endpoint handlers ——————————————————————————————————————

void ep1_out_handler(uint8_t *buf, uint16_t len) {

    if (error_detect) {
        do_glitch   = false;
        is_stopped  = true;
        log_printf("ep1_out_handler: error_detect is true → ignoring USB commands");
        return;
    }
            
    uint8_t v = buf[0], response = 0;
    usb_start_transfer(usb_get_endpoint(EP1_OUT_ADDR), NULL, 64);
    if (v == 0x44 || v == 0x55) {
        do_glitch  = (v == 0x44);
        is_stopped = (v == 0x44);
        //log_printf("ep1_out_handler: USB cmd 0x%02X → do_glitch=%d\n", v, do_glitch);
        if (v == 0x44) {
            // start of a new glitch session
            log_printf("ep1_out_handler: 0x44 -> Glitch Started");
            glitch_started = true;
            sbBufIdx = 0;
        }
        response = (v == 0x44) ? 0x11 : 0x22;
        uint8_t newbuf[64] = { response };
        usb_start_transfer(usb_get_endpoint(EP2_IN_ADDR), newbuf, 64);
    #if GLITCH_CORE_ENABLED
        //log_printf("ep1_out_handler: GLITCH_CORE_ENABLED\n");
        if (v == 0x55) {
            while (!is_stopped) {}
            //log_printf("ep1_out_handler: 0x55 -> Glitch Stopped");
        }
    #endif
    }
}

void ep2_in_handler(uint8_t *buf, uint16_t len) {
    //log_printf("ep2_in_handler");
}

/////////////////////////////////////////////////////////////////////////////
// ——— glitch core ——————————————————————————————————————————————————

// Original logic with changes
void glitch_core(void) {
    //bool shuffle = false;
    uint32_t randValue = get_rand_32();
    bool shuffle = (randValue % 2) == 1;
    
    log_printf("glitch_core loaded [randValue: %d], [shuffle: %d]", randValue, shuffle);
    
    while (1) {
    
        // use fixed base delay
        if (do_glitch) {
            WaitInUs(500);
        }
        
        //const uint32_t CYCLE_NS = 4;// 4 ns @ 250 MHz
        //const uint32_t MIN_JITTER_CYCLES = 0;
        //const uint32_t MAX_JITTER_CYCLES = 40 / CYCLE_NS;// 10
    
        // use jitter cycles and fixed base delay
        /*if (do_glitch) {
            // pick a random # of cycles from 0..MAX_JITTER_CYCLES-1
            uint32_t jitter_cycles = get_rand_32() % MAX_JITTER_CYCLES;
            // burn exactly that many cycles (≈4 ns each)
            spin_loop_count(jitter_cycles);
            // log it so you can watch the effect
            log_printf("glitch_core: initial jitter %u cycles (~%u ns)",
                       jitter_cycles, jitter_cycles * CYCLE_NS);
            // now the fixed 500 µs delay
            WaitInUs(500);
        }*/
        
        // use jitter cycles and random base delay
        /*if (do_glitch) {
            // jitter: 0-MAX_JITTER_CYCLES-1 nops
            uint32_t jitter_cycles = get_rand_32() % MAX_JITTER_CYCLES;
            for (uint32_t i = MIN_JITTER_CYCLES; i < jitter_cycles; ++i) {
                __asm volatile("nop");
            }
            //log_printf("glitch_core: jitter %u cycles (~%u ns)", jitter_cycles, jitter_cycles * CYCLE_NS);

            // random base delay
            //uint32_t base_delay_us = (get_rand_32() % 951) + 50;// 50 - 1000us
            //uint32_t base_delay_us = (get_rand_32() % 501) + 150;// 150 - 650us
            //uint32_t base_delay_us = (get_rand_32() % 401) + 150;// 150 - 550us
            //uint32_t base_delay_us = (get_rand_32() % 301) + 250;// 250 - 550us
            //uint32_t base_delay_us = (get_rand_32() % 501) + 0;// 0 - 500us
            uint32_t base_delay_us = (get_rand_32() % 451) + 50;// 50 - 500us
            //log_printf("glitch_core: base delay %u μs", base_delay_us);
            WaitInUs(base_delay_us);
        }*/
        
        // use jitters with successfull glitches 193us
        /*if (do_glitch) {
            // 4 ns per cycle @ 250 MHz
            const uint32_t CYCLE_NS = 4;
            
            // fix jitter to the “winning” 8 cycles → ~32 ns
            const uint32_t jitter_cycles = 8;
            
            // burn exactly 8 cycles
            for (uint32_t i = 0; i < jitter_cycles; ++i) {__asm volatile("nop");}
            log_printf("glitch_core: jitter %u cycles (~%u ns)\n", jitter_cycles, jitter_cycles * CYCLE_NS);

             // now the fixed 193 µs delay
             WaitInUs(193);
         }*/
        
        /*if (do_glitch) {
            // bias jitter into the 24–36 ns band (cycles 6…9)
            const uint32_t MIN_JITTER = 6;
            const uint32_t MAX_JITTER = MAX_JITTER_CYCLES; // still equals 10
            uint32_t jitter_cycles = MIN_JITTER + (get_rand_32() % (MAX_JITTER - MIN_JITTER));
            for (uint32_t i = 0; i < jitter_cycles; ++i) {
                __asm volatile("nop");
            }
            log_printf("glitch_core: jitter %u cycles (~%u ns)",
            jitter_cycles, jitter_cycles * CYCLE_NS);
            uint32_t base_delay_us = 150 + (get_rand_32() % 101); // 150–250 μs
            log_printf("glitch_core: base delay %u μs", base_delay_us);
            WaitInUs(base_delay_us);
        }*/
        
        /*if (do_glitch) {
            // --- JITTER: skewed toward higher values (near 8–9 cycles) ---
            // two independent 0..MAX_JITTER_CYCLES-1 draws:
            uint32_t j1 = get_rand_32() % MAX_JITTER_CYCLES;
            uint32_t j2 = get_rand_32() % MAX_JITTER_CYCLES;
            // pick the larger one → more probability at the top of the range
            uint32_t jitter_cycles = (j1 > j2 ? j1 : j2);
            // but never drop to zero
            if (jitter_cycles < 1) jitter_cycles = 1;
            // burn ~4 ns per cycle
            for (uint32_t i = 0; i < jitter_cycles; ++i) {
                __asm volatile("nop");
            }
            log_printf("glitch_core: jitter %u cycles (~%u ns)",
            jitter_cycles, jitter_cycles * CYCLE_NS);

            // --- BASE DELAY: uniform 150–190 µs ---
            // 190 - 150 + 1 = 41 possible values
            uint32_t base_delay_us = 150 + (get_rand_32() % 41);
            log_printf("glitch_core: base delay %u μs", base_delay_us);
            WaitInUs(base_delay_us);
        }*/
        
        while (do_glitch) {
            if (error_detect) {
                do_glitch = false;
                glitch_error = true;
                break;
            }
            
            if (glitch_error) {
                log_printf("glitch_core: stopping glitch (error_detected)");
            }
            
            uint32_t wait_us = (get_rand_32() % 1000) + 1000;
            WaitInUs(wait_us);

        #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
            irq_set_enabled(USBCTRL_IRQ, false);
        #endif

        #if PULLDOWN1_ENABLED
        #if PULLDOWN1_ENABLED && PULLDOWN2_ENABLED && SHUFFLE_ENABLED
            if (!shuffle)
        #endif
            {
            #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, false);
            #endif
                //log_printf("pulse PD1 (GPIO %d)", pulldown1_pin_id);
                gpio_set_dir(pulldown1_pin_id, GPIO_OUT);
                gpio_put(pulldown1_pin_id, false);
                gpio_set_function(pulldown1_pin_id, GPIO_FUNC_SIO);
                io_bank0_hw->io[pulldown1_pin_id].ctrl = GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
            #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, true);
            #endif
            }
        #endif

        #if PULLDOWN2_ENABLED
        #if PULLDOWN1_ENABLED && PULLDOWN2_ENABLED && SHUFFLE_ENABLED
            if (shuffle)
        #endif
            {
            #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, false);
            #endif
                //log_printf("pulse PD2 (GPIO %d)", pulldown2_pin_id);
                gpio_set_dir(pulldown2_pin_id, GPIO_OUT);
                gpio_put(pulldown2_pin_id, false);
                gpio_set_function(pulldown2_pin_id, GPIO_FUNC_SIO);
                io_bank0_hw->io[pulldown2_pin_id].ctrl = GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
                
            #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, true);
            #endif
            }
        #endif

            shuffle = !shuffle;
        }
        if (!is_stopped) {
            is_stopped = true;
            __dsb();
        }
    }
}

/*
// Original logic
void glitch_core(void) {
    uint32_t randValue = get_rand_32();
    bool shuffle = (randValue % 2) == 1;
    
    while (1) {
        if (do_glitch) {
            WaitInUs(500);
        }
        while (do_glitch) {
            uint32_t wait_us = (get_rand_32() % 1000) + 1000;
            WaitInUs(wait_us);

        #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
            irq_set_enabled(USBCTRL_IRQ, false);
        #endif

        #if PULLDOWN1_ENABLED
#if PULLDOWN1_ENABLED && PULLDOWN2_ENABLED && SHUFFLE_ENABLED
            if (!shuffle)
#endif
            {
#if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, false);
#endif
                // pull down for 40ns
                gpio_set_dir(pulldown1_pin_id, GPIO_OUT);
                gpio_put(pulldown1_pin_id, false);
                
                // pull down
                gpio_set_function(pulldown1_pin_id, GPIO_FUNC_SIO);
                
                // then we float it
                io_bank0_hw->io[pulldown1_pin_id].ctrl =
                    GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
#if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, true);
#endif
            }
#endif  // PULLDOWN1_ENABLED

#if PULLDOWN2_ENABLED
#if PULLDOWN1_ENABLED && PULLDOWN2_ENABLED && SHUFFLE_ENABLED
            if (shuffle)
#endif
            {
#if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, false);
#endif
                // pull down for 40ns
                gpio_set_dir(pulldown2_pin_id, GPIO_OUT);
                gpio_put(pulldown2_pin_id, false);
                
                // pull down
                gpio_set_function(pulldown2_pin_id, GPIO_FUNC_SIO);
                
                // then we float it
                io_bank0_hw->io[pulldown2_pin_id].ctrl =
                    GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
#if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, true);
#endif
            }
#endif  // PULLDOWN2_ENABLED

            shuffle = !shuffle;
            WaitInUs(1000);
        }
#else
        // updated glitch path
        if (do_glitch) {
            WaitInUs(500);
        }

        while (do_glitch)
        {
            uint32_t wait_us = (get_rand_32() % 1000) + 1000;
            WaitInUs(wait_us);

        #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
            irq_set_enabled(USBCTRL_IRQ, false);
        #endif

        #if PULLDOWN1_ENABLED
        #if PULLDOWN1_ENABLED && PULLDOWN2_ENABLED && SHUFFLE_ENABLED
            if (!shuffle)
        #endif
            {
            #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, false);
            #endif
                // pull down for 40ns
                gpio_set_dir(pulldown1_pin_id, GPIO_OUT);
                gpio_put(pulldown1_pin_id, false);
                
                // pull down
                gpio_set_function(pulldown1_pin_id, GPIO_FUNC_SIO);
                
                // then we float it
                io_bank0_hw->io[pulldown1_pin_id].ctrl =
                    GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
            #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, true);
            #endif
            }
        #endif  // PULLDOWN1_ENABLED

        #if PULLDOWN2_ENABLED
        #if PULLDOWN1_ENABLED && PULLDOWN2_ENABLED && SHUFFLE_ENABLED
            if (shuffle)
        #endif
            {
            #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, false);
            #endif
                // pull down for 40ns
                gpio_set_dir(pulldown2_pin_id, GPIO_OUT);
                gpio_put(pulldown2_pin_id, false);
                
                // pull down
                gpio_set_function(pulldown2_pin_id, GPIO_FUNC_SIO);
                
                // then we float it
                io_bank0_hw->io[pulldown2_pin_id].ctrl =
                    GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
            #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
                irq_set_enabled(USBCTRL_IRQ, true);
            #endif
            }
        #endif  // PULLDOWN2_ENABLED

            shuffle = !shuffle;
        }
#endif  // end of glitch path selection

        if (!is_stopped)
        {
            is_stopped = true;
            __dsb();
        }
    }
}
*/

/*
// One-wire setup
void glitch_core(void) {
    while (1) {
        while (do_glitch) {
            uint32_t wait_us = (get_rand_32() % 1000) + 1000;
            WaitInUs(wait_us);
        #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
            irq_set_enabled(USBCTRL_IRQ, false);
        #endif
            gpio_set_dir(pulldown_pin_id, GPIO_OUT);
            gpio_put(pulldown_pin_id, false);
            gpio_set_function(pulldown_pin_id, GPIO_FUNC_SIO);
            io_bank0_hw->io[pulldown_pin_id].ctrl =
                GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
        #if !GLITCH_CORE_ENABLED && !TEST_MODE_ENABLED
            irq_set_enabled(USBCTRL_IRQ, true);
        #endif
        }
        if (!is_stopped) {
            is_stopped = true;
            __dsb();
        }
    }
}
*/

/////////////////////////////////////////////////////////////////////////////
// ——— init ——————————————————————————————————————————————————————

void init_leds(void) {
    log_printf("init LEDs");
    
    // Red
    gpio_init(error_led_pin);
    gpio_set_dir(error_led_pin, GPIO_OUT);
    gpio_put(error_led_pin, 0);
    
    // Yellow
    gpio_init(yellow_led_pin);
    gpio_set_dir(yellow_led_pin, GPIO_OUT);
    gpio_put(yellow_led_pin, 0);
    
    // Green
    gpio_init(green_led_pin);
    gpio_set_dir(green_led_pin, GPIO_OUT);
    gpio_put(green_led_pin, 0);
    
    // Blue
    gpio_init(blue_led_pin);
    gpio_set_dir(blue_led_pin, GPIO_OUT);
    gpio_put(blue_led_pin, 0);
}

void init_power_button(void) {
    log_printf("init power button line");
    gpio_init(pwr_on_pin_id);
    gpio_set_function(pwr_on_pin_id, GPIO_FUNC_SIO);
    gpio_set_dir(pwr_on_pin_id, GPIO_IN);
}

void init_psu_standby(void) {
    log_printf("init PSU standby monitor");
    gpio_init(standby_mon_pin_id);
    gpio_set_function(standby_mon_pin_id, GPIO_FUNC_SIO);
    gpio_set_dir(standby_mon_pin_id, GPIO_IN);
    gpio_pull_down(standby_mon_pin_id);
}

void reset_ps3_sequence(void) {
    // turn LEDs on
    //log_printf("turn LEDs on\n");
    gpio_put(error_led_pin,  1);
    gpio_put(yellow_led_pin, 1);

    // hold power-button low for 15s (force off)
    log_printf("hold power button low for 15s (force off)");
    gpio_set_dir(pwr_on_pin_id, GPIO_OUT);
    gpio_put(pwr_on_pin_id, 0);
    sleep_ms(15000);

    // turn LEDs off
    //log_printf(">> turn LEDs off\n");
    gpio_put(error_led_pin,  0);
    gpio_put(yellow_led_pin, 0);

    // release button
    log_printf("release button");
    gpio_set_dir(pwr_on_pin_id, GPIO_IN);
    //io_bank0_hw->io[pwr_on_pin_id].ctrl = GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
    //gpio_set_pulls(pwr_on_pin_id, false, false);
    sleep_ms(6000);

    // initial short press to turn on
    log_printf("initial short press to turn on");
    gpio_set_dir(pwr_on_pin_id, GPIO_OUT);
    gpio_put(pwr_on_pin_id, 0);
    sleep_ms(800);
    gpio_set_dir(pwr_on_pin_id, GPIO_IN);
    //io_bank0_hw->io[pwr_on_pin_id].ctrl = GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
    //gpio_set_pulls(pwr_on_pin_id, false, false);
}

static void power_on_ps3(void) {
    log_printf("power on ps3");
    gpio_set_dir(pwr_on_pin_id, GPIO_OUT);
    gpio_put(pwr_on_pin_id, 0);
    sleep_ms(800);
    gpio_set_dir(pwr_on_pin_id, GPIO_IN);
}

/*static void retry_power_on(void) {
    // keep looping until os_booted becomes true
    while (!os_booted) {
    
        const led_t combo[] = {LED_RED, LED_YELLOW};
        blink_leds(combo, 2, 200, 200, 5);
        
        // if the PS3 is off, turn it back on
        if (!gpio_get(standby_mon_pin_id)) {
            // pulse the power button for ~0.5s
            gpio_set_dir(pwr_on_pin_id, GPIO_OUT);
            gpio_put(pwr_on_pin_id, 0);
            sleep_ms(500);
            gpio_set_dir(pwr_on_pin_id, GPIO_IN);
            //io_bank0_hw->io[pwr_on_pin_id].ctrl = GPIO_FUNC_NULL << IO_BANK0_GPIO0_CTRL_FUNCSEL_LSB;
            //gpio_set_pulls(pwr_on_pin_id, false, false);
            
            sleep_ms(3000);
        }
        // wait a bit before checking again
        sleep_ms(1000);
    }
}*/

static void retry_power_on(void) {
    const led_t combo[] = {LED_RED, LED_YELLOW};
    blink_leds(combo, 2, 200, 200, 5);
        
    gpio_set_dir(pwr_on_pin_id, GPIO_OUT);
    gpio_put(pwr_on_pin_id, 0);
    sleep_ms(500);
    gpio_set_dir(pwr_on_pin_id, GPIO_IN);
    sleep_ms(5000);
}

/*#if HDD_ACTIVITY_MONITOR
void init_hdd_activity(void) {
    log_printf("init HDD activity monitor");
    gpio_init(hdd_activity_pin);
    gpio_set_function(hdd_activity_pin, GPIO_FUNC_SIO);
    gpio_set_dir(hdd_activity_pin, GPIO_IN);
    gpio_pull_down(hdd_activity_pin);  // ensure stable low when LED is off
    
    // Enable interrupt on HDD LED rising edge
    gpio_set_irq_enabled_with_callback(
        hdd_activity_pin, GPIO_IRQ_EDGE_RISE, true, &gpio_irq_handler);
}
#else
#define init_hdd_activity() ((void)0)
#endif*/

void clear_leds(void) {
    log_printf("clear LEDs");
    gpio_put(error_led_pin, 0);
    gpio_put(yellow_led_pin, 0);
    gpio_put(green_led_pin, 0);
    gpio_put(blue_led_pin, 0);
}

void clear_all_flags() {
    log_printf("clear flags for next session");
    error_detect    = false;
    yellow_detect   = false;
    green_detect    = false;
    blue_detect     = false;
    glitch_started  = false;
    os_booted       = false;
    linux_booted    = false;
    is_stopped      = false;
    do_glitch       = false;
    glitch_error    = false;
    uart0_ready = false;
    uart1_ready = false;
    set_voltage_ready = false;
    set_sys_clock_ready = false;
    release_glitch_pin_ready = false;
    hdd_activity = false;
}

static void show_all_flags(void) {
    log_printf("=== Flag Status ===");
    log_printf("uart0_ready=%d, uart1_ready=%d",
               uart0_ready, uart1_ready);
    log_printf("do_glitch=%d, is_stopped=%d, glitch_started=%d, glitch_error=%d",
               do_glitch, is_stopped, glitch_started, glitch_error);
    log_printf("error_detect=%d, yellow_detect=%d, green_detect=%d, blue_detect=%d",
               error_detect, yellow_detect, green_detect, blue_detect);
    log_printf("set_voltage_ready=%d, set_sys_clock_ready=%d, release_glitch_pin_ready=%d",
               set_voltage_ready, set_sys_clock_ready, release_glitch_pin_ready);
    log_printf("set_alarm=%d, set_reset_pico=%d, hdd_activity=%d",
               set_alarm, set_reset_pico, hdd_activity);
    log_printf("os_booted=%d, gameos_booted=%d, linux_booted=%d",
               os_booted, gameos_booted, linux_booted);
    log_printf("=====================");
}

static void show_gpio_pinout(void) {
    log_plain("=====================");
    log_plain("PS3 Resistor Connections");
    log_plain("pulldown1_pin_id (RQ7) -> 15");
    log_plain("pulldown2_pin_id (RQ8) -> 16\r\n");
    log_plain("pwr_on_pin_id (PS3 ribbon connector 3.3v) -> 10");
    log_plain("sb_uart_rx_pin (PS3 SB_TX) -> 5");
    log_plain("standby_mon_pin_id (PSU Standby Pin 3) -> 18");
    log_plain("hdd_activity_pin (PS3 HDD LED Anode) -> 22\r\n");
    log_plain("error_led_pin (Red) -> 6");
    log_plain("yellow_led_pin (Yellow) -> 2");
    log_plain("green_led_pin (Green) -> 21");
    log_plain("blue_led_pin (Blue) -> 27");
    log_plain("=====================\r\n");
}

void main(void) {
    
    // init power
    vreg_set_voltage(VREG_VOLTAGE_1_30);// !!DO NOT CHANGE!!
    set_voltage_ready = true;
    
    sleep_ms(200);
    
    // set clock
    uint32_t sys_khz = 250000;
    set_sys_clock_khz(sys_khz, true);
    set_sys_clock_ready = true;

    // release glitch pin
    //gpio_deinit(pulldown_pin_id);// one-wire setup
    gpio_deinit(pulldown1_pin_id);
    gpio_deinit(pulldown2_pin_id);
    release_glitch_pin_ready = true;

    #if UART_ENABLED
        UartInit();
    #endif
    
    // from ps3 sb_uart_tx
    SbUartInit();

    #if !TEST_MODE_ENABLED
        usb_init();
        sleep_ms(50);
    #endif

    #if UART_ENABLED
        //uint32_t get_clock = clock_get_hz(clk_sys) / 1000;
        log_plain("\r\n\r\nHello from Pico BadHTAB Glitcher ;)");
        log_plain("### ALL YOUR PS3 ARE BELONG TO US ###");
        log_plain("\r\nOriginal pico code by Kafuu (aomsin2526), modified by esc0rtd3w\r\n");
        log_plain("*** MAKE SURE YOU ENABLE UART FROM PS3 SYSCON AND CONNECT SB_TX TO PICO GPIO5 ***");
        log_plain("Mullion SYSCONs starting with CXR713 = w 7202 02");
        log_plain("Mullion SYSCON CXR713120-203GB = w 4202 02");
        log_plain("Mullion SYSCONs starting with CXR714 = w 4202 02");
        log_plain("Sherwood SYSCON = w 1202 02\r\n");
        log_plain("*** DO NOT ENABLE THE EXTRA UART OUTPUTS OR GLITCH WILL CRASH WHEN RETURNING TO XMB ***\r\n");
        /*log_plain("*** DO NOT ENABLE THIS OR GLITCH WILL CRASH WHEN RETURNING TO XMB ***");
        log_plain("*** ONLY ENABLE THIS IF YOU ARE BOOTING CUSTOM LV2 KERNEL OR LINUX ***");
        log_plain("Mullion SYSCONs starting with CXR713 = w 72CF 03");
        log_plain("Mullion SYSCON CXR713120-203GB = w 42CF 03");
        log_plain("Mullion SYSCONs starting with CXR714 = w 42CF 03");
        log_plain("Sherwood SYSCON = w 12CF 03\r\n");*/
        log_plain("Thanks to geohot for discovering original dangling HTAB glitch");
        log_plain("Thanks to Kafuu (aomsin2526) for BadHTAB exploit -> https://github.com/aomsin2526/BadHTAB");
        log_plain("Thanks to RIP-Felix for SYSCON tutorial -> https://www.psx-place.com/threads/syscon-tutorial-windows.41664/\r\n");
        
        //print_success_rate();
        //show_gpio_pinout();
        
        if (uart0_ready) { log_printf("UART0 ready: pico [output]"); }
        if (uart1_ready) { log_printf("UART1 ready: ps3 sb_uart [input]"); }
        //if (set_voltage_ready) { log_printf("set pico voltage: %u.%02uv", vreg_get_voltage() / 100, vreg_get_voltage() % 100); }
        if (set_voltage_ready) { log_printf("set pico voltage: 1.3v"); }
        if (set_sys_clock_ready) { log_printf("set system clock: %u khz", sys_khz); }
        if (release_glitch_pin_ready) { log_printf("release glitch pin");}
        
        //last_uart0_rx_ms = to_ms_since_boot(get_absolute_time());
        //last_uart1_rx_ms = to_ms_since_boot(get_absolute_time());
    #endif

    // init LEDs
    init_leds();

    // init power-button line (idle = input)
    init_power_button();

    // init PSU standby monitor
    init_psu_standby();
    
    /*#if HDD_ACTIVITY_MONITOR
        // init hdd activity monitor
        init_hdd_activity();
    #else
        log_printf("HDD monitor disabled");
    #endif*/

    // launch glitch core on core1
    log_printf("launch glitch core on core1");
    sleep_ms(1000);
    multicore_launch_core1(glitch_core);

    // core0: detect crash or hard power-down, then restart
    while (1) {
    
        // Detecting stuck in while loop, doing nothing
        //main_loop_runs++;
        if (++main_loop_runs > 10) {
            main_loop_runs = 0;  // reset counter

            bool psu_on = gpio_get(standby_mon_pin_id);
            if (psu_on) {
                // TODO: Check if stuck in a crashed state, but no uart output
                //log_printf("stuck in loop check: PSU standby HIGH → resetting Pico");
                //log_printf("stuck in loop check: PSU standby HIGH → resetting PS3");
                log_printf("PSU standby is ON. Skipping ps3 reset until state can be accurately determined");
                log_printf("You may have to manually power down the PS3 if it gets stuck");
                //reset_pico();
                //set_reset_pico = true;
                //reset_ps3_sequence();
            } else {
                log_printf("stuck in loop check: PSU standby LOW → powering on PS3");
                power_on_ps3();
                sleep_ms(5000);
                //reset_pico();
                set_reset_pico = true;
            }
        }
    
        //LED test for init and debugging if stuck in while loop
        const led_t ledtest[] = {LED_RED, LED_YELLOW, LED_GREEN, LED_BLUE};
        blink_leds(ledtest, 4, 200, 200, 2);
        sleep_ms(2000);
        
        show_all_flags();
        //print_pin_status();
        log_printf("main_loop_runs: %d", main_loop_runs);
    
        //log_printf("begin main while loop");
        //check_uart0_activity();
        //check_uart1_activity();
    
        /* 
        // If glitch is running, do nothing
        if (do_glitch) {
            sleep_ms(1);
            continue;
        }*/
    
        bool hard_crash = (glitch_started && gpio_get(standby_mon_pin_id) == 0);
        
        /*
        static uint32_t hang_ts = 0;
        #if HDD_ACTIVITY_MONITOR
        // if glitch started, no error, still powered, no boot, and no HDD activity
        if (glitch_started
            && !error_detect
            && !os_booted
            && gpio_get(standby_mon_pin_id))
        {
            if (hang_ts == 0) {
                hang_ts = now_ms();
            } else if (now_ms() - hang_ts > 10000
                       && !hdd_activity)
            {
                log_printf("!! Hang detected (no HDD) — forcing reset");
                reset_ps3_sequence();
                hdd_activity = false;
                hang_ts = now_ms();
            }
        } else {
            hang_ts = 0;
        }
        #else
        if (glitch_started
            && !error_detect
            && !os_booted
            && gpio_get(standby_mon_pin_id))
        {
            if (hang_ts == 0) {
                hang_ts = now_ms();
            } else if (now_ms() - hang_ts > 10000) {
                log_printf("!! Hang detected — forcing reset");
                reset_ps3_sequence();
                hang_ts = now_ms();
            }
        } else {
            hang_ts = 0;
        }
        #endif
        */
        
        /*if (glitch_started && os_booted && !do_glitch) {
            log_printf("** PS3 booted cleanly — resetting Pico session");
            clear_leds();
            clear_all_flags();
            continue;  // go back to top and wait for next 0x44
        }*/
        
        // If we’ve seen the syscon reply, but then no further UART1 activity for 5 sec
        /*if (do_glitch && glitch_started && syscon_reply_flag) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - last_uart1_rx_ms > 5000) {
                log_printf("!! UART quiet >5s after syscon reply — assuming crash");
                syscon_reply_flag = false;
                error_detect      = true;
            }
        }*/
        
        if (glitch_success) {
            set_reset_pico = true;
            break;
        }
        
        if (error_detect) {
            log_printf("Soft crash: beginning restart sequence");
            blink_led(LED_RED, 200, 200, 3);
            sleep_ms(3000);
        }
        
        if (hard_crash) {
            log_printf("Hard crash: beginning restart sequence");
            const led_t crash[] = {LED_RED, LED_YELLOW};
            blink_leds(crash, 2, 200, 200, 3);
            //chase_leds(100, 50, 10);
            //random_blink_leds(200, 100, 10);
            sleep_ms(3000);
        }
        
        if (error_detect || hard_crash) {
            
            //print_pin_status();
            
            // Start reset sequence on PS3
            reset_ps3_sequence();

            // give PS3 time to start up
            //log_printf("wait for PS3 to start up\n");
            sleep_ms(15000);
            
            // ensure PSU standby is high
            /*if (!gpio_get(standby_mon_pin_id)) {
                //log_printf("retry_power_on()\n");
                retry_power_on();
            }*/
            
            /*
            while (!os_booted) {
                if (!gpio_get(standby_mon_pin_id)) {
                    log_printf("!os_booted: standby low during boot—re-pulsing\n");
                    retry_power_on();
                }
                sleep_ms(1000);
            }
            log_printf("restart complete, OS should be running\n");
            */
            
            /*#if HDD_ACTIVITY_MONITOR
            hdd_activity = false;
            sleep_ms(15000);
            if (!os_booted) {
                if (gpio_get(standby_mon_pin_id) && !hdd_activity) {
                    // PS3 remained powered on, but no HDD activity -> likely hung
                    log_printf("!! No HDD activity – boot likely failed");
                    reset_ps3_sequence();// force another full reset cycle
                    hdd_activity = false;  
                    sleep_ms(15000); // give PS3 time after the second reset
                }
            }
            #endif*/
            
            // enter loop to wait for os_booted or handle standby-off case
            while (!os_booted) {
                if (!gpio_get(standby_mon_pin_id)) {
                    log_printf("** standby off, os not booted -> retry power on");
                    retry_power_on();
                }
                sleep_ms(1000);
            }
            while (!gpio_get(standby_mon_pin_id)) {
                log_printf("** standby off -> retry power on");
                retry_power_on();
                sleep_ms(1000);
            }
            
            if (os_booted && gameos_booted) {
                blink_led(LED_BLUE, 200, 200, 5);
                log_printf("** restart complete. OS should be running -> Enable HEN and launch BadHTAB EBOOT");
            }
            else {
                const led_t osvf[] = {LED_BLUE, LED_RED};
                blink_leds(osvf, 2, 200, 200, 5);
                log_printf("** restart complete. OS cannot be verified running");
            }

            // final stabilization
            sleep_ms(2000);
            
            // turn off all leds
            clear_leds();

            // clear flags for next session
            clear_all_flags();
            
            // sleep to allow final uart messages to come through
            sleep_ms(5000);
            
            reset_pico();
        }
        
        //sleep_ms(2000);
        //if (set_reset_pico){ reset_pico(); }
            
        if (glitch_started && !do_glitch) {
            sleep_ms(2000);
            if (set_reset_pico){ reset_pico(); }
        }
    }
    //sleep_ms(5000);
    //reset_pico();
    
    if (!error_detect && gameos_booted) {
        chase_leds(200, 100, 20);
        sleep_ms(10000);
        log_printf("Glitch should have been successful. The XMB should be loaded, and you should now have LV1 peek/poke/exec");
    }
    
    else if (!error_detect && linux_booted) {
        chase_leds(200, 100, 20);
        sleep_ms(10000);
        log_printf("Glitch should have been successful. You should now have loaded into a custom LV2 kernel or Linux loader");
    }
    
    else {
        random_blink_leds(200, 100, 20);
        sleep_ms(10000);
        log_printf("Glitch should have been successful, but it cannot be verified -> Reset Pico Status [%d]", set_reset_pico);
    }
    
    if (!error_detect && glitch_started && !do_glitch) {
        sleep_ms(2000);
        //reset_pico();
        if (set_reset_pico){ reset_pico(); }
    }
    
    if (error_detect) {
        const led_t osvf[] = {LED_RED, LED_YELLOW};
        blink_leds(osvf, 2, 200, 200, 6);
        blink_leds(osvf, 2, 100, 200, 6);
        blink_led(LED_RED, 200, 200, 10);
        log_printf("!! ERROR FLAGGED: You may have to manually power down the PS3 if it gets stuck");
        //reset_ps3_sequence();
        reset_pico();
    }
}






/*
// Code for keeping persistant success rate using flash writes
#include "hardware/flash.h"

/// where in flash we store a 4 KB stats sector
#define STATS_FLASH_OFFSET   (PICO_FLASH_SIZE_BYTES - 4096)
#define STATS_FLASH_ADDR     (XIP_BASE + STATS_FLASH_OFFSET)

/// event types
typedef enum {
    STATS_ATTEMPT,
    STATS_SUCCESS,
    STATS_FAILURE
} stats_event_t;

/// on‐chip representation (must stay ≤4096 B)
typedef struct {
    uint32_t attempts;
    uint32_t successes;
    uint32_t failures;
    uint32_t crc32;
} stats_t;

/// our in‐RAM copy
static stats_t stats;

/// simple CRC32 (standard polynomial 0xEDB88320)
static uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    }
    return ~crc;
}

/// call once to load & validate from flash
static void __no_inline_not_in_flash_func(stats_load)(void) {
    flash_range_read(STATS_FLASH_OFFSET, (uint8_t*)&stats, sizeof(stats));
    uint32_t saved = stats.crc32;
    stats.crc32 = 0;
    if (crc32((uint8_t*)&stats, sizeof(stats)) != saved) {
        // uninitialized or corrupted
        stats.attempts  = 0;
        stats.successes = 0;
        stats.failures  = 0;
    }
}

/// call once to erase & write back to flash
static void __no_inline_not_in_flash_func(stats_save)(void) {
    stats.crc32 = 0;
    stats.crc32 = crc32((uint8_t*)&stats, sizeof(stats));
    flash_range_erase(STATS_FLASH_OFFSET, 4096);
    flash_range_program(STATS_FLASH_OFFSET,
                       (const uint8_t*)&stats,
                       sizeof(stats));
}

/// Unified stats API: load on first use, bump the right counter,
/// save back to flash, and print the updated percentages.
void stats_event(stats_event_t ev) {
    static bool inited = false;
    if (!inited) {
        stats_load();
        inited = true;
    }
    // update
    switch (ev) {
        case STATS_ATTEMPT:  stats.attempts++;  break;
        case STATS_SUCCESS:  stats.successes++; break;
        case STATS_FAILURE:  stats.failures++;  break;
    }
    // persist
    stats_save();

    // print
#if UART_ENABLED
    uint32_t at = stats.attempts,
             su = stats.successes,
             fa = stats.failures;
    uint32_t sp = at ? (su * 100) / at : 0;
    uint32_t fp = at ? (fa * 100) / at : 0;
    log_printf("Stats: atts=%u succ=%u (%u%%) fail=%u (%u%%)\n",
               at, su, sp, fa, fp);
#endif
}
*/

