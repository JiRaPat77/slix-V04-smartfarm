#!/usr/bin/env python3
"""
RKL-01 Submersible Liquid Level Transmitter Library
RIKA Water Level Sensor Class

Based on MODBUS RTU Communication Protocol:
- Baud rate: 9600bps
- Data bits: 8  
- Stop bit: 1
- Check bit: no
- Default slave address: 0x01

Communication Examples:
- Read Level: 01 03 00 04 00 01 C5CB
- Response: 01 03 02 01 B4 B9A3 (Level = 4.36m)
- Set Address: 01 06 00 00 00 02 080B
- Save Address: 02 06 00 0F 00 00 B9FA
"""

import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from Modbus_485 import Modbus_Film69

class SensorWaterLevelRKL01:
    def __init__(self, port="/dev/ttyS2", slave_address=1, baudrate=9600):
        """
        Initialize RKL-01 Water Level Sensor
        
        Args:
            port (str): Serial port path
            slave_address (int): Modbus slave address (1-247)
            baudrate (int): Communication baud rate (default: 9600)
        """
        self.slave_address = slave_address
        self.port = port
        self.baudrate = baudrate
        
        # Initialize Modbus communication
        self.modbus = Modbus_Film69(port=port, slaveaddress=slave_address, baudrate=baudrate)
        
        print(f"RKL-01 Water Level Sensor initialized on {port} with address 0x{slave_address:02X}")

    def read_water_level(self, addr=None):
        """
        Read water level from RKL-01 sensor
        
        Command: 01 03 00 04 00 01 C5CB
        Response: 01 03 02 01 B4 B9A3
        Level calculation: (01B4)H = (436)D, 436/100 = 4.36m
        
        Args:
            addr (int, optional): Override slave address for this read
            
        Returns:
            dict: {"water_level": float, "raw_value": int, "success": bool} or None
        """
        try:
            address = addr if addr is not None else self.slave_address
            
            # Modbus command: Read Holding Register at 0x0004, count 1
            cmd = f"{address:02X} 03 00 04 00 01"
            
            # Expected response length: Address(1) + Function(1) + Byte Count(1) + Data(2) + CRC(2) = 7 bytes
            response, _ = self.modbus.send(cmd, resopne_len=7, ID=address)
            
            parts = response.split()
            if len(parts) < 7:
                raise ValueError(f"Invalid response length: {len(parts)}, expected 7")
            
            # Check function code
            if parts[1] != "03":
                raise ValueError(f"Invalid function code: {parts[1]}, expected 03")
            
            # Extract water level data (2 bytes)
            level_high = parts[3]
            level_low = parts[4]
            
            # Combine high and low bytes to get raw value
            raw_value = int(level_high + level_low, 16)

            if raw_value < 0 or raw_value > 5000:
                print(f"Warning: raw_value ({raw_value}) out of range send value is 0")
                water_level = 0
            else:
                sensor_plant_height = 73  # Offset in cm (50 + 23 :50 คือโคนต้นถึงผิวน้ำ 23 คือระยะที่ sensor อยู่)
                sensor_reading = (raw_value / 10.0)
                water_level = (sensor_plant_height - sensor_reading)
                
                if water_level < 0:
                    water_level = 0
            
            result = {
                "water_level": water_level,
                "raw_value": raw_value,
                "success": True,
                "response_parts": parts
            }
            
            print(f"Water level read: {water_level:.2f}m (raw: {raw_value})")
            return result
            
        except Exception as e:
            print(f"Read water level failed: {e}")
            return {
                "water_level": None,
                "raw_value": None,
                "success": False,
                "error": str(e)
            }

    def set_address(self, new_address):
        """
        Set new slave address for RKL-01 sensor
        
        Step 1: Set new address - 01 06 00 00 00 02 080B
        Step 2: Save address - 02 06 00 0F 00 00 B9FA
        
        Args:
            new_address (int): New slave address (1-247)
            
        Returns:
            bool: True if successful, False otherwise
        """
        if not (1 <= new_address <= 247):
            raise ValueError("Address must be between 1 and 247")
        
        try:
            # Step 1: Set new address at register 0x0000
            print(f"Setting address from 0x{self.slave_address:02X} to 0x{new_address:02X}")
            
            cmd_set = f"{self.slave_address:02X} 06 00 00 00 {new_address:02X}"
            response1, _ = self.modbus.send(cmd_set, resopne_len=8, ID=self.slave_address)
            print(f"Set address response: {response1}")
            
            # Step 2: Save the new address at register 0x000F
            print("Saving new address...")
            
            cmd_save = f"{new_address:02X} 06 00 0F 00 00"
            response2, _ = self.modbus.send(cmd_save, resopne_len=8, ID=new_address)
            print(f"Save address response: {response2}")
            
            # Update internal address
            self.slave_address = new_address
            self.modbus.slaveaddress = new_address
            
            print(f"Address successfully changed to 0x{new_address:02X}")
            return True
            
        except Exception as e:
            print(f"Set address failed: {e}")
            return False

    def test_communication(self, addr=None):
        """
        Test communication with RKL-01 sensor
        
        Args:
            addr (int, optional): Override slave address for test
            
        Returns:
            bool: True if communication successful, False otherwise
        """
        print("Testing communication with RKL-01 sensor...")
        
        result = self.read_water_level(addr)
        if result and result.get("success"):
            print(f"Communication OK - Water level: {result['water_level']:.2f}m")
            return True
        else:
            print("Communication failed")
            return False

    def get_sensor_info(self):
        """
        Get sensor information
        
        Returns:
            dict: Sensor specifications and current settings
        """
        return {
            "model": "RKL-01",
            "manufacturer": "RIKA",
            "type": "Submersible Liquid Level Transmitter",
            "communication": "MODBUS RTU",
            "port": self.port,
            "slave_address": f"0x{self.slave_address:02X}",
            "baudrate": self.baudrate,
            "accuracy": "0.1%FS - 0.5%FS",
            "range": "0~0.5m to 200mH2O",
            "protection": "IP68",
            "temperature_range": "-40°C to +80°C"
        }

    def close(self):
        """Close Modbus connection"""
        if hasattr(self.modbus, 'close'):
            self.modbus.close()
        print("RKL-01 sensor connection closed")

    @staticmethod
    def scan_addresses(port="/dev/ttyS2", baudrate=9600):
        """
        Scan for RKL-01 sensors on the Modbus network
        
        Args:
            port (str): Serial port path
            baudrate (int): Communication baud rate
            
        Returns:
            list: List of found device addresses
        """
        found_devices = []
        modbus = Modbus_Film69(port=port, slaveaddress=1, baudrate=baudrate)
        
        print("Scanning for RKL-01 Water Level Sensors (0x01 to 0xF7)...")
        
        for addr in range(1, 248):
            try:
                # Try to read water level from each address
                cmd = f"{addr:02X} 03 00 04 00 01"
                response, _ = modbus.send(cmd, resopne_len=7, ID=addr)
                
                parts = response.split()
                if len(parts) >= 7 and parts[1] == "03":
                    # Extract water level
                    raw_value = int(parts[3] + parts[4], 16)
                    water_level = raw_value / 100.0
                    
                    print(f"Found RKL-01 at address 0x{addr:02X} | Water Level: {water_level:.2f}m")
                    found_devices.append(addr)
                    
            except Exception:
                # No response or invalid response - continue scanning
                pass
        
        modbus.close()
        
        if found_devices:
            print(f"Found {len(found_devices)} RKL-01 sensor(s)")
        else:
            print("No RKL-01 sensors found")
            
        return found_devices

    @staticmethod
    def calculate_level_from_current(current_ma, scale_range_m):
        """
        Calculate water level from 4-20mA current output
        
        Formula: H = (I-4)/(20-4) * Scale_range
        
        Args:
            current_ma (float): Current reading in mA (4-20mA)
            scale_range_m (float): Full scale range in meters
            
        Returns:
            float: Water level in meters
        """
        if not (4.0 <= current_ma <= 20.0):
            raise ValueError("Current must be between 4.0 and 20.0 mA")
        
        level = (current_ma - 4.0) / (20.0 - 4.0) * scale_range_m
        return level

    @staticmethod
    def calculate_level_from_voltage(voltage_v, full_scale_v, scale_range_m):
        """
        Calculate water level from voltage output (0-5V or 0-10V)
        
        Formula: H = U / (full_scale_voltage - zero_point_voltage) * Scale_range
        
        Args:
            voltage_v (float): Voltage reading in V
            full_scale_v (float): Full scale voltage (5V or 10V)
            scale_range_m (float): Full scale range in meters
            
        Returns:
            float: Water level in meters
        """
        if voltage_v < 0 or voltage_v > full_scale_v:
            raise ValueError(f"Voltage must be between 0 and {full_scale_v}V")
        
        level = voltage_v / full_scale_v * scale_range_m
        return level


# Example usage and testing
if __name__ == "__main__":
    # Create sensor instance
    sensor = SensorWaterLevelRKL01(port="/dev/ttyS2", slave_address=0x25, baudrate=9600)
    
    # Test communication
    if sensor.test_communication():
        # Read water level
        level_data = sensor.read_level()
        if level_data and level_data["success"]:
            print(f"Current water level: {level_data['water_level']:.2f} meters")
        
        # Display sensor info
        info = sensor.get_sensor_info()
        print("\nSensor Information:")
        for key, value in info.items():
            print(f"  {key}: {value}")
    
    # Scan for devices (optional)
    # found = SensorWaterLevelRKL01.scan_addresses()
    
    # Close connection
    sensor.close()
