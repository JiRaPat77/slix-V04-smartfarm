#!/usr/bin/env python3
import time
import sys
import os

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from class_sensor.class_temp_modbus import SensorAirTempHumidityRS30


PORT = "/dev/ttyS2" 
ADDR = 14        # Address ของ Sensor
BAUD = 9600     # Baudrate ของ Sensor


def main():
    print(f"--- Start Testing Sensor on {PORT} (Addr: {ADDR}) ---")
    sensor = SensorAirTempHumidityRS30(port=PORT, slave_address=ADDR, baudrate=BAUD)

    #Read Sensor
    try:
        while True:
            try:
                value = sensor.read_temp()
                timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
                print(f"Read Success: Temp={value['temperature']}°C, Hum={value['humidity']}%")
            except Exception as e:
                print(f"Error reading data: {e}")
            
            time.sleep(2)  
        
    except KeyboardInterrupt:
        print("\nStopping sensor reading...")

    # Check Address
    # real_addr = sensor.check_address()
    # if real_addr:
    #     print(f"Found Sensor Address: {real_addr}")

    # Chance Address
    # NEW_ADDR = 14
    # if sensor.set_address(NEW_ADDR):
    #     print(f"Address changed to {NEW_ADDR}")

    # Chance Baudrate
    # try:
    #     sensor.set_baudrate(9600)

    # except ValueError as e:
    #     print(f"Error: {e}")
    # except Exception as e:
    #     print(f"Failed to change Slave ID: {e}")

    # # Reset to default address
    # sensor.reset_to_default()

if __name__ == "__main__":
    main()