import os
import json
import time
import threading
import logging

logger = logging.getLogger("pnp")

# โครงสร้าง active_sensors.json
# {
#   "active": {
#     "1": {"type": "air_temp", "address": 41, "model": "MW485", "instance": "01"}
#   },
#   "reserved": {
#     "41": {"type": "air_temp", "instance": "01", "model": "MW485",
#            "last_seen_port": "1", "reserved_at": 1718000000}
#   }
# }

class ActiveSensorsManager:
    def __init__(self, file_path: str):
        self.file_path = file_path
        self._lock = threading.Lock()
        self._ensure_file_exists()

    # -----------------------------------------------------------------------
    # private: จัดการไฟล์
    # -----------------------------------------------------------------------

    def _ensure_file_exists(self):
        """สร้างไฟล์เปล่าถ้ายังไม่มี"""
        if not os.path.exists(self.file_path):
            dir_path = os.path.dirname(self.file_path)
            if dir_path and not os.path.exists(dir_path):
                os.makedirs(dir_path)
            self._write_data({"active": {}, "reserved": {}})
            logger.info(f"Created new active_sensors.json at {self.file_path}")

    def _read_data(self) -> dict:
        """อ่านไฟล์ดิบ ไม่มี lock (ต้องถือ lock ก่อนเรียก)"""
        try:
            with open(self.file_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if "active" not in data:
                data["active"] = {}
            if "reserved" not in data:
                data["reserved"] = {}
            return data
        except (json.JSONDecodeError, IOError) as e:
            logger.error(f"Failed to read active_sensors.json: {e} -> returning empty")
            return {"active": {}, "reserved": {}}

    def _write_data(self, data: dict):
        """เขียนไฟล์แบบ atomic (write temp -> replace) ไม่มี lock (ต้องถือ lock ก่อนเรียก)"""
        tmp_path = self.file_path + ".tmp"
        try:
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
            os.replace(tmp_path, self.file_path)
        except IOError as e:
            logger.error(f"Failed to write active_sensors.json: {e}")
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    # -----------------------------------------------------------------------
    # public: อ่านข้อมูล
    # -----------------------------------------------------------------------

    def load(self) -> dict:
        """
        โหลด active sensors ทั้งหมด
        คืน dict ของ active เท่านั้น เช่น {"1": {"type": ..., "address": ...}}
        """
        with self._lock:
            data = self._read_data()
        return data["active"]

    def get_used_addresses(self) -> set:
        """
        คืน set ของ address ที่ถูกใช้งานอยู่ใน active ทั้งหมด
        ใช้ตอน deep scan เพื่อเช็ค collision
        """
        with self._lock:
            data = self._read_data()
        return {entry["address"] for entry in data["active"].values()}

    def get_reserved_by_address(self, address: int) -> dict:
        """
        หา entry ใน reserved ด้วย address
        คืน dict ของ entry นั้น หรือ None ถ้าไม่เจอ
        """
        with self._lock:
            data = self._read_data()
        return data["reserved"].get(str(address), None)

    def get_all_reserved_addresses(self) -> dict:
        """คืน reserved ทั้งหมด key เป็น address string"""
        with self._lock:
            data = self._read_data()
        return data["reserved"]

    def get_instance_count(self, sensor_type: str) -> int:
        """
        นับจำนวน sensor ชนิดนั้นที่มีอยู่ใน active เท่านั้น
        (ไม่นับ reserved เพื่อป้องกัน instance number บวมไปเรื่อยๆ)
        ใช้สำหรับกำหนด instance number ของตัวใหม่
        """
        with self._lock:
            data = self._read_data()

        count = 0
        for entry in data["active"].values():
            if entry.get("type") == sensor_type:
                count += 1
        return count

    # -----------------------------------------------------------------------
    # public: เขียนข้อมูล
    # -----------------------------------------------------------------------

    def add_port(self, port: str, sensor_type: str, address: int,
                 model: str, instance: str):
        """
        เพิ่ม sensor ใหม่เข้า active
        ถ้า address นั้นมีอยู่ใน reserved ให้ลบออกจาก reserved ด้วย (sensor กลับมา)
        """
        with self._lock:
            data = self._read_data()

            data["active"][str(port)] = {
                "type":     sensor_type,
                "address":  address,
                "model":    model,
                "instance": instance,
            }

            # ลบออกจาก reserved ถ้ามี (sensor กลับมาจากที่ไหนก็ตาม)
            addr_key = str(address)
            if addr_key in data["reserved"]:
                del data["reserved"][addr_key]
                logger.info(f"Removed address {address} from reserved (sensor returned)")

            self._write_data(data)

        logger.info(
            f"Added port {port} -> type={sensor_type} address={address} "
            f"model={model} instance={instance}"
        )

    def remove_port(self, port: str):
        """
        ถอด sensor ออกจาก active -> ย้ายไป reserved
        เก็บ address, type, instance, model และ timestamp ไว้
        """
        with self._lock:
            data = self._read_data()

            port_key = str(port)
            entry = data["active"].get(port_key)

            if entry is None:
                logger.warning(f"remove_port: port {port} not found in active")
                return

            # ย้ายไป reserved โดยใช้ address เป็น key
            addr_key = str(entry["address"])
            data["reserved"][addr_key] = {
                "type":           entry["type"],
                "instance":       entry["instance"],
                "model":          entry["model"],
                "last_seen_port": port_key,
                "reserved_at":    int(time.time()),
            }

            del data["active"][port_key]
            self._write_data(data)

        logger.info(
            f"Removed port {port} -> moved address {entry['address']} to reserved "
            f"(type={entry['type']} instance={entry['instance']})"
        )

    def reclaim_oldest_reserved(self, sensor_type: str) -> dict:
        """
        เมื่อ range เต็ม ดึง reserved entry ที่เก่าที่สุดของ type นั้นออกมา
        ลบออกจาก reserved และคืน entry นั้น (caller ต้องจัดการต่อเอง)
        คืน None ถ้าไม่มี reserved ของ type นั้นเลย
        """
        with self._lock:
            data = self._read_data()

            # กรอง reserved ที่ตรง type
            candidates = {
                addr: entry
                for addr, entry in data["reserved"].items()
                if entry.get("type") == sensor_type
            }

            if not candidates:
                return None

            # เอาอันที่ reserved_at น้อยที่สุด (เก่าที่สุด)
            oldest_addr = min(candidates, key=lambda a: candidates[a].get("reserved_at", 0))
            reclaimed = data["reserved"].pop(oldest_addr)
            reclaimed["address"] = int(oldest_addr)

            self._write_data(data)

        logger.warning(
            f"Reclaimed oldest reserved address {oldest_addr} "
            f"(type={sensor_type} instance={reclaimed.get('instance')})"
        )
        return reclaimed