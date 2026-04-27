import sys
import os

current_dir = os.path.dirname(os.path.abspath(__file__))
parent_dir = os.path.dirname(current_dir)
sys.path.append(parent_dir)
from class_sensor.class_soilPH_RK500 import SensorSoilPHRK500_22
sensor = SensorSoilPHRK500_22(port="/dev/ttyS4", slave_address=0x64)

# อ่านข้อมูลทั้งหมด
data = sensor.read_data()
if data["success"]:
    print(data)


# อ่านเฉพาะค่า pH
# ph_value = sensor.read_ph_only()
# print(f"pH: {ph_value:.2f}")

# # จำแนกความเป็นกรด-ด่างของดิน
# classification = SensorSoilPHRK500_22.classify_soil_ph(ph_value)
# print(f"Soil level: {classification['level']}")
# print(f"Suitable for: {', '.join(classification['suitable_crops'])}")

# # ตรวจสอบความเหมาะสมสำหรับพืชเฉพาะ
# crop_info = SensorSoilPHRK500_22.get_optimal_ph_for_crop("tomato")
# is_suitable = crop_info['optimal_ph_range'][0] <= ph_value <= crop_info['optimal_ph_range'][1]
# print(f"Suitable for tomatoes: {'Yes' if is_suitable else 'No'}")

#เปลี่ยน address
# success = sensor.set_address(0x64)

# if success:
#     print("Chance address success!")
#     print(f"New Address : 0x{sensor.slave_address:02X}")
# else:
#     print("Failed to chance address")

# sensor.close()
