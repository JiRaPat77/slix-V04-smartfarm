#!/usr/bin/env python
import sys
import os
import time
import threading

# Import MCP libraries
sys.path.append(os.path.dirname(__file__))
from mcp_1 import MCP23017 as MCP1  # Sensor 1-8
from mcp_2 import MCP23017 as MCP2  # Sensor 9-16
from mcp_3 import MCP23017 as MCP3  # Sensor check & system

class SensorControlSystem:
    def __init__(self, ignore_overcurrent=False):
        print("Initializing MCP controllers...")
        self.ignore_overcurrent = ignore_overcurrent
        
        self.mcp1 = None
        self.mcp2 = None
        self.mcp3 = None
        self.mcp1_ready = False
        self.mcp2_ready = False
        self.mcp3_ready = False

        # --- MCP 1 (Sensor 1-8) ---
        try:
            self.mcp1 = MCP1(bus=3, address=0x27)
            self.mcp1_ready = True
            print("MCP1 (Sensor 1-8) initialized.")
        except Exception as e:
            print(f"MCP1 Init Failed: {e} -> Skipping MCP1")
            self.mcp1_ready = False

        # --- MCP 2 (Sensor 9-16) ---
        try:
            self.mcp2 = MCP2(bus=3, address=0x20)
            self.mcp2_ready = True
            print("MCP2 (Sensor 9-16) initialized.")
        except Exception as e:
            print(f"MCP2 Init Failed: {e} -> Skipping MCP2")
            self.mcp2_ready = False

        # --- MCP 3 (System & Check) ---
        try:
            self.mcp3 = MCP3(bus=3, address=0x21)
            self.mcp3_ready = True
            print("MCP3 (System Control) initialized.")
        except Exception as e:
            print(f"MCP3 Init Failed: {e} -> Skipping MCP3")
            self.mcp3_ready = False
        
        # Sensor status tracking
        self.sensor_status = {}      # เก็บสถานะการเสียบสาย
        self.overcurrent_status = {} # เก็บสถานะ Overcurrent
        self.power_status = {}       # เก็บสถานะการจ่ายไฟ
        
        self.setup_mcp_pins()
        
    def setup_mcp_pins(self):
        print("Configuring MCP pins...")
        def safe_set_pin(mcp, port, pin, mode, description):
            if not mcp: return False
            try:
                mcp.set_pin_mode(port, pin, mode)
                time.sleep(0.01)
                return True
            except Exception as e:
                print(f"Error setting {description}: {e}")
                return False
        
        if self.mcp1_ready:
            # sensor_en 1-4 → B bits 0-3 output; sensor_en 5-8 → A bits 0-3 output
            for i in range(4):
                safe_set_pin(self.mcp1, 'B', i,   0, f"sensor_en{i+1}")
                safe_set_pin(self.mcp1, 'A', i,   0, f"sensor_en{i+5}")
            # OC 1-4 → B bits 4-7 input; OC 5-8 → A bits 4-7 input
            for i in range(4):
                safe_set_pin(self.mcp1, 'B', 4+i, 1, f"over_current{i+1}")
                safe_set_pin(self.mcp1, 'A', 4+i, 1, f"over_current{i+5}")

        if self.mcp2_ready:
            # sensor_en 9-12 → B bits 0-3 output; sensor_en 13-16 → A bits 0-3 output
            for i in range(4):
                safe_set_pin(self.mcp2, 'B', i,   0, f"sensor_en{i+9}")
                safe_set_pin(self.mcp2, 'A', i,   0, f"sensor_en{i+13}")
            # OC 9-12 → B bits 4-7 input; OC 13-16 → A bits 4-7 input
            for i in range(4):
                safe_set_pin(self.mcp2, 'B', 4+i, 1, f"over_current{i+9}")
                safe_set_pin(self.mcp2, 'A', 4+i, 1, f"over_current{i+13}")

        if self.mcp3_ready:
            # sensor_check 1-8 → B bits 0-7 input
            for i in range(8):
                safe_set_pin(self.mcp3, 'B', i, 1, f"sensor_check{i+1}")
            # sensor_check 9-12 → A bits 0-3 input
            for i in range(4):
                safe_set_pin(self.mcp3, 'A', i, 1, f"sensor_check{i+9}")
            safe_set_pin(self.mcp3, 'A', 7, 0, "system_LED")
            for i in range(3):
                safe_set_pin(self.mcp3, 'A', 4+i, 1, f"jumper_mode{i+1}")
            
    def turn_on_all_sensors(self):
        print("Turning ON all sensor power supplies...")
        if self.mcp1_ready:
            for i in range(4):
                self.mcp1.write_pin('B', i, 1)
                self.mcp1.write_pin('A', i, 1)
            for p in range(1, 9): self.power_status[p] = True

        if self.mcp2_ready:
            for i in range(4):
                self.mcp2.write_pin('B', i, 1)
                self.mcp2.write_pin('A', i, 1)
            for p in range(9, 17): self.power_status[p] = True

    def turn_off_all_sensors(self):
        print("Turning OFF all sensor power supplies...")
        if self.mcp1_ready:
            try:
                for i in range(4):
                    self.mcp1.write_pin('B', i, 0)
                    self.mcp1.write_pin('A', i, 0)
                for p in range(1, 9): self.power_status[p] = False
            except Exception as e:
                print(f"Error MCP1 turn off: {e}")

        if self.mcp2_ready:
            try:
                for i in range(4):
                    self.mcp2.write_pin('B', i, 0)
                    self.mcp2.write_pin('A', i, 0)
                for p in range(9, 17): self.power_status[p] = False
            except Exception as e:
                print(f"Error MCP2 turn off: {e}")
            
    def turn_on_sensor(self, sensor_num):
        if not (1 <= sensor_num <= 16): return
        if 1 <= sensor_num <= 8 and self.mcp1_ready:
            if sensor_num <= 4: self.mcp1.write_pin('B', sensor_num - 1,  1)
            else:               self.mcp1.write_pin('A', sensor_num - 5,  1)
        elif 9 <= sensor_num <= 16 and self.mcp2_ready:
            if sensor_num <= 12: self.mcp2.write_pin('B', sensor_num - 9,  1)
            else:                self.mcp2.write_pin('A', sensor_num - 13, 1)
        self.power_status[sensor_num] = True

    def turn_off_sensor(self, sensor_num):
        if not (1 <= sensor_num <= 16): return
        if 1 <= sensor_num <= 8 and self.mcp1_ready:
            if sensor_num <= 4: self.mcp1.write_pin('B', sensor_num - 1,  0)
            else:               self.mcp1.write_pin('A', sensor_num - 5,  0)
        elif 9 <= sensor_num <= 16 and self.mcp2_ready:
            if sensor_num <= 12: self.mcp2.write_pin('B', sensor_num - 9,  0)
            else:                self.mcp2.write_pin('A', sensor_num - 13, 0)
        self.power_status[sensor_num] = False
    
    def check_overcurrent(self):
        if self.mcp1_ready:
            for i in range(4):
                self._handle_fault(i + 1, self.mcp1.read_pin('B', 4+i) == 1)
                self._handle_fault(i + 5, self.mcp1.read_pin('A', 4+i) == 1)

        if self.mcp2_ready:
            for i in range(4):
                self._handle_fault(i + 9,  self.mcp2.read_pin('B', 4+i) == 1)
                self._handle_fault(i + 13, self.mcp2.read_pin('A', 4+i) == 1)

        return [p for p, fault in self.overcurrent_status.items() if fault]

    def _handle_fault(self, port, is_fault):
        self.overcurrent_status[port] = is_fault
        if is_fault:
            if not self.ignore_overcurrent:
                print(f"OVERCURRENT Port {port} -> Turning OFF (Protection Active)")
                self.turn_off_sensor(port)
            else:
                print(f"OVERCURRENT Port {port} -> IGNORED (Force Read Active), Keeping ON")

    def check_sensor_connection(self):
        connected, disconnected = [], []
        if not self.mcp3_ready: return [], []

        for i in range(8):
            port = i + 1
            status = self.mcp3.read_pin('B', i)
            self.sensor_status[port] = status
            if status == 0: connected.append(port)
            else: disconnected.append(port)

        for i in range(4):
            port = i + 9
            status = self.mcp3.read_pin('A', i)
            self.sensor_status[port] = status
            if status == 1: connected.append(port)
            else: disconnected.append(port)

        return connected, disconnected

    def get_all_port_statuses(self):
        status_report = {}
        for port in range(1, 17):
            is_connected = False
            if port in self.sensor_status:
                val = self.sensor_status[port]
                if 1 <= port <= 8:   is_connected = (val == 0)
                elif 9 <= port <= 12: is_connected = (val == 1)
                # ports 13-16 have no sensor_check pin on PCB, always False

            is_overcurrent = self.overcurrent_status.get(port, False)
            is_power_on    = self.power_status.get(port, False)

            status_report[port] = {
                "connected":   is_connected,
                "overcurrent": is_overcurrent,
                "power_on":    is_power_on,
            }
        return status_report