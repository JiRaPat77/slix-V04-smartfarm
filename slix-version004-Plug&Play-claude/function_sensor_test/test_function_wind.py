#Wind RK210-01 check specifically address
import os
import sys


current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from class_sensor.class_wind_modbus import SensorWindSpeedDirection

addr = 0x1A
sensor = SensorWindSpeedDirection("/dev/ttyS2", slave_address=addr)
value= sensor.read_wind(addr)
print(f"Address: 0x{addr:02X} | {value}")

#Wind sensor set address
# sensor = SensorWindSpeedDirection(port="/dev/ttyS2", slave_address=0x01)
# sensor.set_address(0x1A)
# sensor.close()