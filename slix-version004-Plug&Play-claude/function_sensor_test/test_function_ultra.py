#!/usr/bin/env python3
import sys
import os
import json
import time

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from class_sensor.class_ultra_modbus import UltrasonicModbus


sensor = UltrasonicModbus(port="/dev/ttyS2", slave_address=0x4E, baudrate=9600)

#Read sensor
try:
    while True:
        try:
            data = sensor.read_distance()
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            # print(f"Read Success: Distance={data['distance']} cm")
            print(json.dumps(data, indent=2, ensure_ascii=False))
        except Exception as e:
            print(f"Error reading data: {e}")
            
        time.sleep(2)  
        
except KeyboardInterrupt:
        print("\nStopping sensor reading...")


#Change address
# print("\n=== Change Address ===")
# change_result = sensor.change_address(0x4E)
# print(json.dumps(change_result, indent=2, ensure_ascii=False))
 

# if change_result.get("success"):
#     print("\n=== Check Address After Change ===")
#     addr_check2 = sensor.check_address()
#     print(json.dumps(addr_check2, indent=2, ensure_ascii=False))




#Reset address
# print("\n=== Reset Address to Default (0x32) ===")
# reset_result = rain_sensor.reset_address()
# print(json.dumps(reset_result, indent=2, ensure_ascii=False))
    
# print("\n=== Check Address After Reset ===")
# addr_check3 = rain_sensor.check_address()
# print(json.dumps(addr_check3, indent=2, ensure_ascii=False))
