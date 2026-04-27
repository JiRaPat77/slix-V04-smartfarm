#!/usr/bin/env python3
"""
LoRa Gateway Test — Luckfox Pico Pro Max
=========================================
Run from project root:
    python3 lora_gateway/test_lora.py

Phases:
  1. Init SX1278 hardware (checks REG_VERSION == 0x12)
  2. Discovery — broadcast [DISCOVER] for 60s, collect nano nodes
  3. Data polling — [REQUEST] every 30s round-robin, print received data

If no nodes pair during discovery, falls back to raw-receive mode for 30s
so you can verify the hardware is at least receiving RF packets.

Requirements:
    pip install python-periphery
"""

import sys
import os
import time
import logging

# Allow running directly or as a module
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lora_gateway.sx1278      import SX1278
from lora_gateway.lora_protocol import LoRaGateway

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s  %(levelname)-7s  %(name)-18s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("test_lora")


# ---------------------------------------------------------------------------
# Data callback — called every time a [DATA] packet arrives
# ---------------------------------------------------------------------------
def on_data(node_id: str, data: dict):
    print(f"\n{'━' * 55}")
    print(f"  DATA from {node_id}")
    print(f"  type  : {data.get('type', '—')}")
    sensors = data.get("sensors", {})
    if sensors:
        for k, v in sensors.items():
            print(f"    {k:20s}: {v}")
    else:
        for k, v in data.items():
            if k not in ("node_id", "type", "timestamp"):
                print(f"    {k:20s}: {v}")
    print(f"{'━' * 55}\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    print()
    print("=" * 55)
    print("  SX1278 LoRa Gateway Test")
    print("  Frequency  : 433 MHz")
    print("  SF / BW    : 7 / 125 kHz")
    print()
    print("  Pin wiring (board pin → function):")
    print("    Pin 20 (GPIO1_D1) → RST")
    print("    Pin 19 (GPIO1_D0) → NSS")
    print("    Pin 17 (GPIO2_B0) → MOSI")
    print("    Pin 16 (GPIO1_C3) → SCK")
    print("    Pin 15 (GPIO1_C2) → MISO")
    print("    Pin 12 (GPIO1_C0) → DIO0")
    print("=" * 55)
    print()

    # ── 1. Hardware init ─────────────────────────────────────────────────────
    log.info("Initialising SX1278...")
    lora = SX1278(frequency=433e6)

    if not lora.init():
        log.error("SX1278 init FAILED.  Check wiring and 3.3 V power.")
        sys.exit(1)

    log.info("SX1278 init OK")

    # ── 2. Protocol setup ────────────────────────────────────────────────────
    gw = LoRaGateway(
        lora                   = lora,
        discover_interval_sec  = 5.0,
        request_interval_sec   = 30.0,
        ack_timeout_sec        = 5.0,
        max_retries            = 3,
    )
    gw.on_data = on_data

    try:
        # ── 3. Discovery ─────────────────────────────────────────────────────
        print("[Phase 1]  Discovery — 60 seconds")
        print("           Power on nano node(s) now...\n")
        assigned = gw.discover(duration_sec=60.0)

        if not assigned:
            # ── Fallback: raw receive ─────────────────────────────────────────
            print("\n[No nodes paired]  Raw receive mode — 30 s\n")
            deadline = time.time() + 30.0
            while time.time() < deadline:
                pkt = lora.receive(timeout_ms=1000)
                if pkt:
                    print(
                        f"  RX raw: {pkt['data']!r}  "
                        f"RSSI={pkt['rssi']}  SNR={pkt['snr']:.1f}"
                    )
            print("\n[Done]  No nodes found. Verify antenna and nano node firmware.")

        else:
            # ── 4. Normal operation ───────────────────────────────────────────
            print(f"\n[Phase 2]  Polling {len(assigned)} node(s): {assigned}")
            print("           Press Ctrl+C to stop.\n")

            cycle = 0
            while True:
                cycle += 1
                print(f"[Cycle {cycle}]  {time.strftime('%H:%M:%S')}")
                gw.poll_all()

                # Print status summary
                for node_id, s in gw.get_status().items():
                    state = "online " if s["online"] else "offline"
                    last  = time.strftime(
                        "%H:%M:%S", time.localtime(s["last_seen"])
                    ) if s["last_seen"] else "—"
                    print(
                        f"  [{node_id}] {state}  "
                        f"last={last}  RSSI={s['rssi']}  SNR={s['snr']}"
                    )

                print(f"\n  Next poll in {gw.request_interval:.0f}s...\n")
                time.sleep(gw.request_interval)

    except KeyboardInterrupt:
        print("\nStopped by user.")

    finally:
        lora.close()
        print("LoRa closed.  Goodbye.")


if __name__ == "__main__":
    main()
