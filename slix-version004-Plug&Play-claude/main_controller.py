#!/usr/bin/env python3
"""
Smart Farm Main Controller (Production Version)
- Multithreading Architecture (Non-blocking Scheduler Loop)
- Smart Telemetry Status (healthy, weekly, online, offline)
- Port Mismatch Warning System
- Dynamic Configuration via JSON
- Plug & Play Sensor Detection
"""

import time
import threading
import socket
import sys
import math
import statistics
from datetime import datetime
import pytz
import json
import os
import signal
import subprocess
import logging
import serial

# --- Core Logging ---
from logging.handlers import TimedRotatingFileHandler

# --- Hardware & Cloud Modules ---
from mcp_control.mcp_function_control import SensorControlSystem
from telemetry_sending_paho import ThingsBoardSender
from rpc_controller import RPCHandler

# --- Sensor Classes ---
from class_sensor.class_wind_modbus     import SensorWindSpeedDirection
from class_sensor.class_solar_modbus    import SensorPyranometer
from class_sensor.class_soil_modbus     import SensorSoilMoistureTemp
from class_sensor.class_temp_modbus     import SensorAirTempHumidityRS30
from class_sensor.class_rain_modbus     import RainTipModbus
from class_sensor.class_ultra_modbus    import UltrasonicModbus
from class_sensor.class_soil_EC_RK500   import SensorSoilECRK500_23
from class_sensor.class_soilPH_RK500    import SensorSoilPHRK500_22
from class_sensor.class_RKL01           import SensorWaterLevelRKL01

# --- Plug & Play Modules ---
from plug_and_play.active_sensors_manager import ActiveSensorsManager
from plug_and_play.sensor_registry        import SENSOR_TYPE_REGISTRY, get_buffer_size
from plug_and_play.deep_scan              import run_deep_scan

# --- Setup Logging System ---
_raw_config = {}
try:
    with open('config.json', 'r', encoding='utf-8') as f:
        _raw_config = json.load(f)
except FileNotFoundError:
    print("CRITICAL WARNING: 'config.json' file not found! Using default empty configuration.")
except json.JSONDecodeError as e:
    print(f"CRITICAL WARNING: 'config.json' is corrupted or invalid JSON! Error: {e}")
except Exception as e:
    print(f"CRITICAL WARNING: Error reading 'config.json': {e}")

_log_cfg = _raw_config.get("logging", {})
log_filename    = _log_cfg.get("filename", "/root/logs/smartfarm.log")
log_when        = _log_cfg.get("when", "midnight")
log_interval    = _log_cfg.get("interval", 1)
log_backupCount = _log_cfg.get("backupCount", 3)
log_encoding    = _log_cfg.get("encoding", "utf-8")
log_dir         = os.path.dirname(log_filename)

if log_dir and not os.path.exists(log_dir):
    os.makedirs(log_dir)

class ThaiTimeFormatter(logging.Formatter):
    def formatTime(self, record, datefmt=None):
        tz = pytz.timezone('Asia/Bangkok')
        dt = datetime.fromtimestamp(record.created, tz)
        if datefmt:
            return dt.strftime(datefmt)
        return dt.strftime('%Y-%m-%d %H:%M:%S')

log_formatter = ThaiTimeFormatter('%(asctime)s - %(levelname)s - %(message)s', datefmt='%Y-%m-%d %H:%M:%S')
file_handler  = TimedRotatingFileHandler(
    filename=log_filename,
    when=log_when,
    interval=log_interval,
    backupCount=log_backupCount,
    encoding=log_encoding
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
        self.logger   = logger
        self.log_level = log_level
        self.linebuf  = ''

    def write(self, buf):
        for line in buf.rstrip().splitlines():
            if line.strip():
                self.logger.log(self.log_level, line.rstrip())

    def flush(self):
        pass

sys.stdout = StreamToLogger(root_logger, logging.INFO)
sys.stderr = StreamToLogger(root_logger, logging.ERROR)

# --- PnP Loggers (แยกไฟล์ log สำหรับ plug & play ออกเป็น 2 ไฟล์) ---
# pnp_logger  -> activate_logs  : บันทึก event การ register/remove sensor
# scan_logger -> scan_device_logs: บันทึก log ระหว่าง deep scan ทุกบรรทัด

_pnp_cfg = _raw_config.get("plug_and_play", {})

_activate_log_filename = _pnp_cfg.get(
    "activate_log_filename",
    "/root/Main/logs/plug_and_play_logs/activate_logs/smartfarm_pnp.log"
)
_scan_log_filename = _pnp_cfg.get(
    "scan_log_filename",
    "/root/Main/logs/plug_and_play_logs/scan_device_logs/smartfarm_scan.log"
)

def _setup_pnp_logger(name: str, filename: str) -> logging.Logger:
    """สร้าง logger แยกสำหรับ plug & play ที่ไม่ propagate ขึ้น root"""
    log_dir = os.path.dirname(filename)
    if log_dir and not os.path.exists(log_dir):
        os.makedirs(log_dir)
    logger = logging.getLogger(name)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    fh = TimedRotatingFileHandler(
        filename=filename,
        when=log_when,
        interval=log_interval,
        backupCount=log_backupCount,
        encoding=log_encoding,
    )
    fh.setFormatter(log_formatter)
    ch = logging.StreamHandler(sys.stdout)
    ch.setFormatter(log_formatter)
    logger.addHandler(fh)
    logger.addHandler(ch)
    return logger

# pnp_logger  : ใช้ใน main_controller และ active_sensors_manager
# scan_logger : ใช้ใน deep_scan เท่านั้น
pnp_logger  = _setup_pnp_logger("pnp",  _activate_log_filename)
scan_logger = _setup_pnp_logger("scan", _scan_log_filename)


# Dictionary Mapping Sensor Type to Class
SENSOR_CLASS_MAP = {
    "wind":          SensorWindSpeedDirection,
    "soil":          SensorSoilMoistureTemp,
    "air_temp":      SensorAirTempHumidityRS30,
    "ultrasonic":    UltrasonicModbus,
    "rainfall":      RainTipModbus,
    "solar":         SensorPyranometer,
    "soil_ec":       SensorSoilECRK500_23,
    "soil_ph":       SensorSoilPHRK500_22,
    "liquid_level":  SensorWaterLevelRKL01,
}

# Dictionary Mapping Measurement Names for ThingsBoard
MEASUREMENT_NAMES = {
    "air_temp":      {"temperature": "Air_Temp",      "humidity": "Air_Humid"},
    "soil":          {"soil_temperature": "Soil_Temp", "soil_moisture": "Soil_Moist"},
    "solar":         {"solar_radiation": "Solar_Rad"},
    "wind":          {"wind_speed": "Wind_Speed",      "wind_direction": "Wind_Dir"},
    "rainfall":      {"rainfall": "Rain_Gauge"},
    "ultrasonic":    {"distance_cm": "Ultra_Level",    "distance_formula": "Ultra_Level_alarm"},
    "soil_ec":       {"ec_value": "Soil_EC",           "salinity": "Soil_Sal",  "temperature": "Soil_Temp"},
    "soil_ph":       {"ph_value": "Soil_pH",           "temperature": "pH_Temp"},
    "liquid_level":  {"water_level": "Water_Level"},
}


class SmartFarmController:
    def __init__(self):
        print("Initializing Smart Farm Controller...")
        self.running      = True
        self.thailand_tz  = pytz.timezone('Asia/Bangkok')
        self.rs485_lock   = threading.Lock()
        self.data_lock    = threading.Lock()

        config_data        = _raw_config
        self.sys_config    = config_data.get("system", {})
        self.tb_config     = config_data.get("thingsboard", {})
        self.rpc_config    = config_data.get("rpc", {})
        self.pnp_config    = config_data.get("plug_and_play", {})
        self.tele_config   = config_data.get("telemetry", {})

        self.rpc_topic   = self.rpc_config.get("topic_request", "v1/devices/me/rpc/request/+")
        self.rpc_methods = self.rpc_config.get("methods", {})

        self.box_id          = self.sys_config.get("control_box_id", "SLXA_UNKNOWN")
        self.serial_port     = self.sys_config.get("serial_port", "/dev/ttyS2")
        self.net_interval    = self.sys_config.get("internet_check_interval_sec", 10)
        self.read_interval   = self.sys_config.get("read_interval_sec", 10)
        self.send_interval   = self.sys_config.get("telemetry_send_interval_sec", 60)
        self.ignore_overcurrent   = self.sys_config.get("ignore_overcurrent", False)
        self.buffer_size          = self.sys_config.get("sensor_buffer_size", 10)

        # -- telemetry config --
        # unhealthy_threshold: สัดส่วน None ใน buffer ที่ทำให้เปลี่ยนเป็น weekly
        #   1.0 = 100% None ทั้งหมดจึง weekly (default)
        #   0.5 = None เกิน 50% ก็ weekly แล้ว
        self.unhealthy_threshold  = float(self.tele_config.get("unhealthy_threshold", 1.0))
        # aggregation_method: "median" | "mean" | "mode"
        self.aggregation_method   = self.tele_config.get("aggregation_method", "median")

        # -- plug & play config --
        self.pnp_active_sensors_path       = self.pnp_config.get("active_sensors_path", "/root/Main/active_sensors.json")
        self.pnp_scan_power_on_wait        = self.pnp_config.get("scan_power_on_wait_sec", 3.0)
        self.pnp_scan_between_address_wait = self.pnp_config.get("scan_between_address_wait_sec", 0.05)
        self.pnp_power_on_ports            = self._parse_power_on_ports(self.pnp_config.get("power_on_ports", "all"))

        self.current_baudrate = None
        self.master_serial    = None
        self._initialize_master_serial()

        self.latest_data      = {}
        self.sensor_buffers   = {}
        self.rain_accumulators = {}

        # -- plug & play state --
        # set ของ port ที่กำลัง deep scan อยู่ (main loop จะข้ามพอร์ตเหล่านี้)
        self.scanning_ports = set()
        self.scanning_lock  = threading.Lock()

        # สถานะ physical ที่รู้จักล่าสุด ใช้เปรียบเทียบเพื่อตรวจ hotplug
        self.last_known_physical = {}

        # -- hardware --
        self.hw = SensorControlSystem(ignore_overcurrent=self.ignore_overcurrent)
        self._apply_power_on_ports()

        # -- plug & play manager --
        self.pnp_manager = ActiveSensorsManager(self.pnp_active_sensors_path)

        # -- thingsboard --
        self.tb = ThingsBoardSender(
            host=self.tb_config.get("host"),
            port=self.tb_config.get("port", 1883),
            access_token=self.tb_config.get("access_token"),
            topic=self.tb_config.get("topic", "v1/gateway/telemetry")
        )
        self.tb.connect()

        # -- โหลด active sensors จาก active_sensors.json (Phase 1) --
        self.active_sensors = {}
        self._init_sensors_from_registry()

        self.internet_available = False
        self.tb.start_rpc_handler(request_topic=self.rpc_topic)
        self.rpc_manager = RPCHandler()

        if self.rpc_methods.get("reset_remote"):
            self.tb.register_rpc_method(self.rpc_methods["reset_remote"], self.rpc_manager.handle_reset_remote)
        if self.rpc_methods.get("reboot"):
            self.tb.register_rpc_method(self.rpc_methods["reboot"], self.rpc_manager.handle_reboot)
        if self.rpc_methods.get("restart_process"):
            self.tb.register_rpc_method(self.rpc_methods["restart_process"], self.rpc_manager.handle_restart_process)

        print("Initialization Complete.")

    # -----------------------------------------------------------------------
    # Phase 2: จัดการไฟเลี้ยง sensor ตาม config
    # -----------------------------------------------------------------------

    def _parse_power_on_ports(self, value) -> set:
        """
        แปลงค่า power_on_ports จาก config.json ให้เป็น set ของ port number
        รองรับ 3 รูปแบบ:
          "all"                    -> เปิดทุก port (1-16)
          [1, 3, 5]               -> เปิดเฉพาะ port ที่ระบุ
          {"from": 1, "to": 8}   -> เปิด range
        """
        if value == "all":
            return set(range(1, 17))
        elif isinstance(value, list):
            return set(value)
        elif isinstance(value, dict):
            start = value.get("from", 1)
            end   = value.get("to", 16)
            return set(range(start, end + 1))
        else:
            logging.warning(f"[PnP] Invalid power_on_ports value: {value} -> defaulting to all")
            return set(range(1, 17))

    def _apply_power_on_ports(self):
        """เปิดไฟเฉพาะ port ที่กำหนดใน config"""
        all_ports = set(range(1, 17))
        ports_to_off = all_ports - self.pnp_power_on_ports

        # ปิดทุก port ก่อน แล้วค่อยเปิดเฉพาะที่ต้องการ
        self.hw.turn_off_all_sensors()

        for port in sorted(self.pnp_power_on_ports):
            self.hw.turn_on_sensor(port)

        print(f"[PnP] Power ON ports: {sorted(self.pnp_power_on_ports)}")
        if ports_to_off:
            print(f"[PnP] Power OFF ports: {sorted(ports_to_off)}")

    # -----------------------------------------------------------------------
    # Phase 1: โหลด sensor จาก active_sensors.json
    # -----------------------------------------------------------------------

    def _init_sensors_from_registry(self):
        """
        โหลด active sensors จาก active_sensors.json แทนการอ่านจาก config.json
        สร้าง sensor object และเก็บไว้ใน self.active_sensors
        """
        active = self.pnp_manager.load()
        if not active:
            print("[PnP] active_sensors.json is empty -> waiting for sensors to be plugged in")
            return

        for port, s_info in active.items():
            self._instantiate_sensor(port, s_info)

        print(f"[PnP] Loaded {len(self.active_sensors)} sensor(s) from active_sensors.json")

    def _instantiate_sensor(self, port: str, s_info: dict):
        """
        สร้าง sensor object จาก s_info dict และเพิ่มเข้า self.active_sensors
        ถ้าชนิดไม่รู้จักจะ log warning และข้ามไป
        """
        s_type = s_info.get("type")
        address = s_info.get("address")

        if s_type not in SENSOR_CLASS_MAP:
            logging.warning(f"[PnP] Unknown sensor type '{s_type}' at port {port} -> skipping")
            return

        registry_entry = SENSOR_TYPE_REGISTRY.get(s_type, {})
        baudrate = registry_entry.get("baudrate", 9600)

        try:
            sensor_obj = SENSOR_CLASS_MAP[s_type](
                port=self.serial_port,
                slave_address=address,
                baudrate=baudrate,
            )
            self.active_sensors[str(port)] = {
                "type":       s_type,
                "address":    address,
                "model":      s_info.get("model", registry_entry.get("model", "Unknown")),
                "instance":   s_info.get("instance", "01"),
                "baudrate":   baudrate,
                "sensor_obj": sensor_obj,
            }
            print(f"[PnP] Sensor instantiated -> port={port} type={s_type} address={address}")
        except Exception as e:
            logging.error(f"[PnP] Failed to instantiate sensor at port {port}: {e}")

    # -----------------------------------------------------------------------
    # Plug & Play: hotplug detection (Phase 3)
    # -----------------------------------------------------------------------

    def _check_hotplug(self, io_statuses: dict):
        """
        เปรียบเทียบสถานะ physical จาก MCP กับ active_sensors.json
        Case A: port อยู่ใน active แต่ MCP บอกว่าว่าง  -> remove
        Case B: MCP บอกว่ามี sensor แต่ไม่อยู่ใน active -> deep scan
        """
        active_ports = set(self.active_sensors.keys())

        for port_num, status in io_statuses.items():
            port_str     = str(port_num)
            is_connected = status.get("connected", False)

            # ข้าม port ที่ไม่ได้อยู่ใน power_on_ports
            if port_num not in self.pnp_power_on_ports:
                continue

            with self.scanning_lock:
                is_scanning = port_str in self.scanning_ports

            if is_scanning:
                # กำลัง scan อยู่ ข้ามไปก่อน
                continue

            if port_str in active_ports and not is_connected:
                # Case A: สายหลุด
                pnp_logger.warning(f"[PnP] Port {port_str}: cable disconnected -> removing from active")
                self._handle_unplug(port_str)

            elif port_str not in active_ports and is_connected:
                # Case B: เสียบสายใหม่
                pnp_logger.info(f"[PnP] Port {port_str}: new connection detected -> starting deep scan")
                self._handle_new_plug(port_str)

    def _handle_unplug(self, port: str):
        """Case A: ถอด sensor ออก -> ลบจาก active_sensors และ reserved"""
        # ลบ sensor object ออกจาก dict
        with self.data_lock:
            self.active_sensors.pop(port, None)
            self.sensor_buffers.pop(port, None)
            self.rain_accumulators.pop(port, None)

        # ย้ายไป reserved ใน json
        self.pnp_manager.remove_port(port)

    def _handle_new_plug(self, port: str):
        """Case B: เสียบ sensor ใหม่ -> เริ่ม deep scan บน background thread"""
        with self.scanning_lock:
            self.scanning_ports.add(port)

        t = threading.Thread(
            target=self._deep_scan_worker,
            args=(port,),
            daemon=True,
            name=f"DeepScan-Port{port}",
        )
        t.start()


    def _deep_scan_worker(self, port: str):
        """
        รัน deep scan บน background thread
        หลัง scan เสร็จ reload sensor object เข้า active_sensors
        """
        try:
            success = run_deep_scan(
                port=port,
                serial_port=self.serial_port,
                manager=self.pnp_manager,
                power_on_wait_sec=self.pnp_scan_power_on_wait,
                between_address_wait_sec=self.pnp_scan_between_address_wait,
                rs485_lock=self.rs485_lock,
            )

            if success:
                # โหลด entry ใหม่จาก active_sensors.json แล้วสร้าง sensor object
                active = self.pnp_manager.load()
                s_info = active.get(str(port))
                if s_info:
                    with self.data_lock:
                        self._instantiate_sensor(port, s_info)
                    pnp_logger.info(f"[PnP] Port {port}: sensor registered and ready")
                else:
                    pnp_logger.error(f"[PnP] Port {port}: deep scan succeeded but entry not found in json")
            else:
                pnp_logger.warning(f"[PnP] Port {port}: deep scan failed -> port remains unregistered")

        except Exception as e:
            pnp_logger.error(f"[PnP] Port {port}: deep scan worker crashed: {e}")

        finally:
            # ไม่ว่าจะสำเร็จหรือไม่ ปลด lock ของ port นี้เสมอ
            with self.scanning_lock:
                self.scanning_ports.discard(port)

    # -----------------------------------------------------------------------
    # Serial
    # -----------------------------------------------------------------------

    def _initialize_master_serial(self):
        try:
            self.master_serial = serial.Serial(
                port=self.serial_port,
                baudrate=9600,
                timeout=1.5,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                bytesize=serial.EIGHTBITS
            )
            self.current_baudrate = 9600
        except Exception as e:
            print(f"Master Serial Init Error: {e}")

    def _change_baudrate(self, new_baudrate):
        if self.current_baudrate != new_baudrate and self.master_serial:
            try:
                self.master_serial.close()
                self.master_serial = serial.Serial(
                    port=self.serial_port,
                    baudrate=new_baudrate,
                    timeout=1.5,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE,
                    bytesize=serial.EIGHTBITS
                )
                self.current_baudrate = new_baudrate
                print(f"Switched Baudrate to {new_baudrate}")
                time.sleep(0.2)
                return True
            except Exception as e:
                print(f"Failed to change baudrate to {new_baudrate}: {e}")
                return False
        return True

    # -----------------------------------------------------------------------
    # Sensor Reading
    # -----------------------------------------------------------------------

    def _read_sensor_data(self, s_type, instance):
        if s_type == "wind":
            return instance.read_wind()
        elif s_type == "soil":
            return instance.read_data()
        elif s_type == "air_temp":
            res = instance.read_temp()
            if res is None:
                return None
            temp = res.get("temperature")
            hum  = res.get("humidity")
            if temp is None or hum is None:
                return None
            if not (-30 <= temp <= 70) or not (0 <= hum <= 100):
                logging.warning(
                    f"[Sensor] air_temp out of range: temp={temp} hum={hum} -> discarding"
                )
                return None
            return res
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

    # -----------------------------------------------------------------------
    # Thread 1: Network Check Loop
    # -----------------------------------------------------------------------

    def _network_check_loop(self):
        while self.running:
            try:
                socket.create_connection(("8.8.8.8", 53), timeout=3)
                current_status = True
            except OSError:
                current_status = False

            if current_status != self.internet_available:
                self.internet_available = current_status
                if self.internet_available:
                    msg = "[Network] Internet connection restored."
                    print(msg)
                    logging.info(msg)
                    if not self.tb.connected:
                        logging.info("[ThingsBoard] Attempting to reconnect...")
                        self.tb.connect()
                else:
                    msg = "[Network] Internet connection lost."
                    print(msg)
                    logging.warning(msg)

            time.sleep(self.net_interval)

    # -----------------------------------------------------------------------
    # Thread 2: Sensor Reading Loop (Phase 2 + Phase 3)
    # -----------------------------------------------------------------------

    def _sensor_reading_loop(self):
        while self.running:
            print(f"\n[{datetime.now(self.thailand_tz).strftime('%H:%M:%S')}] Starting sensor reading cycle...")

            self.hw.check_overcurrent()
            self.hw.check_sensor_connection()
            io_statuses = self.hw.get_all_port_statuses()

            # Phase 3: ตรวจ hotplug ทุกรอบ
            self._check_hotplug(io_statuses)

            # Phase 2: อ่านค่า sensor ทุกตัวที่ active อยู่
            # snapshot active_sensors ก่อนเพื่อความปลอดภัย
            with self.data_lock:
                active_snapshot = dict(self.active_sensors)

            for port, s_info in active_snapshot.items():

                if not self.running:
                    break

                # ข้าม port ที่กำลัง deep scan อยู่
                with self.scanning_lock:
                    if port in self.scanning_ports:
                        continue

                s_type     = s_info["type"]
                sensor_obj = s_info["sensor_obj"]

                with self.rs485_lock:
                    self._change_baudrate(s_info.get("baudrate", 9600))
                    time.sleep(0.05)
                    data = self._read_sensor_data(s_type, sensor_obj)

                port_num = int(port)
                is_physically_connected = io_statuses.get(port_num, {}).get("connected", False)

                buf_size = get_buffer_size(s_type, fallback=self.buffer_size)

                with self.data_lock:
                    if port not in self.sensor_buffers:
                        self.sensor_buffers[port] = []

                    self.sensor_buffers[port].append(data)

                    if len(self.sensor_buffers[port]) > buf_size:
                        self.sensor_buffers[port].pop(0)

                    if data and s_type == "rainfall" and "rainfall" in data:
                        if port not in self.rain_accumulators:
                            self.rain_accumulators[port] = 0.0
                        self.rain_accumulators[port] += data["rainfall"]

                    self.latest_data[port] = {
                        "physically_connected": is_physically_connected
                    }

                if data:
                    print(f"Port {port} ({s_type}) Read Success: {data}")
                else:
                    print(f"Port {port} ({s_type}) Read Failed / No Response")

            time.sleep(self.read_interval)

    # -----------------------------------------------------------------------
    # Thread 3: Telemetry Sending Loop
    # -----------------------------------------------------------------------

    def _telemetry_send_loop(self):
        while self.running:
            if not self.latest_data:
                print("[Telemetry Task] No new data to send.")
            else:
                print(f"\n[{datetime.now(self.thailand_tz).strftime('%H:%M:%S')}] Sending telemetry to ThingsBoard...")

                with self.data_lock:
                    data_to_send = list(self.latest_data.items())
                    self.latest_data.clear()

                for port, sensor_state in data_to_send:
                    if not self.running:
                        break
                    s_info = self.active_sensors.get(port)
                    if s_info:
                        self.send_telemetry_data(port, sensor_state, s_info)

                self.send_io_status()

            time.sleep(self.send_interval)

    # -----------------------------------------------------------------------
    # Telemetry helpers
    # -----------------------------------------------------------------------

    def _buffer_has_data(self, buffer: list) -> bool:
        """
        ตรวจว่า buffer มี valid data มากพอตาม unhealthy_threshold
        unhealthy_threshold=1.0 → ต้อง None ทั้งหมด 100% จึง weekly
        unhealthy_threshold=0.5 → None เกิน 50% ก็ weekly แล้ว
        """
        if not buffer:
            return False
        none_count = sum(1 for e in buffer if e is None)
        none_ratio = none_count / len(buffer)
        return none_ratio < self.unhealthy_threshold

    def _compute_aggregate_data(self, buffer: list, s_type: str) -> dict:
        """
        คำนวณค่ารวมจาก buffer ตาม aggregation_method (median/mean/mode)
        - ค่า None ใน buffer จะถูกข้าม
        - ถ้า valid entries ว่างเปล่า คืน None
        - wind_direction ใช้ circular mean เสมอ (ไม่ขึ้นกับ method)
        """
        valid_entries = [e for e in buffer if e is not None]
        if not valid_entries:
            return None

        keys   = valid_entries[0].keys()
        method = self.aggregation_method
        result = {}

        for key in keys:
            values = [e[key] for e in valid_entries if e.get(key) is not None]
            if not values:
                continue

            if key == "wind_direction":
                sin_sum = sum(math.sin(math.radians(v)) for v in values)
                cos_sum = sum(math.cos(math.radians(v)) for v in values)
                result[key] = round(math.degrees(math.atan2(sin_sum, cos_sum)) % 360, 1)
            elif method == "mean":
                result[key] = round(statistics.mean(values), 2)
            elif method == "mode":
                try:
                    result[key] = round(statistics.mode(values), 2)
                except statistics.StatisticsError:
                    result[key] = round(statistics.median(values), 2)
            else:
                result[key] = round(statistics.median(values), 2)

        return result if result else None

    # -----------------------------------------------------------------------
    # Telemetry
    # -----------------------------------------------------------------------

    def send_telemetry_data(self, port, sensor_state, s_info):
        if not self.tb or not self.tb.connected:
            return

        physically_connected = sensor_state["physically_connected"]
        s_type = s_info["type"]

        with self.data_lock:
            port_buffer = list(self.sensor_buffers.get(port, []))

        # rainfall ใช้ accumulator ไม่ใช้ aggregate
        if s_type == "rainfall":
            aggregate_data = None
            has_data       = self._buffer_has_data(port_buffer)
        else:
            aggregate_data = self._compute_aggregate_data(port_buffer, s_type)
            has_data       = self._buffer_has_data(port_buffer)

        if physically_connected and has_data:
            operation_status = "online"
            current_status   = "healthy"
            include_data     = True
            if s_type == "rainfall":
                data_to_send = {}
                with self.data_lock:
                    acc_rain = self.rain_accumulators.get(port, 0.0)
                    data_to_send["rainfall"] = round(acc_rain, 2)
                    self.rain_accumulators[port] = 0.0
            else:
                data_to_send = aggregate_data
        elif physically_connected and not has_data:
            operation_status = "online"
            current_status   = "weekly"
            include_data     = False
            data_to_send     = None
        else:
            operation_status = "offline"
            current_status   = "weekly"
            include_data     = False
            data_to_send     = None

        ts_now   = int(datetime.now(self.thailand_tz).timestamp() * 1000)
        messages = {}
        model    = s_info.get("model", "Unknown")
        instance = s_info.get("instance", "01")

        expected_measurements = MEASUREMENT_NAMES.get(s_type, {})

        for key, name_suffix in expected_measurements.items():
            msg_name = f"{self.box_id}_{name_suffix}_{model}-{instance}"
            payload  = {
                "current_status":   current_status,
                "operation_status": operation_status,
            }
            if include_data and data_to_send and key in data_to_send:
                payload["data_value"] = data_to_send[key]

            messages[msg_name] = [{"ts": ts_now, "values": payload}]

        if messages:
            ok = self.tb.send_telemetry(messages)
            if ok:
                print(f"[ThingsBoard] Sent Telemetry for Port {port} (Op: {operation_status}, Cur: {current_status}, Data: {include_data})")

    def send_io_status(self):
        if not self.tb or not self.tb.connected:
            return

        io_statuses = self.hw.get_all_port_statuses()
        ts_now      = int(datetime.now(self.thailand_tz).timestamp() * 1000)
        values      = {}

        for port, status in io_statuses.items():
            values[f"Port_{port}_Connected"]   = "ON" if status["connected"]   else "OFF"
            values[f"Port_{port}_Overcurrent"] = "ON" if status["overcurrent"] else "OFF"
            values[f"Port_{port}_Power"]       = "ON" if status["power_on"]    else "OFF"

        cpu_temp = self.get_cpu_temperature()
        if cpu_temp is not None:
            values["Board_CPU_Temp"] = cpu_temp

        payload  = {"ts": ts_now, "values": values}
        io_topic = self.tb_config.get("io_topic", "v1/devices/me/telemetry")

        if self.tb.send_telemetry(payload, topic=io_topic):
            temp_log = f", CPU Temp: {cpu_temp}C" if cpu_temp else ""
            print(f"[ThingsBoard] Sent I/O Status{temp_log}")

    def get_cpu_temperature(self):
        try:
            with open("/sys/class/thermal/thermal_zone0/temp", "r") as f:
                temp_str = f.read().strip()
                return round(float(temp_str) / 1000.0, 2)
        except Exception as e:
            print(f"Warning: Cannot read CPU temp ({e})")
            return None

    # -----------------------------------------------------------------------
    # Run / Stop
    # -----------------------------------------------------------------------

    def run(self):
        print("Smart Farm Controller is now running (Multithreaded).")
        signal.signal(signal.SIGINT,  lambda signum, frame: self.stop())
        signal.signal(signal.SIGTERM, lambda signum, frame: self.stop())

        self.t_net       = threading.Thread(target=self._network_check_loop,  daemon=True, name="Thread-Network")
        self.t_sensor    = threading.Thread(target=self._sensor_reading_loop, daemon=True, name="Thread-Sensor")
        self.t_telemetry = threading.Thread(target=self._telemetry_send_loop, daemon=True, name="Thread-Telemetry")

        self.t_net.start()
        time.sleep(1)
        self.t_sensor.start()
        time.sleep(1)
        self.t_telemetry.start()

        try:
            while self.running:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\nReceived shutdown signal.")
            self.stop()
        except Exception as e:
            print(f"Fatal error in main loop: {e}")
            self.stop()

    def stop(self):
        self.running = False
        print("Shutting down system... Waiting for threads to finish cleanly.")

        try:
            if hasattr(self, 't_sensor')    and self.t_sensor.is_alive():    self.t_sensor.join(timeout=3.0)
            if hasattr(self, 't_telemetry') and self.t_telemetry.is_alive(): self.t_telemetry.join(timeout=3.0)
            if hasattr(self, 't_net')       and self.t_net.is_alive():       self.t_net.join(timeout=3.0)
        except Exception as e:
            print(f"Error waiting for threads: {e}")

        try:
            self.hw.turn_off_all_sensors()
            if self.tb:
                self.tb.close()
            for s_info in self.active_sensors.values():
                sensor_obj = s_info.get("sensor_obj")
                if sensor_obj and hasattr(sensor_obj, "close"):
                    sensor_obj.close()
        except Exception as e:
            print(f"Error during shutdown hardware: {e}")

        print("System shutdown complete.")
        sys.exit(0)


if __name__ == "__main__":
    controller = SmartFarmController()
    controller.run()