#!/usr/bin/env python
import smbus
import time

class MCP23017:
    # MCP23017 Registers (same for all addresses)
    IODIRA = 0x00  # I/O Direction A
    IODIRB = 0x01  # I/O Direction B
    GPIOA  = 0x12  # GPIO Port A
    GPIOB  = 0x13  # GPIO Port B
    GPPUA  = 0x0C  # Pull-up Resistor A
    GPPUB  = 0x0D  # Pull-up Resistor B

    def __init__(self, bus=3, address=0x25):
        self.bus = smbus.SMBus(bus)
        self.address = address
        self._verify_connection()
        self._setup_defaults()

    def _verify_connection(self):
        """Verify device responds at address"""
        try:
            self.bus.write_quick(self.address)
            print(f"MCP23017 found at 0x{self.address:02X}")
        except IOError:
            raise RuntimeError(f"No device at 0x{self.address:02X} - Check wiring/address")

    def _setup_defaults(self):
        """Initialize with all inputs + pull-ups"""
        self._write_register(self.IODIRA, 0xFF)  # All inputs
        self._write_register(self.IODIRB, 0xFF)
        self._write_register(self.GPPUA, 0xFF)   # Enable pull-ups
        self._write_register(self.GPPUB, 0xFF)

    def _write_register(self, reg, value):
        self.bus.write_byte_data(self.address, reg, value)

    def _read_register(self, reg):
        return self.bus.read_byte_data(self.address, reg)

    def set_pin_mode(self, port, pin, mode):
        """Set pin direction: 0=output, 1=input"""
        reg = self.IODIRA if port.upper() == 'A' else self.IODIRB
        current = self._read_register(reg)
        mask = 1 << pin
        new = (current & ~mask) if mode == 0 else (current | mask)
        self._write_register(reg, new)

    def write_pin(self, port, pin, value):
        """Write output pin: 0=low, 1=high"""
        reg = self.GPIOA if port.upper() == 'A' else self.GPIOB
        current = self._read_register(reg)
        mask = 1 << pin
        new = (current & ~mask) | (value << pin)
        self._write_register(reg, new)

    def read_pin(self, port, pin):
        """Read input pin: returns 0 or 1"""
        reg = self.GPIOA if port.upper() == 'A' else self.GPIOB
        return (self._read_register(reg) >> pin) & 0x01
        
    def cleanup(self):
        self.bus.close()

# Example Usage
if __name__ == "__main__":
    try:
        
        
        print("Testing MCP23017 at 0x25")
        mcp = MCP23017(bus=3, address=0x25)
        
        # Configure Port 
        mcp.set_pin_mode('B', 0, 1) # sensor_check1
        mcp.set_pin_mode('B', 1, 1) # sensor_check2
        mcp.set_pin_mode('B', 2, 1) # sensor_check3
        mcp.set_pin_mode('B', 3, 1) # sensor_check4
        
        mcp.set_pin_mode('B', 4, 1) # sensor_check5
        mcp.set_pin_mode('B', 5, 1) # sensor_check6
        mcp.set_pin_mode('B', 6, 1) # sensor_check7
        mcp.set_pin_mode('B', 7, 1) # sensor_check8
        
        mcp.set_pin_mode('A', 7, 0) # system_LED
        mcp.set_pin_mode('A', 6, 1) # jumper_mode1
        mcp.set_pin_mode('A', 5, 1) # jumper_mode2
        mcp.set_pin_mode('A', 4, 1) # jumper_mode3
        
        mcp.set_pin_mode('A', 3, 1) # sensor_check12
        mcp.set_pin_mode('A', 2, 1) # sensor_check11
        mcp.set_pin_mode('A', 1, 1) # sensor_check10
        mcp.set_pin_mode('A', 0, 1) # sensor_check9
        
        # Blink LED on A0
        while True:
            
            print("mode")
            mcp.write_pin('A', 7, 1)
            print("jumper 1:",mcp.read_pin('A', 6))
            print("jumper 2:",mcp.read_pin('A', 5))
            print("jumper 3:",mcp.read_pin('A', 4))
            print("port 1:",mcp.read_pin('B', 0))
            time.sleep(1)
            mcp.write_pin('A', 7, 0)
            time.sleep(1)
            
    except KeyboardInterrupt:
        print("\nCleaning up...")
        mcp.cleanup()
    except Exception as e:
        print(f"Error: {str(e)}")
        mcp.cleanup()