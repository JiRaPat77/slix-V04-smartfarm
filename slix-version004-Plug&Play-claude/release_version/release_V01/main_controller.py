#!/usr/bin/env python3
"""
Smart Farm Main Controller (Production Version)
- Non-blocking Scheduler Loop
- Latest Data Update & Interval Telemetry Send
- Centralized RS485 Thread Locking
- Dynamic Configuration via JSON
"""

import time
import threading
import socket
import sys
from datetime import datetime
import pytz
import os
import logging
from logging.handlers import TimedRotatingFileHandler


# --- Setup Logging System ---
if not os.path.exists('logs'):
    os.makedirs('logs')

class ThaiTimeFormatter(logging.Formatter):
    def formatTime(self, record, datefmt=None):
        tz = pytz.timezone('Asia/Bangkok')
        dt = datetime.fromtimestamp(record.created, tz)
        if datefmt:
            return dt.strftime(datefmt)
        return dt.strftime('%Y-%m-%d %H:%M:%S')
    
log_formatter = ThaiTimeFormatter('%(asctime)s - %(levelname)s - %(message)s', datefmt='%Y-%m-%d %H:%M:%S')
file_handler = TimedRotatingFileHandler(
    filename='/root/logs/smartfarm.log', 
    when='midnight', 
    interval=1, 
    backupCount=3, 
    encoding='utf-8'
)
file_handler.setFormatter(log_formatter)
console_handler = logging.StreamHandler(sys.stdout)
console_handler.setFormatter(log_formatter)


root_logger = logging.getLogger()
root_logger.setLevel(logging.INFO)
root_logger.addHandler(file_handler)
root_logger.addHandler(console_handler)


class StreamToLogger(object):
    def __init__(self, logger, log_level):
        self.logger = logger
        self.log_level = log_level
        self.linebuf = ''
        
    def write(self, buf):
        for line in buf.rstrip().splitlines():
            if line.strip():
                self.logger.log(self.log_level, line.rstrip())
            
    def flush(self):
        pass
sys.stdout = StreamToLogger(root_logger, logging.INFO)
sys.stderr = StreamToLogger(root_logger, logging.ERROR)


# --- Core Modules ---
from config_manager import ConfigManager

# --- Hardware & Cloud Modules ---
from mcp_control.mcp_function_control import SensorControlSystem
from telemetry_sending_paho import ThingsBoardSender

# --- Sensor Classes ---
from class_sensor.class_wind_modbus import SensorWindSpeedDirection
from class_sensor.class_solar_modbus import SensorPyranometer  
from class_sensor.class_soil_modbus import SensorSoilMoistureTemp
from class_sensor.class_temp_modbus import SensorAirTempHumidityRS30
from class_sensor.class_rain_modbus import RainTipModbus
from class_sensor.class_ultra_modbus import UltrasonicModbus
from class_sensor.class_soil_EC_RK500 import SensorSoilECRK500_23  
from class_sensor.class_soilPH_RK500 import SensorSoilPHRK500_22 
from class_sensor.class_RKL01 import SensorWaterLevelRKL01

# Dictionary Mapping Sensor Type to Class
SENSOR_CLASS_MAP = {
    "wind": SensorWindSpeedDirection,
    "soil": SensorSoilMoistureTemp,
    "air_temp": SensorAirTempHumidityRS30,
    "ultrasonic": UltrasonicModbus,
    "rainfall": RainTipModbus,
    "solar": SensorPyranometer,
    "soil_ec": SensorSoilECRK500_23,
    "soil_ph": SensorSoilPHRK500_22,
    "liquid_level": SensorWaterLevelRKL01
}

# Dictionary Mapping Measurement Names for ThingsBoard
MEASUREMENT_NAMES = {
    "air_temp": {"temperature": "Air_Temp",
                  "humidity": "Air_Humid"},

    "soil": {"soil_temperature": "Soil_Temp",
              "soil_moisture": "Soil_Moist"},

    "solar": {"solar_radiation": "Solar_Rad"},

    "wind": {"wind_speed": "Wind_Speed",
              "wind_direction": "Wind_Dir"},

    "rainfall": {"rainfall": "Rain_Gauge"},

    "ultrasonic": {"distance_cm": "Ultra_Level", 
                   "distance_formula": "Ultra_Level_alarm"},

    "soil_ec": {"ec_value": "Soil_EC",
                 "salinity": "Soil_Sal",
                   "temperature": "Soil_Temp"},

    "soil_ph": {"ph_value": "Soil_pH", 
                "temperature": "pH_Temp"},

    "liquid_level": {"water_level": "Water_Level"}
}

class SmartFarmController:
    def __init__(self):
        print("Initializing Smart Farm Controller...")
        self.running = True
        self.thailand_tz = pytz.timezone('Asia/Bangkok')
        self.rs485_lock = threading.Lock()
        
        # 1. Load Configuration
        self.config_mgr = ConfigManager('config.json')
        self.sys_config = self.config_mgr.get_system_config()
        self.tb_config = self.config_mgr.get_thingsboard_config()
        self.box_id = self.sys_config.get("control_box_id", "SLXA_UNKNOWN")
        self.serial_port = self.sys_config.get("serial_port", "/dev/ttyS2")
        
        # Timers configuration
        self.net_interval = self.sys_config.get("internet_check_interval_sec", 10)
        self.read_interval = self.sys_config.get("read_interval_sec", 10)
        self.send_interval = self.sys_config.get("telemetry_send_interval_sec", 60)
        self.ignore_overcurrent = self.sys_config.get("ignore_overcurrent", False)

        
        # Dictionary to keep only the latest reading for each port
        self.latest_data = {}
        
        # 2. Initialize Hardware (MCP)
        self.hw = SensorControlSystem(ignore_overcurrent=self.ignore_overcurrent)
        self.hw.turn_on_all_sensors()
        
        # 3. Initialize ThingsBoard
        self.tb = ThingsBoardSender(
            host=self.tb_config.get("host"),
            port=self.tb_config.get("port", 1883),
            access_token=self.tb_config.get("access_token"),
            topic=self.tb_config.get("topic", "v1/gateway/telemetry") 
        )
        self.tb.connect()
        
        # 4. Initialize Active Sensors
        self.active_sensors = {}
        self._init_sensors()
        
        self.internet_available = False
        print("Initialization Complete.")

    def _init_sensors(self):
        """Create instances only for enabled sensors"""
        sensors_config = self.config_mgr.get_active_sensors()
        for port, s_info in sensors_config.items():
            s_type = s_info.get("type")
            if s_type in SENSOR_CLASS_MAP:
                try:
                    sensor_class = SENSOR_CLASS_MAP[s_type]
                    instance = sensor_class(
                        port=self.serial_port, 
                        slave_address=s_info.get("address"), 
                        baudrate=s_info.get("baudrate")
                    )
                    s_info['sensor_obj'] = instance
                    self.active_sensors[port] = s_info
                    print(f"Sensor Port {port} ({s_type}) Initialized.")
                except Exception as e:
                    print(f"Failed to initialize sensor at port {port}: {e}")
            else:
                print(f"Unknown sensor type '{s_type}' at port {port}")

    def _read_sensor_data(self, s_type, instance):
        """Unified wrapper - Removed try/except to expose real errors to log"""
        if s_type == "wind": return instance.read_wind()
        elif s_type == "soil": return instance.read_data()
        elif s_type == "air_temp": return instance.read_temp()
        elif s_type == "rainfall": 
            res = instance.read_tip()
            return {"rainfall": res.get("rainfall")} if res and res.get("success") else None
        elif s_type == "ultrasonic": 
            res = instance.read_distance()
            return res if res and res.get("success") else None
        elif s_type == "solar":
            res = instance.read_radiation()
            return {"solar_radiation": res.get("radiation")} if res else None
        elif s_type in ["soil_ec", "soil_ph"]:
            return instance.read_data() 
        elif s_type == "liquid_level":
            res = instance.read_water_level()
            return {"water_level": res.get("water_level")} if res else None
        return None

    def task_check_internet(self):
        """Check internet connection and manage ThingsBoard status"""
        try:
            socket.create_connection(("8.8.8.8", 53), timeout=3)
            current_status = True
        except OSError:
            current_status = False
            
        if current_status != self.internet_available:
            self.internet_available = current_status
            if self.internet_available:
                print("[Network] Internet connection restored.")
                if not self.tb.connected:
                    self.tb.connect()
            else:
                print("[Network] Internet connection lost.")

    def task_read_sensors(self):
        """Read all active sensors and store the latest value"""
        print(f"\n[{datetime.now(self.thailand_tz).strftime('%H:%M:%S')}] Starting sensor reading cycle...")
        
        self.hw.check_overcurrent()
        self.hw.check_sensor_connection()

        for port, s_info in self.active_sensors.items():
            s_type = s_info['type']
            instance = s_info['sensor_obj']
            
            with self.rs485_lock:
                time.sleep(0.05)
                data = self._read_sensor_data(s_type, instance)
                
            if data:
                print(f"Port {port} ({s_type}) Read Success: {data}")
                self.latest_data[port] = data
            else:
                print(f"Port {port} ({s_type}) Read Failed / No Response")

    def task_send_telemetry(self):
        """Send the latest gathered data to ThingsBoard"""
        if not self.latest_data:
            print("No new data to send.")
            return

        print(f"\n[{datetime.now(self.thailand_tz).strftime('%H:%M:%S')}] Sending telemetry to ThingsBoard...")
        
        for port, data in list(self.latest_data.items()):
            s_info = self.active_sensors.get(port)
            if s_info:
                self.send_telemetry_data(port, data, s_info)

        self.send_io_status()
                
        # Clear data after sending to avoid sending stale data in the next cycle if a sensor disconnects
        self.latest_data.clear()

    def send_telemetry_data(self, port, sensor_data, s_info):
        """Format and send a single port's data via MQTT"""
        if not self.tb or not self.tb.connected:
            return

        ts_now = int(datetime.now(self.thailand_tz).timestamp() * 1000)
        messages = {}
        s_type = s_info['type']
        
        for key, value in sensor_data.items():
            if key in MEASUREMENT_NAMES.get(s_type, {}):
                name_suffix = MEASUREMENT_NAMES[s_type][key]
                model = s_info.get('model', 'Unknown')
                instance = s_info.get('instance', '01')
                msg_name = f"{self.box_id}_{name_suffix}_{model}-{instance}"
                
                messages[msg_name] = [{
                    "ts": ts_now,
                    "values": {"data_value": value}
                }]
                
        if messages:
            ok = self.tb.send_telemetry(messages)
            if ok:
                print(f"[ThingsBoard] Sent Telemetry for Port {port}")

    def send_io_status(self):
        if not self.tb or not self.tb.connected:
            return

        io_statuses = self.hw.get_all_port_statuses()
        ts_now = int(datetime.now(self.thailand_tz).timestamp() * 1000)
        
        values = {}
        for port, status in io_statuses.items():
            values[f"Port_{port}_Connected"] = "ON" if status["connected"] else "OFF"
            values[f"Port_{port}_Overcurrent"] = "ON" if status["overcurrent"] else "OFF"
            values[f"Port_{port}_Power"] = "ON" if status["power_on"] else "OFF"
        
        cpu_temp = self.get_cpu_temperature()
        if cpu_temp is not None:
            values["Board_CPU_Temp"] = cpu_temp
            
        payload = {
            "ts": ts_now,
            "values": values
        }
        
        io_topic = self.tb_config.get("io_topic", "v1/devices/me/telemetry")
        if self.tb.send_telemetry(payload, topic=io_topic):
            temp_log = f", CPU Temp: {cpu_temp}C" if cpu_temp else ""
            print(f"[ThingsBoard] Sent I/O Status to {io_topic}{temp_log}")
    
    def get_cpu_temperature(self):
        try:
            with open("/sys/class/thermal/thermal_zone0/temp", "r") as f:
                temp_str = f.read().strip()
                temp_c = float(temp_str) / 1000.0
                return round(temp_c, 2)
        except Exception as e:
            print(f"Warning: Cannot read CPU temp ({e})")
            return None

    def run(self):
        """Non-blocking Main Loop Scheduler"""
        print("Smart Farm Controller is now running.")
        
        last_net_check = 0
        last_sensor_read = 0
        last_telemetry_send = 0

        try:
            while self.running:
                current_time = time.time()
                
                # Task 1: Check Internet
                if current_time - last_net_check >= self.net_interval:
                    self.task_check_internet()
                    last_net_check = time.time()
                
                # Task 2: Read Sensors
                if current_time - last_sensor_read >= self.read_interval:
                    self.task_read_sensors()
                    last_sensor_read = time.time()
                    
                # Task 3: Send Telemetry
                if current_time - last_telemetry_send >= self.send_interval:
                    self.task_send_telemetry()
                    last_telemetry_send = time.time()
                
                # Yield CPU
                time.sleep(0.1)
                
        except KeyboardInterrupt:
            print("\nReceived shutdown signal.")
            self.stop()
        except Exception as e:
            print(f"Fatal error in main loop: {e}")
            self.stop()

    def stop(self):
        """Safely shutdown the system"""
        self.running = False
        print("Shutting down system...")
        try:
            self.hw.turn_off_all_sensors()
            if self.tb:
                self.tb.close()
            for s_info in self.active_sensors.values():
                sensor_obj = s_info.get('sensor_obj')
                if sensor_obj and hasattr(sensor_obj, 'close'):
                    sensor_obj.close()
        except Exception as e:
            print(f"Error during shutdown: {e}")
        print("System shutdown complete.")
        sys.exit(0)

if __name__ == "__main__":
    controller = SmartFarmController()
    controller.run()