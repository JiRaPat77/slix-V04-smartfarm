# from Modbus_485 import Modbus_Film69

# ser = Modbus_Film69("/dev/ttyS2")
# hex,hex_len=ser.send(f"01 03 00 00 00 02")

# value_soil_temp=int(hex[9:14].replace(" ",""), 16)
# value_soil_moisture = int(hex[15:20].replace(" ",""), 16)
# result = {
#     "soil_temperature": value_soil_temp / 100,
#     "soil_moisture": value_soil_moisture / 100
# }

# print(result)






# Second Class
import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from Modbus_485 import Modbus_Film69

class SensorSoilMoistureTemp:
    def __init__(self, port="/dev/ttyS2", slave_address=1, baudrate=9600):
        self.slave_address = slave_address
        self.modbus = Modbus_Film69(port=port, slaveaddress=slave_address, baudrate=baudrate)

    def read_data(self, addr=None):
        try:
            # response, _ = self.modbus.send("01 03 00 00 00 02", resopne_len=9, ID=self.slave_address)
            address = addr if addr is not None else self.slave_address
            cmd = f"{address:02X} 03 00 00 00 02"
            response, _ = self.modbus.send(cmd, resopne_len=9, ID=address)
            parts = response.split()
            if len(parts) < 9:
                raise ValueError("Invalid response length")

            # แปลงอุณหภูมิ
            temp_raw = int(parts[3] + parts[4], 16)
            temp = self._parse_signed(temp_raw) / 10.0

            # แปลงความชื้น
            moist_raw = int(parts[5] + parts[6], 16)
            moisture = moist_raw / 10.0

            if (temp < -30 or temp > 70) or (moisture < 0 or moisture > 100):
                raise ValueError(f"Sensor incorrect value (Temperature: {temp}, Moisture: {moisture})")
            
            return {"soil_temperature": temp, "soil_moisture": moisture, "Bit":parts}
            
        except Exception as e:
            print(f"Read failed: {e}")
            return None

    def set_address(self, new_address):
        if not (0x00 <= new_address <= 0xFF):
            raise ValueError("Address must be between 0x00 and 0xFF")

        # เปลี่ยน address ที่ register 0x0200 ให้เป็น new_address
        hex_cmd = f"{self.slave_address:02X} 06 02 00 00 {new_address:02X}"
        try:
            response, _ = self.modbus.send(hex_cmd, resopne_len=8, ID=self.slave_address)
            print(f"Set address response: {response}")
        except Exception as e:
            print(f"Set address failed: {e}")

    def _parse_signed(self, value):
   
        if value >= 0x8000:
            return value - 0x10000
        return value

    def close(self):
        self.modbus.close()

    @staticmethod
    def scan_addresses(port="/dev/ttyS2", baudrate=9600):
        found_devices = []
        modbus = Modbus_Film69(port=port, slaveaddress=1, baudrate=baudrate)

        print("Scanning Modbus addresses from 0x01 to 0xF7...")
        for addr in range(1, 248):
            try:
                cmd = f"{addr:02X} 03 00 00 00 02" 
                response, _ = modbus.send(cmd, resopne_len=9, ID=addr)
                parts = response.split()
                if len(parts) < 9:
                    continue

              
                temp_raw = int(parts[3] + parts[4], 16)
                temp = temp_raw if temp_raw < 0x8000 else temp_raw - 0x10000
                temp = temp / 100.0

                moist_raw = int(parts[5] + parts[6], 16)
                moisture = moist_raw / 100.0

                print(f" Found device at address: 0x{addr:02X} | Temp: {temp:.2f} °C, Moisture: {moisture:.2f} %")
                found_devices.append(addr)

            except Exception:
                pass

        modbus.close()
        return found_devices



