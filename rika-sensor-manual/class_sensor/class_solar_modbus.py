import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from Modbus_485 import Modbus_Film69

class SensorPyranometer:
    def __init__(self, port="/dev/ttyS2", slave_address=1, baudrate=9600):
        self.slave_address = slave_address
        self.modbus = Modbus_Film69(port=port, slaveaddress=slave_address, baudrate=baudrate)

    def read_radiation(self, addr=None):
        try:
            address = addr if addr is not None else self.slave_address
            command = f"{address:02X} 03 00 00 00 01"
            response, _ = self.modbus.send(command, resopne_len=7, ID=address)
            parts = response.split()
            if len(parts) < 7:
                raise ValueError("Invalid response length")

            radiation_raw = int(parts[3] + parts[4], 16)

            if radiation_raw < 0 or radiation_raw > 2000:
                raise ValueError(f" Radiation value ({radiation_raw}) Out of range (0 - 2000)")
            
            return {"radiation": radiation_raw}  # หน่วย: W/m²

        except Exception as e:
            print(f"Read failed: {e}")
            return None

    def set_address(self, new_address):
        if not (0x00 <= new_address <= 0xFF):
            raise ValueError("Address must be between 0x00 and 0xFF")

        hex_cmd = f"00 10 {new_address:02X}"
        try:
            response, _ = self.modbus.send(hex_cmd, resopne_len=3, ID=0x00)
            print(f"Set address response: {response}")
        except Exception as e:
            print(f"Set address failed: {e}")

    def read_current_address(self):
        try:
            response, _ = self.modbus.send("00 20", resopne_len=4, ID=0x00)
            parts = response.split()
            if len(parts) < 3:
                raise ValueError("Invalid response")

            current_address = int(parts[2], 16)
            return current_address
        except Exception as e:
            print(f"Read current address failed: {e}")
            return None

    def close(self):
        self.modbus.close()
