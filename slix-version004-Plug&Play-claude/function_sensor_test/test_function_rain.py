import json
import sys
import os

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)

from class_sensor.class_rain_modbus import RainTipModbus

rain_sensor = RainTipModbus(port="/dev/ttyS2", slave_address=0x32, baudrate=9600)


# อ่าน sensor
rain_data = rain_sensor.read_tip()
print(json.dumps(rain_data, indent=2, ensure_ascii=False))


# print("\n=== Check Current Address ===")
# addr_check = rain_sensor.check_address()
# print(json.dumps(addr_check, indent=2, ensure_ascii=False))



    
# print("\n=== Change Address to 0x33 ===")
# change_result = rain_sensor.change_address(0x32)
# print(json.dumps(change_result, indent=2, ensure_ascii=False))
 
# # หลังเปลี่ยน address แล้ว ต้องใช้ address ใหม่
# if change_result.get("success"):
#     print("\n=== Check Address After Change ===")
#     addr_check2 = rain_sensor.check_address()
#     print(json.dumps(addr_check2, indent=2, ensure_ascii=False))



    
# print("\n=== Reset Address to Default (0x32) ===")
# reset_result = rain_sensor.reset_address()
# print(json.dumps(reset_result, indent=2, ensure_ascii=False))
    
# print("\n=== Check Address After Reset ===")
# addr_check3 = rain_sensor.check_address()
# print(json.dumps(addr_check3, indent=2, ensure_ascii=False))