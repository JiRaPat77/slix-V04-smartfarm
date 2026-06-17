#!/usr/bin/env python3
"""
RK500-23 Soil EC & Salinity Sensor Library
RIKA Soil Electrical Conductivity and Salinity Sensor Class

Based on MODBUS RTU Communication Protocol:
- Baud rate: 9600bps
- Data bits: 8  
- Stop bit: 1
- Check bit: no
- Default slave address: 0x04

Communication Examples:
- Read EC & Salinity: 04 03 00 00 00 0A C598
- Response: 04 03 14 3F 82 DC 81 41 1C 80 64 41 DC F9 0C 43 FF 96 AC 44 0C 92 DF E1 37
- EC: (3F82DC81) = 1.022 mS/cm
- Salinity: (440C92DF) = 562 PPM
- Change Address: 0C 06 00 14 00 01 0913
"""

import struct
import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from Modbus_485 import Modbus_Film69

class SensorSoilECRK500_23:
    def __init__(self, port="/dev/ttyS2", slave_address=4, baudrate=9600):
        """
        Initialize RK500-23 Soil EC & Salinity Sensor
        
        Args:
            port (str): Serial port path
            slave_address (int): Modbus slave address (1-247, default: 4)
            baudrate (int): Communication baud rate (default: 9600)
        """
        self.slave_address = slave_address
        self.port = port
        self.baudrate = baudrate
        
        # Initialize Modbus communication
        self.modbus = Modbus_Film69(port=port, slaveaddress=slave_address, baudrate=baudrate)
        
        print(f"RK500-23 Soil EC Sensor initialized on {port} with address 0x{slave_address:02X}")

    def read_data(self, addr=None):
        """
        Read EC and Salinity from RK500-23 sensor
        
        Command: 04 03 00 00 00 0A C598
        Response: 04 03 14 [20 bytes of float data]
        
        Data structure (IEEE 754 floating point):
        - Bytes 3-6: EC value (mS/cm)
        - Bytes 7-10: Unknown parameter 1
        - Bytes 11-14: Unknown parameter 2
        - Bytes 15-18: Unknown parameter 3  
        - Bytes 19-22: Salinity (PPM)
        
        Args:
            addr (int, optional): Override slave address for this read
            
        Returns:
            dict: {"ec_value": float, "salinity": float, "success": bool, "raw_data": dict} or None
        """
        try:
            address = addr if addr is not None else self.slave_address
            
            # Modbus command: Read Holding Register starting at 0x0000, count 10 (0x0A)
            cmd = f"{address:02X} 03 00 00 00 0A"
            
            # Expected response length: Address(1) + Function(1) + Byte Count(1) + Data(20) + CRC(2) = 25 bytes
            response, _ = self.modbus.send(cmd, resopne_len=25, ID=address)
            
            parts = response.split()
            if len(parts) < 25:
                raise ValueError(f"Invalid response length: {len(parts)}, expected 25")
            
            # Check function code
            if parts[1] != "03":
                raise ValueError(f"Invalid function code: {parts[1]}, expected 03")
            
            # Check data byte count
            data_count = int(parts[2], 16)
            if data_count != 20:  # 0x14 = 20 bytes
                raise ValueError(f"Invalid data count: {data_count}, expected 20")
            
            # Extract floating point data (IEEE 754 format)
            # Each float is 4 bytes (32-bit)
            
            # EC Value (bytes 3-6): 3F 82 DC 81 = 1.022 mS/cm
            ec_bytes = bytes.fromhex("".join(parts[3:7]))
            ec_value = struct.unpack(">f", ec_bytes)[0]  # Big-endian float
            
            # Parameter 1 (bytes 7-10)
            param1_bytes = bytes.fromhex("".join(parts[7:11]))
            param1 = struct.unpack(">f", param1_bytes)[0]
            
            # Parameter 2 (bytes 11-14)
            param2_bytes = bytes.fromhex("".join(parts[11:15]))
            param2 = struct.unpack(">f", param2_bytes)[0]
            
            # Parameter 3 (bytes 15-18)
            param3_bytes = bytes.fromhex("".join(parts[15:19]))
            param3 = struct.unpack(">f", param3_bytes)[0]
            
            # Salinity (bytes 19-22): 44 0C 92 DF = 562 PPM
            salinity_bytes = bytes.fromhex("".join(parts[19:23]))
            salinity = struct.unpack(">f", salinity_bytes)[0]
            
            if (ec_value < 0 or ec_value > 2000) or (salinity < 0 or salinity > 1000):
                raise ValueError(f"Sensor incorrect value (EC: {ec_value}, Salinity: {salinity})")
            
            result = {
                "ec_value": ec_value,        # mS/cm (milliSiemens per centimeter)
                "salinity": salinity,        # PPM (Parts Per Million)
                "parameter_1": param1,       # Unknown parameter
                "parameter_2": param2,       # Unknown parameter
                "parameter_3": param3,       # Unknown parameter
                "success": True,
                "raw_data": {
                    "ec_bytes": parts[3:7],
                    "param1_bytes": parts[7:11],
                    "param2_bytes": parts[11:15],
                    "param3_bytes": parts[15:19],
                    "salinity_bytes": parts[19:23],
                    "response_parts": parts
                }
            }
            
            print(f"Soil EC: {ec_value:.3f} mS/cm, Salinity: {salinity:.1f} PPM")
            return result
            
        except Exception as e:
            print(f"Read sensor data failed: {e}")
            return {
                "ec_value": None,
                "salinity": None,
                "parameter_1": None,
                "parameter_2": None,
                "parameter_3": None,
                "success": False,
                "error": str(e)
            }

    def read_ec_only(self, addr=None):
        """
        Read only EC value (simplified method)
        
        Returns:
            float: EC value in mS/cm, or None if failed
        """
        result = self.read_data(addr)
        if result and result["success"]:
            return result["ec_value"]
        return None

    def read_salinity_only(self, addr=None):
        """
        Read only Salinity value (simplified method)
        
        Returns:
            float: Salinity in PPM, or None if failed
        """
        result = self.read_data(addr)
        if result and result["success"]:
            return result["salinity"]
        return None

    def set_address(self, new_address):
        """
        Set new slave address for RK500-23 sensor
        
        Command: 0C 06 00 14 00 01 0913 (example: change from 0x0C to 0x01)
        Response: 0C 06 00 14 00 01 0913
        
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
        Test communication with RK500-23 sensor
        
        Args:
            addr (int, optional): Override slave address for test
            
        Returns:
            bool: True if communication successful, False otherwise
        """
        print("Testing communication with RK500-23 sensor...")
        
        result = self.read_data(addr)
        if result and result.get("success"):
            print(f"Communication OK - EC: {result['ec_value']:.3f} mS/cm, Salinity: {result['salinity']:.1f} PPM")
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
            "model": "RK500-23",
            "manufacturer": "RIKA",
            "type": "Soil EC & Salinity Sensor",
            "communication": "MODBUS RTU",
            "port": self.port,
            "slave_address": f"0x{self.slave_address:02X}",
            "baudrate": self.baudrate,
            "accuracy": "±1%",
            "response_time": "<1s",
            "range_ec": "0~20000 μS/cm",
            "range_salinity": "Calculated from EC",
            "electrode": "316L & Graphite",
            "temperature_compensation": "Automatic (0-150°C)",
            "protection": "IP68",
            "operating_temperature": "0-50°C"
        }

    def close(self):
        """Close Modbus connection"""
        if hasattr(self.modbus, 'close'):
            self.modbus.close()
        print("RK500-23 sensor connection closed")

    @staticmethod
    def scan_addresses(port="/dev/ttyS2", baudrate=9600):
        """
        Scan for RK500-23 sensors on the Modbus network
        
        Args:
            port (str): Serial port path
            baudrate (int): Communication baud rate
            
        Returns:
            list: List of found device addresses
        """
        found_devices = []
        modbus = Modbus_Film69(port=port, slaveaddress=1, baudrate=baudrate)
        
        print("Scanning for RK500-23 Soil EC Sensors (0x01 to 0xF7)...")
        
        for addr in range(1, 248):
            try:
                # Try to read EC data from each address
                cmd = f"{addr:02X} 03 00 00 00 0A"
                response, _ = modbus.send(cmd, resopne_len=25, ID=addr)
                
                parts = response.split()
                if len(parts) >= 25 and parts[1] == "03" and parts[2] == "14":
                    # Extract EC value
                    ec_bytes = bytes.fromhex("".join(parts[3:7]))
                    ec_value = struct.unpack(">f", ec_bytes)[0]
                    
                    # Extract Salinity value
                    salinity_bytes = bytes.fromhex("".join(parts[19:23]))
                    salinity = struct.unpack(">f", salinity_bytes)[0]
                    
                    print(f"Found RK500-23 at address 0x{addr:02X} | EC: {ec_value:.3f} mS/cm, Salinity: {salinity:.1f} PPM")
                    found_devices.append(addr)
                    
            except Exception:
                # No response or invalid response - continue scanning
                pass
        
        modbus.close()
        
        if found_devices:
            print(f"Found {len(found_devices)} RK500-23 sensor(s)")
        else:
            print("No RK500-23 sensors found")
            
        return found_devices

    @staticmethod
    def convert_ec_to_salinity_ppm(ec_ms_cm):
        """
        Convert EC (mS/cm) to Salinity (PPM) using standard conversion
        
        Common conversion formulas:
        - For dilute solutions: PPM ≈ EC(μS/cm) * 0.64
        - For seawater: PPM ≈ EC(mS/cm) * 640
        
        Args:
            ec_ms_cm (float): Electrical Conductivity in mS/cm
            
        Returns:
            float: Salinity in PPM
        """
        if ec_ms_cm is None or ec_ms_cm < 0:
            return None
        
        # Convert mS/cm to μS/cm first (multiply by 1000)
        ec_us_cm = ec_ms_cm * 1000
        
        # Standard conversion: PPM ≈ EC(μS/cm) * 0.64
        salinity_ppm = ec_us_cm * 0.64
        
        return salinity_ppm

    @staticmethod
    def convert_salinity_to_ec(salinity_ppm):
        """
        Convert Salinity (PPM) to EC (mS/cm)
        
        Args:
            salinity_ppm (float): Salinity in PPM
            
        Returns:
            float: Electrical Conductivity in mS/cm
        """
        if salinity_ppm is None or salinity_ppm < 0:
            return None
        
        # Reverse conversion: EC(μS/cm) = PPM / 0.64
        ec_us_cm = salinity_ppm / 0.64
        
        # Convert μS/cm to mS/cm (divide by 1000)
        ec_ms_cm = ec_us_cm / 1000
        
        return ec_ms_cm

    @staticmethod
    def classify_soil_salinity(ec_ms_cm):
        """
        Classify soil salinity level based on EC value
        
        Classification (FAO standards):
        - Non-saline: EC < 2 mS/cm
        - Slightly saline: 2-4 mS/cm  
        - Moderately saline: 4-8 mS/cm
        - Highly saline: 8-16 mS/cm
        - Extremely saline: EC > 16 mS/cm
        
        Args:
            ec_ms_cm (float): Electrical Conductivity in mS/cm
            
        Returns:
            dict: {"level": str, "description": str, "suitable_crops": list}
        """
        if ec_ms_cm is None or ec_ms_cm < 0:
            return {"level": "Invalid", "description": "Invalid EC value", "suitable_crops": []}
        
        if ec_ms_cm < 2:
            return {
                "level": "Non-saline",
                "description": "Normal soil, suitable for most crops",
                "suitable_crops": ["All crops", "Vegetables", "Fruits", "Grains"]
            }
        elif ec_ms_cm < 4:
            return {
                "level": "Slightly saline", 
                "description": "Some salt-sensitive crops may be affected",
                "suitable_crops": ["Barley", "Cotton", "Sugar beet", "Most vegetables"]
            }
        elif ec_ms_cm < 8:
            return {
                "level": "Moderately saline",
                "description": "Many crops will show reduced yields",
                "suitable_crops": ["Barley", "Cotton", "Sugar beet", "Wheat (tolerant varieties)"]
            }
        elif ec_ms_cm < 16:
            return {
                "level": "Highly saline",
                "description": "Only salt-tolerant crops will survive",
                "suitable_crops": ["Barley (salt-tolerant)", "Sugar beet", "Date palm"]
            }
        else:
            return {
                "level": "Extremely saline",
                "description": "Very few plants can survive",
                "suitable_crops": ["Halophytes", "Salt-tolerant grasses"]
            }


# Example usage and testing
if __name__ == "__main__":
    # Create sensor instance
    sensor = SensorSoilECRK500_23(port="/dev/ttyS4", slave_address=0x04, baudrate=9600)
    
    # Test communication
    if sensor.test_communication():
        # Read all sensor data
        data = sensor.read_data()
        if data and data["success"]:
            ec = data["ec_value"]
            salinity = data["salinity"]
            
            print(f"\nMeasurement Results:")
            print(f"  EC: {ec:.3f} mS/cm")
            print(f"  Salinity: {salinity:.1f} PPM")
            
            # Classify soil salinity
            classification = SensorSoilECRK500_23.classify_soil_salinity(ec)
            print(f"\nSoil Classification:")
            print(f"  Level: {classification['level']}")
            print(f"  Description: {classification['description']}")
            print(f"  Suitable crops: {', '.join(classification['suitable_crops'])}")
        
        # Display sensor info
        info = sensor.get_sensor_info()
        print("\nSensor Information:")
        for key, value in info.items():
            print(f"  {key}: {value}")
    
    # Scan for devices (optional)
    # found = SensorSoilECRK500_23.scan_addresses()
    
    # Close connection
    sensor.close()
