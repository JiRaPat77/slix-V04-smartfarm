#!/usr/bin/env python3
"""
RK500-22 Soil pH Sensor Library
RIKA Soil pH Sensor Class

Based on MODBUS RTU Communication Protocol:
- Baud rate: 9600bps
- Data bits: 8  
- Stop bit: 1
- Check bit: no
- Default slave address: 0x03

Communication Examples:
- Read pH & Temperature: 03 03 00 00 00 06 C42A
- Response: 03 03 0C 40 E0 51 EC C0 89 99 9A 41 C9 47 AE AECE
- pH: 40E051EC = 7.01
- Temperature: 41C947AE = 25.16°C
- Change Address: 03 06 00 14 00 01 09EC
"""

import struct
import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from Modbus_485 import Modbus_Film69

class SensorSoilPHRK500_22:
    def __init__(self, port="/dev/ttyS2", slave_address=3, baudrate=9600):
        """
        Initialize RK500-22 Soil pH Sensor
        
        Args:
            port (str): Serial port path
            slave_address (int): Modbus slave address (1-247, default: 3)
            baudrate (int): Communication baud rate (default: 9600)
        """
        self.slave_address = slave_address
        self.port = port
        self.baudrate = baudrate
        
        # Initialize Modbus communication
        self.modbus = Modbus_Film69(port=port, slaveaddress=slave_address, baudrate=baudrate)
        
        print(f"RK500-22 Soil pH Sensor initialized on {port} with address 0x{slave_address:02X}")

    def read_data(self, addr=None):
        """
        Read pH and Temperature from RK500-22 sensor
        
        Command: 03 03 00 00 00 06 C42A
        Response: 03 03 0C 40E051EC C089999A 41C947AE AECE
        
        Data structure (IEEE 754 floating point):
        - Bytes 3-6: pH value (0-14)
        - Bytes 7-10: Unknown parameter
        - Bytes 11-14: Temperature (°C)
        
        Args:
            addr (int, optional): Override slave address for this read
            
        Returns:
            dict: {"ph_value": float, "temperature": float, "success": bool, "raw_data": dict} or None
        """
        try:
            address = addr if addr is not None else self.slave_address
            
            # Modbus command: Read Holding Register starting at 0x0000, count 6
            cmd = f"{address:02X} 03 00 00 00 06"
            
            # Expected response length: Address(1) + Function(1) + Byte Count(1) + Data(12) + CRC(2) = 17 bytes
            response, _ = self.modbus.send(cmd, resopne_len=17, ID=address)
            
            parts = response.split()
            if len(parts) < 17:
                raise ValueError(f"Invalid response length: {len(parts)}, expected 17")
            
            # Check function code
            if parts[1] != "03":
                raise ValueError(f"Invalid function code: {parts[1]}, expected 03")
            
            # Check data byte count
            data_count = int(parts[2], 16)
            if data_count != 12:  # 0x0C = 12 bytes
                raise ValueError(f"Invalid data count: {data_count}, expected 12")
            
            # Extract floating point data (IEEE 754 format)
            # Each float is 4 bytes (32-bit)
            
            # pH Value (bytes 3-6): 40 E0 51 EC = 7.01
            ph_bytes = bytes.fromhex("".join(parts[3:7]))
            ph_value = struct.unpack(">f", ph_bytes)[0]  # Big-endian float
      
            # Parameter (bytes 7-10) - Unknown parameter
            param_bytes = bytes.fromhex("".join(parts[7:11]))
            param = struct.unpack(">f", param_bytes)[0]
            
            # Temperature (bytes 11-14): 41 C9 47 AE = 25.16°C
            temp_bytes = bytes.fromhex("".join(parts[11:15]))
            temperature = struct.unpack(">f", temp_bytes)[0]

            if (ph_value < 0 or ph_value > 14) or (temperature < 0 or temperature > 80):
                raise ValueError(f"Sensor incorrect value (PH: {ph_value}, Temperature: {temperature})")
            
            result = {
                "ph_value": ph_value,            # pH (0-14)
                "temperature": temperature,      # Temperature in °C
                "parameter": param,              # Unknown parameter
                "success": True,
                "raw_data": {
                    "ph_bytes": parts[3:7],
                    "param_bytes": parts[7:11],
                    "temp_bytes": parts[11:15],
                    "response_parts": parts
                }
            }
            
            print(f"Soil pH: {ph_value:.2f}, Temperature: {temperature:.1f}°C")
            return result
            
        except Exception as e:
            print(f"Read sensor data failed: {e}")
            return {
                "ph_value": None,
                "temperature": None,
                "parameter": None,
                "success": False,
                "error": str(e)
            }

    def read_ph_only(self, addr=None):
        """
        Read only pH value (simplified method)
        
        Returns:
            float: pH value (0-14), or None if failed
        """
        result = self.read_data(addr)
        if result and result["success"]:
            return result["ph_value"]
        return None

    def read_temperature_only(self, addr=None):
        """
        Read only Temperature value (simplified method)
        
        Returns:
            float: Temperature in °C, or None if failed
        """
        result = self.read_data(addr)
        if result and result["success"]:
            return result["temperature"]
        return None

    def set_address(self, new_address):
        """
        Set new slave address for RK500-22 sensor
        
        Command: 03 06 00 14 00 01 09EC (example: change from 0x03 to 0x01)
        Response: 03 06 00 14 00 01 09EC
        
        Args:
            new_address (int): New slave address (1-247)
            
        Returns:
            bool: True if successful, False otherwise
        """
        if not (1 <= new_address <= 247):
            raise ValueError("Address must be between 1 and 247")
        
        try:
            print(f"Setting address from 0x{self.slave_address:02X} to 0x{new_address:02X}")
            
            # Write to register 0x0014 (20 decimal) to set new address
            cmd = f"{self.slave_address:02X} 06 00 14 00 {new_address:02X}"
            response, _ = self.modbus.send(cmd, resopne_len=8, ID=self.slave_address)
            print(f"Set address response: {response}")
            
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
        Test communication with RK500-22 sensor
        
        Args:
            addr (int, optional): Override slave address for test
            
        Returns:
            bool: True if communication successful, False otherwise
        """
        print("Testing communication with RK500-22 sensor...")
        
        result = self.read_data(addr)
        if result and result.get("success"):
            print(f"Communication OK - pH: {result['ph_value']:.2f}, Temp: {result['temperature']:.1f}°C")
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
            "model": "RK500-22",
            "manufacturer": "RIKA",
            "type": "Soil pH Sensor",
            "communication": "MODBUS RTU",
            "port": self.port,
            "slave_address": f"0x{self.slave_address:02X}",
            "baudrate": self.baudrate,
            "accuracy": "±0.02 pH",
            "resolution": "0.01 pH",
            "response_time": "<10s",
            "stability": "≤0.01 pH/24h",
            "range": "0-14 pH",
            "protection": "IP67",
            "operating_temperature": "0-+80°C"
        }

    def close(self):
        """Close Modbus connection"""
        if hasattr(self.modbus, 'close'):
            self.modbus.close()
        print("RK500-22 sensor connection closed")

    @staticmethod
    def scan_addresses(port="/dev/ttyS2", baudrate=9600):
        """
        Scan for RK500-22 sensors on the Modbus network
        
        Args:
            port (str): Serial port path
            baudrate (int): Communication baud rate
            
        Returns:
            list: List of found device addresses
        """
        found_devices = []
        modbus = Modbus_Film69(port=port, slaveaddress=1, baudrate=baudrate)
        
        print("Scanning for RK500-22 Soil pH Sensors (0x01 to 0xF7)...")
        
        for addr in range(1, 248):
            try:
                # Try to read pH data from each address
                cmd = f"{addr:02X} 03 00 00 00 06"
                response, _ = modbus.send(cmd, resopne_len=17, ID=addr)
                
                parts = response.split()
                if len(parts) >= 17 and parts[1] == "03" and parts[2] == "0C":
                    # Extract pH value
                    ph_bytes = bytes.fromhex("".join(parts[3:7]))
                    ph_value = struct.unpack(">f", ph_bytes)[0]
                    
                    # Extract Temperature value
                    temp_bytes = bytes.fromhex("".join(parts[11:15]))
                    temperature = struct.unpack(">f", temp_bytes)[0]
                    
                    print(f"Found RK500-22 at address 0x{addr:02X} | pH: {ph_value:.2f}, Temp: {temperature:.1f}°C")
                    found_devices.append(addr)
                    
            except Exception:
                # No response or invalid response - continue scanning
                pass
        
        modbus.close()
        
        if found_devices:
            print(f"Found {len(found_devices)} RK500-22 sensor(s)")
        else:
            print("No RK500-22 sensors found")
            
        return found_devices

    @staticmethod
    def convert_current_to_ph(current_ma):
        """
        Convert 4-20mA current output to pH value
        
        Formula from manual: pH = (I-4)/16 * 14
        where I = output current (mA)
        
        Args:
            current_ma (float): Current reading in mA (4-20mA)
            
        Returns:
            float: pH value (0-14)
        """
        if not (4.0 <= current_ma <= 20.0):
            raise ValueError("Current must be between 4.0 and 20.0 mA")
        
        ph_value = (current_ma - 4) / 16 * 14
        return ph_value

    @staticmethod
    def convert_voltage_to_ph(voltage_v, full_scale_v=5.0):
        """
        Convert voltage output to pH value
        
        Formula from manual: pH = V/V_full_scale * 14
        where V = output voltage (V), V_full_scale = 5V
        
        Args:
            voltage_v (float): Voltage reading in V
            full_scale_v (float): Full scale voltage (default: 5V)
            
        Returns:
            float: pH value (0-14)
        """
        if voltage_v < 0 or voltage_v > full_scale_v:
            raise ValueError(f"Voltage must be between 0 and {full_scale_v}V")
        
        ph_value = voltage_v / full_scale_v * 14
        return ph_value

    @staticmethod
    def classify_soil_ph(ph_value):
        """
        Classify soil pH level based on pH value
        
        Classification (Agricultural standards):
        - Strongly acidic: pH < 4.5
        - Acidic: pH 4.5-5.5
        - Slightly acidic: pH 5.5-6.5
        - Neutral: pH 6.5-7.5
        - Slightly alkaline: pH 7.5-8.5
        - Alkaline: pH 8.5-9.5
        - Strongly alkaline: pH > 9.5
        
        Args:
            ph_value (float): pH value (0-14)
            
        Returns:
            dict: {"level": str, "description": str, "suitable_crops": list, "recommendations": list}
        """
        if ph_value is None or ph_value < 0 or ph_value > 14:
            return {"level": "Invalid", "description": "Invalid pH value", 
                    "suitable_crops": [], "recommendations": []}
        
        if ph_value < 4.5:
            return {
                "level": "Strongly acidic",
                "description": "Very acidic soil, most crops cannot survive",
                "suitable_crops": ["Blueberries", "Azaleas", "Rhododendrons"],
                "recommendations": ["Add lime", "Use organic matter", "Check aluminum toxicity"]
            }
        elif ph_value < 5.5:
            return {
                "level": "Acidic",
                "description": "Acidic soil, limited crop selection",
                "suitable_crops": ["Potatoes", "Sweet potatoes", "Berries", "Conifers"],
                "recommendations": ["Add lime gradually", "Monitor nutrient availability", "Test for heavy metals"]
            }
        elif ph_value < 6.5:
            return {
                "level": "Slightly acidic",
                "description": "Slightly acidic, good for many crops",
                "suitable_crops": ["Tomatoes", "Carrots", "Beans", "Most vegetables"],
                "recommendations": ["Good for most crops", "Monitor phosphorus availability"]
            }
        elif ph_value <= 7.5:
            return {
                "level": "Neutral",
                "description": "Optimal pH for most crops",
                "suitable_crops": ["Almost all crops", "Vegetables", "Grains", "Legumes"],
                "recommendations": ["Ideal conditions", "Maintain current pH level"]
            }
        elif ph_value <= 8.5:
            return {
                "level": "Slightly alkaline",
                "description": "Slightly alkaline, some nutrient issues may occur",
                "suitable_crops": ["Cabbage", "Beets", "Spinach", "Asparagus"],
                "recommendations": ["Monitor iron availability", "Add organic matter", "Check for salt buildup"]
            }
        elif ph_value <= 9.5:
            return {
                "level": "Alkaline",
                "description": "Alkaline soil, nutrient deficiencies likely",
                "suitable_crops": ["Beets", "Cabbage", "Some grasses"],
                "recommendations": ["Add sulfur", "Use acidifying fertilizers", "Improve drainage"]
            }
        else:
            return {
                "level": "Strongly alkaline",
                "description": "Very alkaline, most crops cannot survive",
                "suitable_crops": ["Very few plants", "Salt-tolerant species"],
                "recommendations": ["Major soil amendment needed", "Add sulfur", "Improve drainage", "Consider raised beds"]
            }

    @staticmethod
    def get_optimal_ph_for_crop(crop_name):
        """
        Get optimal pH range for specific crops
        
        Args:
            crop_name (str): Name of the crop
            
        Returns:
            dict: {"crop": str, "optimal_ph_range": tuple, "description": str}
        """
        crop_ph_ranges = {
            # Vegetables
            "tomato": (6.0, 6.8, "Tomatoes prefer slightly acidic soil"),
            "potato": (4.8, 5.4, "Potatoes grow best in acidic soil"),
            "carrot": (5.5, 7.0, "Carrots tolerate a wide pH range"),
            "cabbage": (6.0, 7.5, "Cabbage prefers neutral to slightly alkaline soil"),
            "lettuce": (6.0, 7.0, "Lettuce grows best in neutral soil"),
            "spinach": (6.5, 7.5, "Spinach prefers neutral to slightly alkaline soil"),
            
            # Grains
            "wheat": (6.0, 7.0, "Wheat grows best in neutral soil"),
            "corn": (5.8, 6.8, "Corn prefers slightly acidic to neutral soil"),
            "rice": (5.0, 6.5, "Rice tolerates acidic conditions"),
            
            # Legumes
            "soybean": (6.0, 6.8, "Soybeans prefer neutral soil"),
            "pea": (6.0, 7.0, "Peas grow best in neutral soil"),
            "bean": (6.0, 7.0, "Beans prefer neutral soil"),
            
            # Fruits
            "apple": (5.8, 6.5, "Apples prefer slightly acidic soil"),
            "blueberry": (4.0, 5.2, "Blueberries require acidic soil"),
            "strawberry": (5.5, 6.5, "Strawberries prefer slightly acidic soil"),
        }
        
        crop_lower = crop_name.lower()
        if crop_lower in crop_ph_ranges:
            min_ph, max_ph, description = crop_ph_ranges[crop_lower]
            return {
                "crop": crop_name.title(),
                "optimal_ph_range": (min_ph, max_ph),
                "description": description
            }
        else:
            return {
                "crop": crop_name.title(),
                "optimal_ph_range": (6.0, 7.0),
                "description": "Most crops prefer neutral soil (pH 6.0-7.0)"
            }


# Example usage and testing
if __name__ == "__main__":
    # Create sensor instance
    sensor = SensorSoilPHRK500_22(port="/dev/ttyS4", slave_address=0x03, baudrate=9600)
    
    # Test communication
    if sensor.test_communication():
        # Read all sensor data
        data = sensor.read_data()
        if data and data["success"]:
            ph = data["ph_value"]
            temp = data["temperature"]
            
            print(f"\nMeasurement Results:")
            print(f"  pH: {ph:.2f}")
            print(f"  Temperature: {temp:.1f}°C")
            
            # Classify soil pH
            classification = SensorSoilPHRK500_22.classify_soil_ph(ph)
            print(f"\nSoil Classification:")
            print(f"  Level: {classification['level']}")
            print(f"  Description: {classification['description']}")
            print(f"  Suitable crops: {', '.join(classification['suitable_crops'])}")
            print(f"  Recommendations: {', '.join(classification['recommendations'])}")
            
            # Check optimal pH for specific crop
            crop_info = SensorSoilPHRK500_22.get_optimal_ph_for_crop("tomato")
            print(f"\nCrop Analysis (Tomato):")
            print(f"  Optimal pH range: {crop_info['optimal_ph_range'][0]:.1f} - {crop_info['optimal_ph_range'][1]:.1f}")
            print(f"  Current pH suitable: {'Yes' if crop_info['optimal_ph_range'][0] <= ph <= crop_info['optimal_ph_range'][1] else 'No'}")
        
        # Display sensor info
        info = sensor.get_sensor_info()
        print("\nSensor Information:")
        for key, value in info.items():
            print(f"  {key}: {value}")
    
    # Scan for devices (optional)
    # found = SensorSoilPHRK500_22.scan_addresses()
    
    # Close connection
    sensor.close()
