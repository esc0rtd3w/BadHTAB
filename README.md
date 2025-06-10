![BadHTAB Wiring](http://ps3xploit.me/images/badhtab-wiring-picow.png)

# BadHTAB

**BadHTAB** is a **hardware–software** hypervisor (LV1) exploit for Sony PlayStation 3. Originally invented by geohot for Linux, it works by briefly pulling certain RAM signals to ground during a hypervisor HTAB invalidation—causing the write to be skipped and leaving an entry valid. This grants full read/write access to a small region of hypervisor memory, which can later be manipulated for complete memory control.

This GameOS-ported exploit runs on every PS3 model with PS3HEN, bringing CFW-only features to non-CFW consoles for the first time ever.

> **Warning:** Success rate is low (~5–10%). Requires soldering, patience, and skill. Not for daily-driver consoles.  
> **Note:** Not persistent—must rerun after every reboot.  
> **Firmware:** Supports **4.70 or later**.

---

## Components

- **BadHTAB** — Software side, delivered as a `.pkg` for PS3  
- **ps3pulldown2** — Hardware side, a Raspberry Pi Pico (RP2040) glitcher over USB

---

## Features

After a successful run:

- **`hvcall 114 everywhere`** — Map any memory region without restriction  
- **New HVCalls**: `lv1_peek(34)` / `lv1_poke(35)` / `lv1_exec(36)`  
- **Dump LV1 memory** — Write hypervisor RAM to file  
- **Boot custom `lv2_kernel.fself`** — Load any LV2 kernel in FSELF format  
- **Boot OtherOS** — Launch Petitboot to restore Linux/OtherOS  

> **Note:** Using “Boot LV2/OtherOS” removes the new HVCalls (besides 114); you can reinstall them via HVCall 114.

---

## New HVCalls

```c
// lv1_peek(34)
//   in:  r3 = addr
//  out:  r3 = value

// lv1_poke(35)
//   in:  r3 = addr, r4 = value
//  out:  r3 = 0

// lv1_exec(36)
//   in:  r3–r8 = args, r9 = addr
```

---

## Installation (Hardware)

<details>
<summary><strong>Requirements & Wiring</strong></summary>

- Raspberry Pi Pico (RP2040)  
- 0.1 mm magnet wire  
- Soldering tools  

### Glitch Wiring

Solder two pull-down wires from the PS3 RAM resistors to the Pico:

1. Identify the two target resistors (e.g. **RQ7** and **RQ8** on the PS3 service manual or by tracing after desoldering).  
2. Solder one end of each wire to those resistors.  
3. Solder the other ends to the Pico’s GPIO15 and GPIO16 (bottom-most pins).  

<p align="center">
  <img src="https://github.com/user-attachments/assets/20fc9f39-b23a-43f3-9067-c0c686b4bc2b" alt="BadHTAB Solder Points" width="50%">
</p>

### Assembly & UF2 Flash

1. Reassemble PS3 enough to power on safely.  
2. Hold **BOOTSEL** and plug Pico into PC → new drive appears.  
3. Copy `ps3pulldown2.uf2` from [Releases](https://github.com/aomsin2526/BadHTAB/releases) onto the Pico drive.  
4. Remove Pico, reinsert into PS3’s USB port.

> **Tips:**  
> - Keep wires off any metal.  
> - Test on a naked PS3 (no case) for easy access—HDD light blink verifies boot.  
> - Super Slim power buttons are fragile—consider shorting pins with a screwdriver.

</details>

---

## Installation (Software)

<details>
<summary><strong>BadHTAB .pkg & Configuration</strong></summary>

1. Install `BadHTAB.pkg` from [Releases](https://github.com/aomsin2526/BadHTAB/releases).  

<details>
<summary><strong>Dump LV1 Memory</strong></summary>

- Create an empty file at `/dev_hdd0/BadHTAB_doDumpLv1.txt`  
  - (For 240 MB dump, use `BadHTAB_doDumpLv1_240M.txt`)  
- Run the exploit.

</details>

<details>
<summary><strong>Boot Custom LV2 Kernel (FSELF)</strong></summary>

1. Convert `lv2_kernel.self` → `lv2_kernel.fself`:
   ```bash
   make_fself.exe -u lv2_kernel.elf lv2_kernel.fself
   ```
2. Place an empty `/dev_hdd0/BadHTAB_doLoadLv2Kernel_Fself.txt`.  
3. Copy `lv2_kernel.fself` to `/dev_flash/sys/lv2_kernel.fself`.  
4. Graceful shutdown, then run exploit.

> **Tip:** Use `/dev_blind/` (via webman MOD) to write to `/dev_flash/` if full. Remove `ps1emu/ps2emu/pspemu` to free space.

</details>

<details>
<summary><strong>Boot OtherOS (Petitboot)</strong></summary>

1. Place empty `/dev_hdd0/BadHTAB_doOtherOS.txt`.  
2. Copy `dtbImage.ps3.fself` (from [ps3-petitboot-kexec-patched](https://github.com/aomsin2526/ps3-petitboot-kexec-patched/releases/tag/fself)) to `/dev_flash/sys/dtbImage.ps3.fself`.  
3. Graceful shutdown, then run exploit.

</details>

</details>

---

## Running the Exploit

1. Plug Pico into a PS3 USB port.  
2. Launch **BadHTAB** pkg.  
3. **Beep** = status indicator; log at `/dev_hdd0/BadHTAB.txt`.  

   1. One triple-beep = exploit started.  
   2. Pico LED will blink; you’ll hear periodic beeps.  
   3. If beeps stop or PS3 powers off → glitch failed → reboot & retry.  
   4. Successful glitch: triple-beep, pause, then multiple triple-beeps.  
   5. HVCalls are patched; configured action (dump/kernel/OtherOS) runs.  
   6. If not booting LV2/OtherOS → 5 s long beep → exploit ends → back to XMB.

---

## Additional Modifications via esc0rtd3w

- **Auto-reboot** on crash  
- UART0 for Pico, UART1 for PS3 SB_UART  
- PSU standby & ribbon-connector monitoring  
- 4 status LEDs (error/yellow/green/blue)  
- Optional HDD activity monitoring  
- Improved glitch stability & auto-recover (requires PSU 5 V “always-on” to Pico + toggle switch)  
- **SYSCON UART enables**  
  ```
  Mullion CXR713*:   w 7202 02
  Mullion CXR713120: w 4202 02
  Mullion CXR714*:   w 4202 02
  Sherwood SYSCON:   w 1202 02
  ```  
  > **Warning:** Do **not** enable extra UART outputs or glitch will crash on XMB return.

**PS3 → Pico wiring summary**  
| Signal                     | PS3 Pin/Source                    | Pico GPIO |
|----------------------------|-----------------------------------|-----------|
| pulldown1 (RQ7)            | RAM resistor                      | 15        |
| pulldown2 (RQ8)            | RAM resistor                      | 16        |
| pwr_on_ribbon (PS3 3.3 V)  | Ribbon connector                  | 10        |
| sb_uart_rx (PS3 SB_TX)     | Super Busy UART TX                | 5         |
| psu_standby                | PSU standby pin 3                 | 18        |
| (optional) hdd_activity    | HDD LED anode                     | 22        |

**Pico status LEDs**  
| Color  | GPIO |
|--------|------|
| Error  | 6    |
| Yellow | 2    |
| Green  | 21   |
| Blue   | 27   |
