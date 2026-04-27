#Pyranometer check specifically address and value
import sys
import os

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from class_sensor.class_solar_modbus import SensorPyranometer

addr = 0x4F
sensor = SensorPyranometer("/dev/ttyS2", slave_address=addr)
value= sensor.read_radiation(addr)
print(f"Address: 0x{addr:02X} | {value}")
current = sensor.read_current_address()
print(f"Current address: 0x{current:02X}")


#Pyranometer set address
# import sys
# import os
# addr = 0x0E

# current_dir = os.path.dirname(os.path.abspath(__file__))
# parent_dir = os.path.dirname(current_dir)
# sys.path.append(parent_dir)
# from class_sensor.class_solar_modbus import SensorPyranometer
# sensor = SensorPyranometer("/dev/ttyS2", slave_address=addr)
# sensor.set_address(0x4F)
# print("Success Set Address")