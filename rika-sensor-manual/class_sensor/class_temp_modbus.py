import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from Modbus_485 import Modbus_Film69

class SensorAirTempHumidityRS30:
    def __init__(self, port="/dev/ttyS2", slave_address=1, baudrate=9600):
        self.slave_address = slave_address
        self.modbus = Modbus_Film69(port=port, slaveaddress=slave_address, baudrate=baudrate)

    def read_temp(self):
        try:
            cmd = f"{self.slave_address:02X} 03 00 00 00 02"
            response, _ = self.modbus.send(cmd, resopne_len=9, ID=self.slave_address)
            parts = response.split()

            if len(parts) < 9:
                return None

            hum_raw = int(parts[3] + parts[4], 16)
            humidity = hum_raw / 10

            temp_raw = int(parts[5] + parts[6], 16)
            temperature = self._parse_signed(temp_raw) / 10

            return {
                "temperature": round(temperature, 1),
                "humidity": round(humidity, 1)
            }

        except Exception as e:
            print(f"Read failed: {e}")
            return None

    def _parse_signed(self, value):
        if value >= 0x8000:
            return value - 0x10000
        return value

    def set_address(self, new_address):
        if not (0x01 <= new_address <= 0xFE):
            raise ValueError("Address must be 1–254")
        cmd = f"{self.slave_address:02X} 06 07 D0 00 {new_address:02X}"
        try:
            response, _ = self.modbus.send(cmd, resopne_len=8, ID=self.slave_address)
            parts = response.split()
            return len(parts) == 8
        except Exception as e:
            print(f"Set address failed: {e}")
            return False

    def set_baudrate(self, new_baud_rate_value):
     
   
        baud_rate_map = {
            2400: 0,
            4800: 1,
            9600: 2
        }

  
        if new_baud_rate_value not in baud_rate_map:
            raise ValueError("Invalid baud rate value. Must be 2400, 4800, or 9600.")
        
        command_value = baud_rate_map[new_baud_rate_value]

        cmd = f"{self.slave_address:02X} 06 07 D1 00 {command_value:02X}"

        print(f"Sending command to set baud rate: {cmd}")
        try:
      
            response, _ = self.modbus.send(cmd, resopne_len=8, ID=self.slave_address)
            print(f"Set baud rate response: {response}")


            parts = response.split()
            if len(parts) == 8 and \
               parts[0] == f"{self.slave_address:02X}" and \
               parts[1] == '06' and \
               parts[2] == '07' and \
               parts[3] == 'D1' and \
               parts[4] == '00' and \
               int(parts[5], 16) == command_value:
                
                print(f"Baud rate successfully set to {new_baud_rate_value}.")
                self.baudrate = new_baud_rate_value
                self.modbus.close()
                self.modbus = Modbus_Film69(port=self.modbus.port, 
                                            slaveaddress=self.slave_address, 
                                            baudrate=self.baudrate)
                print(f"Modbus connection re-established with baud rate {self.baudrate}.")
                return True
            else:
                print(f"Failed to verify baud rate change from response: {response}")
                return False

        except Exception as e:
            print(f"Set baud rate failed: {e}")
            return False

    def close(self):
        self.modbus.close()
