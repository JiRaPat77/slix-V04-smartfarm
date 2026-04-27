import sys
import time
import logging
import threading

from plug_and_play.sensor_registry import (
    SENSOR_TYPE_REGISTRY,
    get_all_scan_addresses,
    identify_type_by_address,
    is_address_in_range,
)
from plug_and_play.active_sensors_manager import ActiveSensorsManager

logger = logging.getLogger("scan")

_SCAN_RETRIES    = 3      # จำนวนครั้งที่ลองอ่านซ้ำต่อ address
_SCAN_RETRY_DELAY = 1.0  # วินาทีรอระหว่าง retry


def _try_read(sensor_type: str, address: int, serial_port: str,
              rs485_lock: threading.Lock = None,
              max_retries: int = _SCAN_RETRIES,
              retry_delay: float = _SCAN_RETRY_DELAY) -> bool:
    """
    พยายามอ่านค่าจาก sensor ที่ address นั้น
    คืน True ถ้าตอบกลับมาได้ (ไม่ว่าค่าจะสมเหตุสมผลหรือเปล่า)
    คืน False ถ้า timeout หรือ exception ครบทุก retry
    ใช้ rs485_lock ถ้ามี เพื่อป้องกัน race condition กับ Thread-Sensor
    """
    entry = SENSOR_TYPE_REGISTRY.get(sensor_type)
    if entry is None:
        return False

    sensor_class = entry["class"]
    baudrate     = entry["baudrate"]

    for attempt in range(1, max_retries + 1):
        try:
            def _do_read():
                instance = sensor_class(
                    port=serial_port,
                    slave_address=address,
                    baudrate=baudrate,
                )
                result = None
                if sensor_type == "soil":
                    result = instance.read_data(addr=address)
                elif sensor_type == "soil_ec":
                    result = instance.read_data(addr=address)
                elif sensor_type == "soil_ph":
                    result = instance.read_data(addr=address)
                elif sensor_type == "air_temp":
                    result = instance.read_temp()
                elif sensor_type == "wind":
                    result = instance.read_wind(addr=address)
                elif sensor_type == "rainfall":
                    result = instance.read_tip()
                elif sensor_type == "solar":
                    result = instance.read_radiation(addr=address)
                elif sensor_type == "ultrasonic":
                    result = instance.read_distance()
                elif sensor_type == "liquid_level":
                    result = instance.read_water_level(addr=address)
                if hasattr(instance, "close"):
                    instance.close()
                return result

            if rs485_lock is not None:
                with rs485_lock:
                    result = _do_read()
            else:
                result = _do_read()

            if result is None:
                logger.debug(
                    f"  [{sensor_type}@{address}] attempt {attempt}/{max_retries}: no response"
                )
                if attempt < max_retries:
                    time.sleep(retry_delay)
                continue

            # ถือว่าตอบกลับมาได้ ถึงแม้ค่าจะผิดปกติ
            logger.debug(
                f"  [{sensor_type}@{address}] attempt {attempt}/{max_retries}: responded"
            )
            return True

        except Exception as e:
            logger.debug(
                f"  [{sensor_type}@{address}] attempt {attempt}/{max_retries}: exception: {e}"
            )
            if attempt < max_retries:
                time.sleep(retry_delay)

    return False


def _find_free_address(sensor_type: str, used_addresses: set) -> int:
    """
    หา address ว่างแรกสุดใน range ของ type นั้น
    คืน address ที่ว่าง หรือ None ถ้าเต็ม
    """
    from plug_and_play.sensor_registry import get_address_range
    start, end = get_address_range(sensor_type)
    for addr in range(start, end + 1):
        if addr not in used_addresses:
            return addr
    return None


def _set_sensor_address(sensor_type: str, current_address: int,
                        new_address: int, serial_port: str,
                        rs485_lock: threading.Lock = None) -> bool:
    """
    ส่งคำสั่ง set_address ไปยัง sensor
    คืน True ถ้าสำเร็จ
    """
    entry = SENSOR_TYPE_REGISTRY.get(sensor_type)
    if entry is None:
        return False

    sensor_class = entry["class"]
    baudrate     = entry["baudrate"]

    def _do_set():
        instance = sensor_class(
            port=serial_port,
            slave_address=current_address,
            baudrate=baudrate,
        )
        result = instance.set_address(new_address)
        if hasattr(instance, "close"):
            instance.close()
        if isinstance(result, bool):
            return result
        if isinstance(result, dict):
            return result.get("success", False)
        # ถ้า None หรือ type อื่นให้ถือว่าสำเร็จ (backward compat)
        return result is not None

    try:
        if rs485_lock is not None:
            with rs485_lock:
                return _do_set()
        else:
            return _do_set()
    except Exception as e:
        logger.error(f"set_address failed for {sensor_type} at {current_address}: {e}")
        return False


# -----------------------------------------------------------------------
# main function
# -----------------------------------------------------------------------

def run_deep_scan(port: str, serial_port: str,
                  manager: ActiveSensorsManager,
                  power_on_wait_sec: float = 3.0,
                  between_address_wait_sec: float = 0.05,
                  rs485_lock: threading.Lock = None) -> bool:
    """
    Phase 4: Deep Scan สำหรับ port ที่เพิ่งเสียบ sensor เข้ามาใหม่

    ขั้นตอน:
    1. รอให้ sensor พร้อม (power_on_wait_sec)
    2. sweep ทุก sensor type ทีละ address (default ก่อน แล้วตาม range)
    3. พอ sensor ตอบกลับ -> ระบุ type และ address ที่ได้
    4. เช็ค reserved -> ถ้าเจอ address ตรง คืน instance เดิม
    5. เช็ค collision -> ถ้าไม่ชน ใช้ address ปัจจุบันได้เลย
    6. ถ้าชน -> หา address ว่างใน range แล้ว set_address ใหม่
    7. ถ้า range เต็ม -> reclaim oldest reserved
    8. บันทึกลง active_sensors.json

    คืน True ถ้า scan และลงทะเบียนสำเร็จ
    """
    logger.info(
        f"[DeepScan] Port {port}: starting scan "
        f"(waiting {power_on_wait_sec}s for sensor ready)"
    )
    time.sleep(power_on_wait_sec)

    found_type    = None
    found_address = None

    # -----------------------------------------------------------------------
    # ขั้นตอนที่ 2: sweep ทุก type ทุก address
    # -----------------------------------------------------------------------
    for sensor_type in SENSOR_TYPE_REGISTRY:
        scan_addresses = get_all_scan_addresses(sensor_type)
        logger.info(
            f"[DeepScan] Port {port}: scanning type={sensor_type} "
            f"addresses={scan_addresses}"
        )

        for address in scan_addresses:
            time.sleep(between_address_wait_sec)
            responded = _try_read(
                sensor_type, address, serial_port,
                rs485_lock=rs485_lock,
            )
            if responded:
                logger.info(
                    f"[DeepScan] Port {port}: response from "
                    f"type={sensor_type} address={address}"
                )
                found_type    = sensor_type
                found_address = address
                break

        if found_type is not None:
            break

    if found_type is None:
        logger.warning(
            f"[DeepScan] Port {port}: no sensor responded on any address "
            f"-> scan failed"
        )
        return False

    # -----------------------------------------------------------------------
    # ขั้นตอนที่ 4: เช็ค reserved ก่อน
    # -----------------------------------------------------------------------
    reserved_entry = manager.get_reserved_by_address(found_address)
    if reserved_entry is not None and reserved_entry.get("type") == found_type:
        final_address  = found_address
        final_instance = reserved_entry["instance"]
        final_model    = reserved_entry["model"]
        logger.info(
            f"[DeepScan] Port {port}: recognized returning sensor "
            f"address={final_address} instance={final_instance} "
            f"(restored from reserved)"
        )
        manager.add_port(port, found_type, final_address, final_model, final_instance)
        return True

    # -----------------------------------------------------------------------
    # ขั้นตอนที่ 5: เช็ค collision กับ active ปัจจุบัน
    # -----------------------------------------------------------------------
    used_addresses = manager.get_used_addresses()
    entry_info     = SENSOR_TYPE_REGISTRY[found_type]
    final_model    = entry_info["model"]

    if found_address not in used_addresses:
        final_address = found_address
        logger.info(
            f"[DeepScan] Port {port}: address {final_address} is free "
            f"-> no re-addressing needed"
        )
    else:
        # -----------------------------------------------------------------------
        # ขั้นตอนที่ 6: ชน -> หา address ว่างใน range
        # -----------------------------------------------------------------------
        logger.warning(
            f"[DeepScan] Port {port}: address {found_address} already used "
            f"-> finding free address for type={found_type}"
        )
        free_address = _find_free_address(found_type, used_addresses)

        if free_address is None:
            # -----------------------------------------------------------------------
            # ขั้นตอนที่ 7: range เต็ม -> reclaim oldest reserved
            # -----------------------------------------------------------------------
            logger.warning(
                f"[DeepScan] Port {port}: range full for type={found_type} "
                f"-> reclaiming oldest reserved"
            )
            reclaimed = manager.reclaim_oldest_reserved(found_type)
            if reclaimed is None:
                logger.error(
                    f"[DeepScan] Port {port}: range full and no reserved to reclaim "
                    f"for type={found_type} -> scan failed"
                )
                return False

            free_address = reclaimed["address"]
            logger.warning(
                f"[DeepScan] Port {port}: reclaimed address {free_address} "
                f"(was instance={reclaimed.get('instance')})"
            )

        logger.info(
            f"[DeepScan] Port {port}: sending set_address "
            f"{found_address} -> {free_address}"
        )
        success = _set_sensor_address(
            found_type, found_address, free_address, serial_port,
            rs485_lock=rs485_lock,
        )
        if not success:
            logger.error(
                f"[DeepScan] Port {port}: set_address failed "
                f"({found_address} -> {free_address}) -> scan failed"
            )
            return False

        final_address = free_address
        logger.info(
            f"[DeepScan] Port {port}: set_address success "
            f"-> new address={final_address}"
        )

    # -----------------------------------------------------------------------
    # ขั้นตอนที่ 8: กำหนด instance และบันทึก
    # -----------------------------------------------------------------------
    instance_num   = manager.get_instance_count(found_type) + 1
    final_instance = f"{instance_num:02d}"

    manager.add_port(port, found_type, final_address, final_model, final_instance)

    logger.info(
        f"[DeepScan] Port {port}: registered -> "
        f"type={found_type} address={final_address} "
        f"model={final_model} instance={final_instance}"
    )
    return True
