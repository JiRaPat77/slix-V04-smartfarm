import sys
import os
import time

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)

from class_sensor.class_RKL01 import SensorWaterLevelRKL01 


sensor = SensorWaterLevelRKL01(port="/dev/ttyS2", slave_address=0x25)

#read value
# if sensor.test_communication():
#         # Read water level
#         level_data = sensor.read_water_level()
#         if level_data and level_data["success"]:
#             print(f"Current water level: {level_data['water_level']:.2f} meters")
        
#         # Display sensor info
#         info = sensor.get_sensor_info()
#         print("\nSensor Information:")
#         for key, value in info.items():
#             print(f"  {key}: {value}")

try:
     while True:
        try:
            level_data = sensor.read_water_level()
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            print(f"Current water level: {level_data['water_level']:.2f} meters")

            info = sensor.get_sensor_info()
            print("\nSensor Information:")
            for key, value in info.items():
                print(f"  {key}: {value}")

        except Exception as e:
            print(f"Error reading data: {e}")
            
        time.sleep(2)  
        
except KeyboardInterrupt:
     print("\nStopping sensor reading...")



#Chance address
# sensor.set_address(0x25)   # 37-48 (0x25-0x30)

#scan address
# found = SensorWaterLevelRKL01.scan_addresses()
