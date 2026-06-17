# import serial
# import time
# import json

# class UltrasonicModbus:
#     def __init__(self, port="/dev/ttyS2", slave_address=0x4C, baudrate=4800, timeout=1.0):
#         self.port = port
#         self.slave_address = slave_address
#         self.baudrate = baudrate
#         self.timeout = timeout

#     @staticmethod
#     def modbus_crc(buf):
#         crc = 0xFFFF
#         for b in buf:
#             crc ^= b
#             for _ in range(8):
#                 if crc & 1:
#                     crc = (crc >> 1) ^ 0xA001
#                 else:
#                     crc >>= 1
#         return crc

#     def read_distance(self, max_attempts=5, delay_between=0.001):
#         cmd = [self.slave_address, 0x03, 0x00, 0x00, 0x00, 0x01]
#         crc = self.modbus_crc(cmd)
#         cmd.append(crc & 0xFF)          # CRC Low
#         cmd.append((crc >> 8) & 0xFF)   # CRC High

#         result = {
#             "port": self.port,
#             "slave_address": self.slave_address,
#             "distance_cm": None,
#             "crc_error": False,
#             "attempts": 0,
#             "success": False,
#             "raw": None,
#             "timestamp": None
#         }

#         try:
#             ser = serial.Serial(self.port, baudrate=self.baudrate, bytesize=8, parity="N", stopbits=1, timeout=self.timeout)
#         except Exception as e:
#             result["error"] = f"Serial error: {e}"
#             return result

#         value = None
#         for attempt in range(1, max_attempts + 1):
#             ser.reset_input_buffer()
#             ser.write(bytearray(cmd))
#             ser.flush()
#             resp = ser.read(7)
#             print(f"[Attempt {attempt}] Raw response bytes: {list(resp)}")
#             result["raw"] = list(resp)
#             result["attempts"] = attempt

#             if len(resp) != 7:
#                 time.sleep(delay_between)
#                 continue

#             addr, func, bytecount, hi, lo, crc_l, crc_h = resp
#             resp_frame = [addr, func, bytecount, hi, lo]
#             crc_calc = self.modbus_crc(resp_frame)

#             if (crc_l != (crc_calc & 0xFF)) or (crc_h != ((crc_calc >> 8) & 0xFF)):
#                 result["crc_error"] = True
#                 time.sleep(delay_between)
#                 continue

#             value = (hi << 8) | lo
#             result["distance_cm"] = value
#             result["crc_error"] = False
#             result["success"] = True
#             result["timestamp"] = int(time.time())
#             break
#         else:
#             result["success"] = False

#         ser.close()
#         return result

#     def read_json(self):
#         """Return result as JSON string"""
#         return json.dumps(self.read_distance(), ensure_ascii=False)








import serial
import time
import json

class UltrasonicModbus:
    def __init__(self, port="/dev/ttyS2", slave_address=0x32, baudrate=4800, timeout=1.0):
        self.port = port
        self.slave_address = slave_address
        self.baudrate = baudrate
        self.timeout = timeout

    @staticmethod
    def modbus_crc(buf):
        crc = 0xFFFF
        for b in buf:
            crc ^= b
            for _ in range(8):
                if crc & 1:
                    crc = (crc >> 1) ^ 0xA001
                else:
                    crc >>= 1
        return crc

    def read_distance(self, max_attempts=5, delay_between=0.001):
        cmd = [self.slave_address, 0x03, 0x00, 0x00, 0x00, 0x01]
        crc = self.modbus_crc(cmd)
        cmd.append(crc & 0xFF)          # CRC Low
        cmd.append((crc >> 8) & 0xFF)   # CRC High

        result = {
            "port": self.port,
            "slave_address": self.slave_address,
            "distance_cm": None,
            "crc_error": False,
            "attempts": 0,
            "success": False,
            "raw": None,
            "timestamp": None
        }

        try:
            ser = serial.Serial(self.port, baudrate=self.baudrate, bytesize=8, parity="N", stopbits=1, timeout=self.timeout)
        except Exception as e:
            result["error"] = f"Serial error: {e}"
            return result

        value = None
        for attempt in range(1, max_attempts + 1):
            ser.reset_input_buffer()
            ser.write(bytearray(cmd))
            ser.flush()
            resp = ser.read(7)
            print(f"[Attempt {attempt}] Raw response bytes: {list(resp)}")
            result["raw"] = list(resp)
            result["attempts"] = attempt

            if len(resp) != 7:
                time.sleep(delay_between)
                continue

            addr, func, bytecount, hi, lo, crc_l, crc_h = resp
            resp_frame = [addr, func, bytecount, hi, lo]
            crc_calc = self.modbus_crc(resp_frame)

            if (crc_l != (crc_calc & 0xFF)) or (crc_h != ((crc_calc >> 8) & 0xFF)):
                result["crc_error"] = True
                time.sleep(delay_between)
                continue

            value = (hi << 8) | lo
            result["distance_cm"] = value
            result["crc_error"] = False
            result["success"] = True
            result["timestamp"] = int(time.time())
            break
        else:
            result["success"] = False

        ser.close()
        return result

    def read_json(self):
        """Return result as JSON string"""
        return json.dumps(self.read_distance(), ensure_ascii=False)

    def check_address(self, max_attempts=3, delay_between=0.5):
        """
        อ่าน address ปัจจุบันของ slave device
        ใช้ Function 0x03 อ่าน Register 0x0100
        """
        cmd = [self.slave_address, 0x03, 0x01, 0x00, 0x00, 0x01]
        crc = self.modbus_crc(cmd)
        cmd.append(crc & 0xFF)
        cmd.append((crc >> 8) & 0xFF)

        result = {
            "port": self.port,
            "slave_address": self.slave_address,
            "current_address": None,
            "crc_error": False,
            "attempts": 0,
            "success": False,
            "raw": None,
            "timestamp": None
        }

        try:
            ser = serial.Serial(self.port, baudrate=self.baudrate, bytesize=8, parity="N", stopbits=1, timeout=self.timeout)
        except Exception as e:
            result["error"] = f"Serial error: {e}"
            return result

        for attempt in range(1, max_attempts + 1):
            ser.reset_input_buffer()
            ser.write(bytearray(cmd))
            ser.flush()
            resp = ser.read(7)
            result["raw"] = list(resp)
            result["attempts"] = attempt

            if len(resp) != 7:
                time.sleep(delay_between)
                continue

            addr, func, bytecount, hi, lo, crc_l, crc_h = resp
            resp_frame = [addr, func, bytecount, hi, lo]
            crc_calc = self.modbus_crc(resp_frame)

            if (crc_l != (crc_calc & 0xFF)) or (crc_h != ((crc_calc >> 8) & 0xFF)):
                result["crc_error"] = True
                time.sleep(delay_between)
                continue

            current_addr = lo  
            result["current_address"] = f"0x{current_addr:02X}"
            result["current_address_decimal"] = current_addr
            result["crc_error"] = False
            result["success"] = True
            result["timestamp"] = int(time.time())
            break
        else:
            result["success"] = False

        ser.close()
        return result

    def change_address(self, new_address, max_attempts=3, delay_between=0.5):
        """
        เปลี่ยน address ของ slave device
        ใช้ Function 0x06 เขียน Register 0x0100
        new_address: ค่า address ใหม่ (0x01 - 0xF7)
        """
        if not (0x01 <= new_address <= 0xF7):
            return {
                "error": "Invalid address range. Must be between 0x01 and 0xF7",
                "success": False
            }

        cmd = [self.slave_address, 0x06, 0x01, 0x00, 0x00, new_address]
        crc = self.modbus_crc(cmd)
        cmd.append(crc & 0xFF)
        cmd.append((crc >> 8) & 0xFF)

        result = {
            "port": self.port,
            "old_address": f"0x{self.slave_address:02X}",
            "new_address": f"0x{new_address:02X}",
            "crc_error": False,
            "attempts": 0,
            "success": False,
            "raw": None,
            "timestamp": None
        }

        try:
            ser = serial.Serial(self.port, baudrate=self.baudrate, bytesize=8, parity="N", stopbits=1, timeout=self.timeout)
        except Exception as e:
            result["error"] = f"Serial error: {e}"
            return result

        for attempt in range(1, max_attempts + 1):
            ser.reset_input_buffer()
            ser.write(bytearray(cmd))
            ser.flush()
            resp = ser.read(8)
            result["raw"] = list(resp)
            result["attempts"] = attempt

            if len(resp) != 8:
                time.sleep(delay_between)
                continue

            resp_frame = list(resp[:6])
            crc_calc = self.modbus_crc(resp_frame)
            crc_l, crc_h = resp[6], resp[7]

            if (crc_l != (crc_calc & 0xFF)) or (crc_h != ((crc_calc >> 8) & 0xFF)):
                result["crc_error"] = True
                time.sleep(delay_between)
                continue

            result["crc_error"] = False
            result["success"] = True
            result["timestamp"] = int(time.time())
            result["message"] = f"Address changed from 0x{self.slave_address:02X} to 0x{new_address:02X}"
            
            
            self.slave_address = new_address
            break
        else:
            result["success"] = False

        ser.close()
        return result

    def reset_address(self, default_address=0x32, max_attempts=3, delay_between=0.5):
  
        cmd = [self.slave_address, 0x06, 0x02, 0x00, 0x00, 0x00]
        crc = self.modbus_crc(cmd)
        cmd.append(crc & 0xFF)
        cmd.append((crc >> 8) & 0xFF)

        result = {
            "port": self.port,
            "old_address": f"0x{self.slave_address:02X}",
            "reset_to_default": f"0x{default_address:02X}",
            "crc_error": False,
            "attempts": 0,
            "success": False,
            "raw": None,
            "timestamp": None
        }

        try:
            ser = serial.Serial(self.port, baudrate=self.baudrate, bytesize=8, parity="N", stopbits=1, timeout=self.timeout)
        except Exception as e:
            result["error"] = f"Serial error: {e}"
            return result

        for attempt in range(1, max_attempts + 1):
            ser.reset_input_buffer()
            ser.write(bytearray(cmd))
            ser.flush()
            resp = ser.read(8)
            result["raw"] = list(resp)
            result["attempts"] = attempt

            if len(resp) != 8:
                time.sleep(delay_between)
                continue

         
            resp_frame = list(resp[:6])
            crc_calc = self.modbus_crc(resp_frame)
            crc_l, crc_h = resp[6], resp[7]

            if (crc_l != (crc_calc & 0xFF)) or (crc_h != ((crc_calc >> 8) & 0xFF)):
                result["crc_error"] = True
                time.sleep(delay_between)
                continue

            result["crc_error"] = False
            result["success"] = True
            result["timestamp"] = int(time.time())
            result["message"] = f"Address reset from 0x{self.slave_address:02X} to default 0x{default_address:02X}"
            self.slave_address = default_address
            break
        else:
            result["success"] = False

        ser.close()
        return result


if __name__ == "__main__":
    # ตัวอย่างการใช้งานฟังก์ชันเดิม
    ultra_sensor = UltrasonicModbus(port="/dev/ttyS2", slave_address=0x4E, baudrate=9600)
    ultra_data = ultra_sensor.read_distance()
    print("=== Read Distance ===")
    print(json.dumps(ultra_data, indent=2, ensure_ascii=False))
    
    # ตัวอย่างการใช้งานฟังก์ชันใหม่
    # print("\n=== Check Current Address ===")
    # addr_check = ultra_sensor.check_address()
    # print(json.dumps(addr_check, indent=2, ensure_ascii=False))
    
    # print("\n=== Change Address to 0x33 ===")
    # change_result = ultra_sensor.change_address(0x33)
    # print(json.dumps(change_result, indent=2, ensure_ascii=False))
    
    # # หลังเปลี่ยน address แล้ว ต้องใช้ address ใหม่
    # if change_result.get("success"):
    #     print("\n=== Check Address After Change ===")
    #     addr_check2 = ultra_sensor.check_address()
    #     print(json.dumps(addr_check2, indent=2, ensure_ascii=False))
    
    # print("\n=== Reset Address to Default (0x32) ===")
    # reset_result = ultra_sensor_sensor.reset_address()
    # print(json.dumps(reset_result, indent=2, ensure_ascii=False))
    
    # print("\n=== Check Address After Reset ===")
    # addr_check3 = ultra_sensor.check_address()
    # print(json.dumps(addr_check3, indent=2, ensure_ascii=False))