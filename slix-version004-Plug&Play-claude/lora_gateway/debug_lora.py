#!/usr/bin/env python3
"""
LoRa SX1278 Debug — Luckfox Pico Pro Max
Uses raw sysfs GPIO (no external library needed).

Run: python3 debug_lora.py

จะวิเคราะห์:
  1. GPIO chip ที่มีอยู่ (gpiodetect)
  2. SPI communication ด้วย sysfs GPIO ตรงๆ
  3. ทดสอบสลับ MOSI/MISO อัตโนมัติถ้าไม่เจอ
"""

import os
import time
import subprocess

# ---------------------------------------------------------------------------
# GPIO sysfs numbers จาก pinout image (ตรงกับ bank*32 + group*8 + pin)
# ---------------------------------------------------------------------------
PIN_RST  = 57   # Pin 20  GPIO1_D1
PIN_NSS  = 56   # Pin 19  GPIO1_D0
PIN_MOSI = 72   # Pin 17  GPIO2_B0
PIN_SCK  = 51   # Pin 16  GPIO1_C3
PIN_MISO = 50   # Pin 15  GPIO1_C2
PIN_DIO0 = 48   # Pin 12  GPIO1_C0

GPIO_BASE = "/sys/class/gpio"

# ---------------------------------------------------------------------------
# sysfs GPIO helpers (ไม่ต้องใช้ library ใดๆ)
# ---------------------------------------------------------------------------

def gpio_export(n: int):
    path = f"{GPIO_BASE}/gpio{n}"
    if not os.path.exists(path):
        try:
            with open(f"{GPIO_BASE}/export", "w") as f:
                f.write(str(n))
            time.sleep(0.05)
        except IOError as e:
            print(f"  [WARN] export GPIO{n} failed: {e}")

def gpio_unexport(n: int):
    try:
        with open(f"{GPIO_BASE}/unexport", "w") as f:
            f.write(str(n))
    except Exception:
        pass

def gpio_dir(n: int, d: str):
    with open(f"{GPIO_BASE}/gpio{n}/direction", "w") as f:
        f.write(d)

def gpio_w(n: int, v):
    with open(f"{GPIO_BASE}/gpio{n}/value", "w") as f:
        f.write("1" if v else "0")

def gpio_r(n: int) -> int:
    with open(f"{GPIO_BASE}/gpio{n}/value") as f:
        return int(f.read().strip())

# ---------------------------------------------------------------------------
# SPI bit-bang
# ---------------------------------------------------------------------------

_mosi = PIN_MOSI
_miso = PIN_MISO

def spi_setup():
    for n, d in [(PIN_RST, "out"), (PIN_NSS, "out"),
                 (_mosi,   "out"), (PIN_SCK, "out"),
                 (_miso,   "in"),  (PIN_DIO0, "in")]:
        gpio_export(n)
        gpio_dir(n, d)
    gpio_w(PIN_NSS, 1)
    gpio_w(PIN_SCK, 0)
    gpio_w(_mosi,   0)

def spi_xfer(byte: int) -> int:
    recv = 0
    for bit in range(7, -1, -1):
        gpio_w(_mosi, bool(byte & (1 << bit)))
        gpio_w(PIN_SCK, 1)
        if gpio_r(_miso):
            recv |= (1 << bit)
        gpio_w(PIN_SCK, 0)
    return recv

def spi_read_reg(addr: int) -> int:
    gpio_w(PIN_NSS, 0)
    spi_xfer(addr & 0x7F)
    val = spi_xfer(0x00)
    gpio_w(PIN_NSS, 1)
    return val

def spi_write_reg(addr: int, value: int):
    gpio_w(PIN_NSS, 0)
    spi_xfer(addr | 0x80)
    spi_xfer(value & 0xFF)
    gpio_w(PIN_NSS, 1)

def hw_reset():
    gpio_w(PIN_RST, 0)
    time.sleep(0.02)
    gpio_w(PIN_RST, 1)
    time.sleep(0.05)

def cleanup():
    for n in [PIN_RST, PIN_NSS, PIN_MOSI, PIN_SCK, PIN_MISO, PIN_DIO0]:
        gpio_unexport(n)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def check_version() -> int:
    hw_reset()
    return spi_read_reg(0x42)   # REG_VERSION

def dump_regs():
    regs = {
        "REG_OP_MODE (0x01)": 0x01,
        "REG_FRF_MSB (0x06)": 0x06,
        "REG_FRF_MID (0x07)": 0x07,
        "REG_FRF_LSB (0x08)": 0x08,
        "REG_MODEM_CFG1(0x1D)": 0x1D,
        "REG_MODEM_CFG2(0x1E)": 0x1E,
    }
    for name, addr in regs.items():
        print(f"    {name} = 0x{spi_read_reg(addr):02X}")

def main():
    global _mosi, _miso

    print("=" * 55)
    print("  SX1278 / Ra-02  Debug Script  (sysfs GPIO)")
    print("=" * 55)

    # ── 1. GPIO chips ────────────────────────────────────
    print("\n[1] Available GPIO chips:")
    chips = sorted([f for f in os.listdir("/dev") if f.startswith("gpiochip")])
    if chips:
        for c in chips:
            print(f"    /dev/{c}")
    else:
        print("    (no gpiochip devices found)")

    # แสดง /sys/class/gpio
    print("\n    /sys/class/gpio contents:")
    try:
        entries = sorted(os.listdir("/sys/class/gpio"))
        for e in entries:
            print(f"      {e}")
    except Exception as e:
        print(f"      Error: {e}")

    # ── 2. Setup sysfs GPIO ──────────────────────────────
    print(f"\n[2] Exporting GPIO pins via sysfs...")
    print(f"    RST ={PIN_RST}  NSS={PIN_NSS}  MOSI={_mosi}")
    print(f"    SCK ={PIN_SCK}  MISO={_miso}  DIO0={PIN_DIO0}")

    try:
        spi_setup()
        print("    Export OK")
    except Exception as e:
        print(f"    FAILED: {e}")
        print("    → Kernel GPIO sysfs may be disabled. Check /sys/class/gpio/")
        return

    # ── Check for hardware SPI conflict ─────────────────
    print("\n[3] Checking for hardware SPI0 conflict:")
    spidev_list = [f for f in os.listdir("/dev") if f.startswith("spidev")]
    if spidev_list:
        print(f"    FOUND: {spidev_list}")
        print("    WARNING: hardware SPI0 active — Pin15/16 might be locked by kernel")
        print("             Pin15 (GPIO1_C2) = SPI0_MOSI_M0")
        print("             Pin16 (GPIO1_C3) = SPI0_MISO_M0")
        print("             These pins may be unusable as GPIO")
    else:
        print("    /dev/spidev* not found — hardware SPI0 not active (OK)")

    # ── Individual pin test (multimeter check) ───────────
    print("\n[4] Pin toggle test (verify with multimeter):")
    test_pins = [
        (PIN_NSS,  "NSS  Pin19"),
        (PIN_RST,  "RST  Pin20"),
        (PIN_MOSI, "MOSI Pin17"),
        (PIN_SCK,  "SCK  Pin16"),
    ]
    for pin, label in test_pins:
        gpio_w(pin, 0)
        time.sleep(0.05)
        gpio_w(pin, 1)
        time.sleep(0.05)
        gpio_w(pin, 0)
        print(f"    {label} (GPIO{pin}) toggled 0→1→0  ← measure with multimeter")

    print(f"\n    MISO Pin15 (GPIO{PIN_MISO}) = {gpio_r(PIN_MISO)} (now reading)")
    print(f"    DIO0 Pin12 (GPIO{PIN_DIO0}) = {gpio_r(PIN_DIO0)} (now reading)")

    # ── NSS test ─────────────────────────────────────────
    print("\n[5] NSS pulse test:")
    print("    Holding NSS LOW for 2 seconds — measure Pin19 with multimeter now")
    gpio_w(PIN_NSS, 0)
    time.sleep(2.0)
    gpio_w(PIN_NSS, 1)
    print("    NSS back HIGH")

    print("\n[6] MISO monitoring (Ra-02 NSS low, 50 SCK pulses):")
    gpio_w(PIN_NSS, 0)
    gpio_w(_mosi, 0)
    miso_vals = []
    for _ in range(50):
        gpio_w(PIN_SCK, 1)
        miso_vals.append(gpio_r(_miso))
        gpio_w(PIN_SCK, 0)
    gpio_w(PIN_NSS, 1)
    ones  = sum(miso_vals)
    zeros = len(miso_vals) - ones
    print(f"    MISO readings: {ones} HIGH, {zeros} LOW out of {len(miso_vals)}")
    if ones == 0:
        print("    → MISO always 0: module not powered, not connected, or pin conflict")
    elif ones == len(miso_vals):
        print("    → MISO always 1: MISO floating high (not connected to module)")
    else:
        print("    → MISO fluctuates — module IS responding, SPI data flowing")

    try:
        # ── 7. First read attempt ────────────────────────
        print(f"\n[7] Reading REG_VERSION (should be 0x12 for SX1278)...")
        ver = check_version()
        print(f"    REG_VERSION = 0x{ver:02X}")

        if ver == 0x12:
            print("    ✓ SX1278 detected! SPI OK.\n")
            print("[8] Register dump:")
            dump_regs()

        elif ver in (0x00, 0xFF):
            print(f"    ✗ MISO stuck at {'HIGH' if ver == 0xFF else 'LOW'}")
            print(f"      → Trying MOSI/MISO swapped (Pin15↔Pin17)...\n")

            # Swap MOSI/MISO
            _mosi, _miso = _miso, _mosi
            gpio_dir(_mosi, "out")
            gpio_dir(_miso, "in")

            ver2 = check_version()
            print(f"    After swap: REG_VERSION = 0x{ver2:02X}")

            if ver2 == 0x12:
                print(f"    ✓ Found with swapped pins!")
                print(f"      Fix sx1278.py: swap _mosi(chip line) and _miso(chip line)")
                print(f"      MOSI should use GPIO{_mosi}, MISO should use GPIO{_miso}")
            else:
                print("    ✗ Still not found.\n")
                print("    Possible causes:")
                print("      A) 3.3V power not connected to Ra-02 VCC")
                print("      B) GND not connected")
                print("      C) NSS (CS) pin wrong")
                print("      D) Pinmux conflict (pin used by other peripheral in device tree)")
                print()
                print("    Try manually:")
                print(f"      echo {PIN_NSS} > /sys/class/gpio/export")
                print(f"      echo out > /sys/class/gpio/gpio{PIN_NSS}/direction")
                print(f"      echo 0   > /sys/class/gpio/gpio{PIN_NSS}/value")
                print(f"      # Check with multimeter: Pin 19 should read ~0V")
        else:
            print(f"    ? Partial value 0x{ver:02X} — possible floating MISO or noise")

    except Exception as e:
        print(f"\n    ERROR during SPI test: {e}")
        import traceback
        traceback.print_exc()

    finally:
        cleanup()
        print("\n[Done] GPIO unexported.")


if __name__ == "__main__":
    main()
