import os
import sys

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)

from class_sensor.class_soil_modbus      import SensorSoilMoistureTemp
from class_sensor.class_soil_EC_RK500    import SensorSoilECRK500_23
from class_sensor.class_soilPH_RK500     import SensorSoilPHRK500_22
from class_sensor.class_temp_modbus      import SensorAirTempHumidityRS30
from class_sensor.class_wind_modbus      import SensorWindSpeedDirection
from class_sensor.class_rain_modbus      import RainTipModbus
from class_sensor.class_solar_modbus     import SensorPyranometer
from class_sensor.class_ultra_modbus     import UltrasonicModbus
from class_sensor.class_RKL01            import SensorWaterLevelRKL01


SENSOR_TYPE_REGISTRY = {
    "soil": {
        "default":         1,
        "range":           (2, 13),
        "factory_defaults": [],
        "model":           "RK520",
        "baudrate":        9600,
        "buffer_size":     10,
        "class":           SensorSoilMoistureTemp,
    },
    "soil_ec": {
        "default":         14,
        "range":           (15, 26),
        "factory_defaults": [],
        "model":           "RK500-23",
        "baudrate":        9600,
        "buffer_size":     10,
        "class":           SensorSoilECRK500_23,
    },
    "soil_ph": {
        "default":         27,
        "range":           (28, 39),
        "factory_defaults": [],
        "model":           "RK500-22",
        "baudrate":        9600,
        "buffer_size":     10,
        "class":           SensorSoilPHRK500_22,
    },
    "air_temp": {
        "default":         40,
        "range":           (41, 52),
        # address 1 is common factory default for RS485 temp sensors
        # scanned last so soil (default=1) takes priority if both are at address 1
        "factory_defaults": [1],
        "model":           "MW485",
        "baudrate":        9600,
        "buffer_size":     10,
        "class":           SensorAirTempHumidityRS30,
    },
    "wind": {
        "default":         53,
        "range":           (54, 65),
        "factory_defaults": [],
        "model":           "RK120",
        "baudrate":        9600,
        "buffer_size":     10,
        "class":           SensorWindSpeedDirection,
    },
    "rainfall": {
        "default":         66,
        "range":           (67, 78),
        "factory_defaults": [],
        "model":           "RK400",
        "baudrate":        9600,
        "buffer_size":     20,
        "class":           RainTipModbus,
    },
    "solar": {
        "default":         79,
        "range":           (80, 91),
        "factory_defaults": [],
        "model":           "RK200",
        "baudrate":        9600,
        "buffer_size":     10,
        "class":           SensorPyranometer,
    },
    "ultrasonic": {
        "default":         92,
        "range":           (93, 104),
        "factory_defaults": [],
        "model":           "RCWL",
        "baudrate":        9600,
        "buffer_size":     10,
        "class":           UltrasonicModbus,
    },
    "liquid_level": {
        "default":         105,
        "range":           (106, 117),
        "factory_defaults": [],
        "model":           "RKL-01",
        "baudrate":        9600,
        "buffer_size":     10,
        "class":           SensorWaterLevelRKL01,
    },
}

ADDRESS_TO_TYPE = {}
for _sensor_type, _info in SENSOR_TYPE_REGISTRY.items():
    ADDRESS_TO_TYPE[_info["default"]] = _sensor_type
    _start, _end = _info["range"]
    for _addr in range(_start, _end + 1):
        ADDRESS_TO_TYPE[_addr] = _sensor_type


def get_sensor_class(sensor_type: str):
    entry = SENSOR_TYPE_REGISTRY.get(sensor_type)
    if entry is None:
        return None
    return entry["class"]


def get_default_address(sensor_type: str) -> int:
    entry = SENSOR_TYPE_REGISTRY.get(sensor_type)
    if entry is None:
        return None
    return entry["default"]


def get_address_range(sensor_type: str) -> tuple:
    entry = SENSOR_TYPE_REGISTRY.get(sensor_type)
    if entry is None:
        return None
    return entry["range"]


def get_all_scan_addresses(sensor_type: str) -> list:
    entry = SENSOR_TYPE_REGISTRY.get(sensor_type)
    if entry is None:
        return []
    default = entry["default"]
    start, end = entry["range"]
    factory = entry.get("factory_defaults", [])
    # regular range first, factory defaults appended last (lower priority)
    return [default] + list(range(start, end + 1)) + factory


def get_buffer_size(sensor_type: str, fallback: int = 10) -> int:
    entry = SENSOR_TYPE_REGISTRY.get(sensor_type)
    if entry is None:
        return fallback
    return entry.get("buffer_size", fallback)


def identify_type_by_address(address: int) -> str:
    return ADDRESS_TO_TYPE.get(address, None)


def is_address_in_range(sensor_type: str, address: int) -> bool:
    entry = SENSOR_TYPE_REGISTRY.get(sensor_type)
    if entry is None:
        return False
    start, end = entry["range"]
    return start <= address <= end