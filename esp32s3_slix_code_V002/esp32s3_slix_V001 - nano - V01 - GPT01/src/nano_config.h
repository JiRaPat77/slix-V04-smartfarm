// ════════════════════════════════════════════════════════════════════════
// NANO CONFIGURATION — แก้ตรงนี้ก่อน flash nano แต่ละตัว
//
// Nano ใช้บอร์ด PCB เดียวกับ gateway — มี 12 sensor ports (RS485 + power
// control via MCP23017) เหมือนกันทุกอย่าง แค่หน้าที่ต่างกัน
//
// แต่ละ nano ต้องตั้งสิ่งเดียวคือ "เสียบ sensor อะไรไว้ที่ port ไหน"
// (temp_id auto จาก MAC ของชิป — ไม่ต้องตั้ง)
// ════════════════════════════════════════════════════════════════════════
#pragma once
#include "sensor_types.h"

// ── 1) Sensor mapping ─────────────────────────────────────────────────
// Format: { port (1-12), type (ST_xxx), Modbus address, instance "01"... }
//
// ใช้ sensor ชนิดเดียวกันได้หลายตัวที่ instance ต่างกัน
// เช่น Soil 2 ตัวที่ port 1 (addr 1, instance "01") และ port 2 (addr 2, instance "02")
//
// แก้ array ตรงนี้ตามจริง:
struct PortConfig {
    uint8_t      port;       // Port บนบอร์ด (1-12)
    SensorTypeID type;       // ชนิด sensor (sensor_types.h)
    uint8_t      address;    // Modbus address ของ sensor
    const char*  instance;   // Instance สำหรับแยก device ใน ThingsBoard ("01", "02"...)
};

static const PortConfig PORT_CONFIG[] = {
    //  port  type            address  instance
    {   1,   ST_SOIL,         1,       "01"  },   // ตัวอย่าง: Soil RK520 ตัวที่ 1
    {   2,   ST_SOIL,         2,       "02"  },   // Soil RK520 ตัวที่ 2 (ชนิดเดียวกัน, addr ต่าง)
    // {  3,   ST_WIND,         1,       "01"  },   // Wind RK120
    // {  4,   ST_AIR_TEMP,     1,       "01"  },
    // {  5,   ST_RAINFALL,     50,      "01"  },
    // {  6,   ST_SOLAR,        1,       "01"  },
};
static const int PORT_CONFIG_COUNT = sizeof(PORT_CONFIG) / sizeof(PortConfig);

// ── 2) Timing — ปกติไม่ต้องแก้ ────────────────────────────────────────
#define NANO_READ_INTERVAL_MS    5000     // อ่าน sensor ทุก 5 วิ
#define NANO_SEND_INTERVAL_MS    10000    // ส่ง LoRa ทุก 10 วิ
                                          //   (gateway สะสม median แล้วยิง ThingsBoard ที่ 60s)
#define NANO_HELLO_INTERVAL_MS   5000     // ขณะ unpaired: HELLO ทุก 5 วิ
#define NANO_RX_WATCHDOG_MS      (10UL * 60UL * 1000UL)  // ไม่ ack 10 นาที → re-init LoRa

// ── 3) Pin (ตรงกับ PCB เดิม — เหมือน gateway) ────────────────────────
#define NANO_NEOPIXEL_PIN        21
#define NANO_BTN_RESET_PIN       45       // กดค้าง 10s = factory reset (ลบ ID, pair ใหม่)
#define NANO_I2C_SDA             42
#define NANO_I2C_SCL             41

// ── 4) MCP23017 (เหมือน gateway) ──────────────────────────────────────
#define NANO_MCP23_1_ADDR        0x27    // ports 1-8 power control
#define NANO_MCP23_2_ADDR        0x20    // ports 9-12 power control
#define NANO_MCP23_IODIRA        0x00
#define NANO_MCP23_IODIRB        0x01
#define NANO_MCP23_GPPUA         0x0C
#define NANO_MCP23_GPPUB         0x0D
#define NANO_MCP23_OLATA         0x14
#define NANO_MCP23_OLATB         0x15

// ── 5) LoRa packet limit ──────────────────────────────────────────────
#define NANO_LORA_SOFT_MAX_BYTES 200      // ถ้า payload > ค่านี้ → auto split per-port
