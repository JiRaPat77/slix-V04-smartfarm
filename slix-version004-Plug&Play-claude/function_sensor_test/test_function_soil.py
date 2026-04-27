#Soil RK520-01 check specifically address

## Read once time
# from class_soil_modbus import SensorSoilMoistureTemp
# addr = 0x02
# sensor = SensorSoilMoistureTemp("/dev/ttyS2", slave_address=addr)
# value= sensor.read_data(addr)
# print(f"Address: 0x{addr:02X} | {value}")

# Read continuously every 5 seconds
import time
import sys
import os

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from class_sensor.class_soil_modbus import SensorSoilMoistureTemp

addr = 0x01
sensor = SensorSoilMoistureTemp("/dev/ttyS2", slave_address=addr)

print("Starting sensor reading... Press Ctrl+C to stop")

try:
    while True:
        try:
            value = sensor.read_data(addr)
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            print(f"[{timestamp}] Address: 0x{addr:02X} | {value}")
        except Exception as e:
            print(f"Error reading data: {e}")
        
        time.sleep(5)  
        
except KeyboardInterrupt:
    print("\nStopping sensor reading...")





#Soil RK520-01 check respond ALL address

# import sys
# import os

# current_dir = os.path.dirname(os.path.abspath(__file__))
# parent_dir = os.path.dirname(current_dir)
# sys.path.append(parent_dir)
# from class_sensor.class_soil_modbus import SensorSoilMoistureTemp
# if __name__ == "__main__":


#     devices = SensorSoilMoistureTemp.scan_addresses("/dev/ttyS2")

#     all_data = []

#     for addr in devices:
#         sensor = SensorSoilMoistureTemp("/dev/ttyS2", slave_address=addr)
#         data = sensor.read_data()
#         sensor.close()

#         if data is not None:
#             entry = {
#                 "address": f"0x{addr:02X}",
#                 "temperature": data["temperature"],
#                 "moisture": data["moisture"]
#             }
#             all_data.append(entry)

#     import json
#     print(json.dumps(all_data, indent=2))


#Soil set address

# import sys
# import os
# import time

# current_dir = os.path.dirname(os.path.abspath(__file__))
# parent_dir = os.path.dirname(current_dir)
# sys.path.append(parent_dir)
# from class_sensor.class_soil_modbus import SensorSoilMoistureTemp


# if __name__ == "__main__":

#     current_addr = 0x03
#     new_addr = 0x01

#     # สร้าง instance
#     sensor = SensorSoilMoistureTemp("/dev/ttyS2", slave_address=current_addr)


#     print(f"Setting sensor at address 0x{current_addr:02X} to new address 0x{new_addr:02X}")
#     sensor.set_address(new_addr)
#     sensor.close()

#     print("Please restart sensor before use new address")


#     time.sleep(5)


#     sensor_new = SensorSoilMoistureTemp("/dev/ttyS2", slave_address=new_addr)
#     data = sensor_new.read_data()
#     sensor_new.close()

#     if data:
#         print(f"Read from new address 0x{new_addr:02X}:")
#         print(f"Temperature: {data['temperature']} °C")
#         print(f"Moisture: {data['moisture']} %")
#     else:
#         print("Failed to read from new address.")

