"""
SX1278 LoRa hardware driver — software SPI (bit-bang) for Luckfox Pico Pro Max.

Pin mapping (physical board pin → GPIO bank/line):
  Pin 20  GPIO1_D1  gpiochip1 line 25  — RST
  Pin 19  GPIO1_D0  gpiochip1 line 24  — NSS (CS)
  Pin 17  GPIO2_B0  gpiochip2 line  8  — MOSI
  Pin 16  GPIO1_C3  gpiochip1 line 19  — SCK
  Pin 15  GPIO1_C2  gpiochip1 line 18  — MISO
  Pin 12  GPIO1_C0  gpiochip1 line 16  — DIO0 (RX/TX feedback)

Requires: pip install python-periphery
"""

import time
import logging

try:
    from periphery import GPIO
except ImportError:
    raise ImportError("Run: pip install python-periphery")

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# SX1278 Register Map
# ---------------------------------------------------------------------------
REG_FIFO          = 0x00
REG_OP_MODE       = 0x01
REG_FRF_MSB       = 0x06
REG_FRF_MID       = 0x07
REG_FRF_LSB       = 0x08
REG_PA_CONFIG     = 0x09
REG_OCP           = 0x0B
REG_LNA           = 0x0C
REG_FIFO_ADDR_PTR = 0x0D
REG_FIFO_TX_BASE  = 0x0E
REG_FIFO_RX_BASE  = 0x0F
REG_FIFO_RX_CURR  = 0x10
REG_IRQ_FLAGS     = 0x12
REG_RX_NB_BYTES   = 0x13
REG_PKT_SNR       = 0x19
REG_PKT_RSSI      = 0x1A
REG_MODEM_CFG1    = 0x1D
REG_MODEM_CFG2    = 0x1E
REG_PREAMBLE_MSB  = 0x20
REG_PREAMBLE_LSB  = 0x21
REG_PAYLOAD_LEN   = 0x22
REG_MODEM_CFG3    = 0x26
REG_DETECT_OPT    = 0x31
REG_DETECT_THR    = 0x37
REG_SYNC_WORD     = 0x39
REG_DIO_MAP1      = 0x40
REG_VERSION       = 0x42
REG_PA_DAC        = 0x4D

# ---------------------------------------------------------------------------
# Operating modes  (bit7 = LoRa mode)
# ---------------------------------------------------------------------------
_LR          = 0x80
MODE_SLEEP   = _LR | 0x00
MODE_STDBY   = _LR | 0x01
MODE_TX      = _LR | 0x03
MODE_RX_CONT = _LR | 0x05

# ---------------------------------------------------------------------------
# IRQ flag bits
# ---------------------------------------------------------------------------
IRQ_TX_DONE  = 0x08
IRQ_RX_DONE  = 0x40
IRQ_CRC_ERR  = 0x20


class SX1278:
    """
    SX1278 LoRa driver using software SPI.
    Settings: 433 MHz, SF7, BW125kHz, CR4/5, CRC on — matches nano node defaults.
    """

    def __init__(self, frequency: float = 433e6):
        self.frequency = frequency
        self._open_gpio()

    # -----------------------------------------------------------------------
    # GPIO setup
    # -----------------------------------------------------------------------

    def _open_gpio(self):
        # gpiochip1 = GPIO1 bank,  gpiochip2 = GPIO2 bank
        # Line offset = group_index*8 + pin_num
        #   GPIO1_D1 → D=3, 1 → line 25
        #   GPIO1_D0 → D=3, 0 → line 24
        #   GPIO2_B0 → B=1, 0 → line  8
        #   GPIO1_C3 → C=2, 3 → line 19
        #   GPIO1_C2 → C=2, 2 → line 18
        #   GPIO1_C0 → C=2, 0 → line 16
        self._rst  = GPIO("/dev/gpiochip1", 25, "out")   # Pin 20
        self._nss  = GPIO("/dev/gpiochip1", 24, "out")   # Pin 19
        self._mosi = GPIO("/dev/gpiochip2",  8, "out")   # Pin 17
        self._sck  = GPIO("/dev/gpiochip1", 19, "out")   # Pin 16
        self._miso = GPIO("/dev/gpiochip1", 18, "in")    # Pin 15
        self._dio0 = GPIO("/dev/gpiochip1", 16, "in")    # Pin 12

        self._nss.write(True)    # CS idle = high
        self._sck.write(False)
        self._mosi.write(False)

    # -----------------------------------------------------------------------
    # Software SPI — Mode 0 (CPOL=0, CPHA=0), MSB first
    # -----------------------------------------------------------------------

    def _xfer_byte(self, byte: int) -> int:
        recv = 0
        for bit in range(7, -1, -1):
            self._mosi.write(bool(byte & (1 << bit)))
            self._sck.write(True)            # rising edge → sample MISO
            if self._miso.read():
                recv |= (1 << bit)
            self._sck.write(False)           # falling edge
        return recv

    def _read_reg(self, addr: int) -> int:
        self._nss.write(False)
        self._xfer_byte(addr & 0x7F)         # MSB=0 → read
        val = self._xfer_byte(0x00)
        self._nss.write(True)
        return val

    def _write_reg(self, addr: int, value: int):
        self._nss.write(False)
        self._xfer_byte(addr | 0x80)         # MSB=1 → write
        self._xfer_byte(value & 0xFF)
        self._nss.write(True)

    # -----------------------------------------------------------------------
    # Initialisation
    # -----------------------------------------------------------------------

    def reset(self):
        self._rst.write(False)
        time.sleep(0.02)
        self._rst.write(True)
        time.sleep(0.05)

    def init(self) -> bool:
        """
        Reset and configure the SX1278.
        Returns True on success, False if chip not detected.
        """
        self.reset()

        ver = self._read_reg(REG_VERSION)
        if ver != 0x12:
            logger.error(f"SX1278 not found  REG_VERSION=0x{ver:02X} (expected 0x12)")
            return False

        # Must be in sleep mode before entering LoRa mode
        self._write_reg(REG_OP_MODE, MODE_SLEEP)
        time.sleep(0.01)

        # Frequency: 433 MHz
        # frf = freq / (Fxosc / 2^19) = freq * 2^19 / 32e6
        frf = int(self.frequency * (1 << 19) / 32e6)
        self._write_reg(REG_FRF_MSB, (frf >> 16) & 0xFF)
        self._write_reg(REG_FRF_MID, (frf >>  8) & 0xFF)
        self._write_reg(REG_FRF_LSB,  frf        & 0xFF)

        # FIFO base addresses
        self._write_reg(REG_FIFO_TX_BASE, 0x00)
        self._write_reg(REG_FIFO_RX_BASE, 0x00)

        # LNA: max gain, boost on (recommended for 433 MHz)
        self._write_reg(REG_LNA, 0x23)

        # Modem config 1: BW=125kHz(0111), CR=4/5(001), ExplicitHeader(0)
        self._write_reg(REG_MODEM_CFG1, 0x72)

        # Modem config 2: SF=7(0111), TX single(0), CRC on(1), RxTimeout MSB=00
        self._write_reg(REG_MODEM_CFG2, 0x74)

        # Modem config 3: AGC on, LowDataRateOptimize off (only needed SF≥11)
        self._write_reg(REG_MODEM_CFG3, 0x04)

        # Preamble: 8 symbols
        self._write_reg(REG_PREAMBLE_MSB, 0x00)
        self._write_reg(REG_PREAMBLE_LSB, 0x08)

        # Sync word 0x12 = public LoRa network
        self._write_reg(REG_SYNC_WORD, 0x12)

        # PA: PA_BOOST pin, MaxPower=7, OutputPower=15 → ~17 dBm
        self._write_reg(REG_PA_CONFIG, 0x8F)
        self._write_reg(REG_PA_DAC,    0x84)

        # OCP: 100 mA
        self._write_reg(REG_OCP, 0x2B)

        # DIO0 = RxDone by default
        self._write_reg(REG_DIO_MAP1, 0x00)

        # Standby
        self._write_reg(REG_OP_MODE, MODE_STDBY)
        time.sleep(0.01)

        logger.info(
            f"SX1278 initialised  "
            f"freq={self.frequency/1e6:.1f} MHz  SF7  BW125  CR4/5  CRC=on"
        )
        return True

    # -----------------------------------------------------------------------
    # Transmit
    # -----------------------------------------------------------------------

    def send(self, data, timeout_sec: float = 3.0) -> bool:
        """
        Send a string or bytes packet.
        Returns True when TX-done IRQ is received within timeout_sec.
        """
        payload = data.encode("utf-8") if isinstance(data, str) else bytes(data)

        self._write_reg(REG_OP_MODE,       MODE_STDBY)
        self._write_reg(REG_DIO_MAP1,      0x40)    # DIO0 = TxDone
        self._write_reg(REG_FIFO_ADDR_PTR, 0x00)

        for byte in payload:
            self._write_reg(REG_FIFO, byte)
        self._write_reg(REG_PAYLOAD_LEN, len(payload))

        self._write_reg(REG_OP_MODE, MODE_TX)

        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            if self._read_reg(REG_IRQ_FLAGS) & IRQ_TX_DONE:
                self._write_reg(REG_IRQ_FLAGS, 0xFF)     # clear all IRQs
                self._write_reg(REG_OP_MODE,   MODE_STDBY)
                logger.debug(f"TX ok  {len(payload)}B  {data!r}")
                return True
            time.sleep(0.005)

        logger.warning(f"TX timeout  ({len(payload)} bytes)")
        self._write_reg(REG_OP_MODE, MODE_STDBY)
        return False

    # -----------------------------------------------------------------------
    # Receive
    # -----------------------------------------------------------------------

    def receive(self, timeout_ms: int = 1000) -> dict | None:
        """
        Enter RX-continuous mode and wait up to timeout_ms for a packet.
        Returns {"data": str, "rssi": int, "snr": float} or None.
        """
        self._write_reg(REG_DIO_MAP1, 0x00)       # DIO0 = RxDone
        self._write_reg(REG_OP_MODE,  MODE_RX_CONT)

        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            flags = self._read_reg(REG_IRQ_FLAGS)

            if flags & IRQ_RX_DONE:
                self._write_reg(REG_IRQ_FLAGS, 0xFF)   # clear IRQs first

                if flags & IRQ_CRC_ERR:
                    logger.warning("RX CRC error — packet discarded")
                    continue                            # keep listening

                nb   = self._read_reg(REG_RX_NB_BYTES)
                addr = self._read_reg(REG_FIFO_RX_CURR)
                self._write_reg(REG_FIFO_ADDR_PTR, addr)
                raw  = bytes([self._read_reg(REG_FIFO) for _ in range(nb)])

                # SNR  (signed 7.2 fixed-point, register is 2s-complement byte)
                snr_raw = self._read_reg(REG_PKT_SNR)
                snr     = snr_raw / 4.0 if snr_raw < 128 else (snr_raw - 256) / 4.0

                # RSSI (correction depends on SNR sign)
                rssi_raw = self._read_reg(REG_PKT_RSSI)
                rssi     = (-164 + rssi_raw) if snr < 0 else (-157 + rssi_raw)

                self._write_reg(REG_OP_MODE, MODE_STDBY)

                try:
                    text = raw.decode("utf-8")
                except UnicodeDecodeError:
                    text = raw.hex()

                logger.debug(f"RX  {text!r}  RSSI={rssi}  SNR={snr:.1f}")
                return {"data": text, "rssi": rssi, "snr": snr}

            time.sleep(0.001)

        self._write_reg(REG_OP_MODE, MODE_STDBY)
        return None

    # -----------------------------------------------------------------------
    # Cleanup
    # -----------------------------------------------------------------------

    def close(self):
        try:
            self._write_reg(REG_OP_MODE, MODE_SLEEP)
        except Exception:
            pass
        for pin in (self._rst, self._nss, self._mosi, self._sck, self._miso, self._dio0):
            try:
                pin.close()
            except Exception:
                pass
        logger.info("SX1278 closed")
