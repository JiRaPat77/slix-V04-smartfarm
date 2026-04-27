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

    def __init__(self, bus=3, address=0x26):
        self.bus = smbus.SMBus(bus)
        self.address = address
        self._verify_connection()
        self._setup_defaults()
        time.sleep(0.1)

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
        
        
        print("Testing MCP23017 at 0x26")
        mcp = MCP23017(bus=3, address=0x26)
        
        # Configure Port 
        mcp.set_pin_mode('B', 0, 0) # sensor_en1
        mcp.set_pin_mode('B', 1, 0) # sensor_en2
        mcp.set_pin_mode('B', 2, 0) # sensor_en3
        mcp.set_pin_mode('B', 3, 0) # sensor_en4
        
        mcp.set_pin_mode('B', 4, 1) # over_current1
        mcp.set_pin_mode('B', 5, 1) # over_current2
        mcp.set_pin_mode('B', 6, 1) # over_current3
        mcp.set_pin_mode('B', 7, 1) # over_current4
        
        mcp.set_pin_mode('A', 7, 1) # over_current8
        mcp.set_pin_mode('A', 6, 1) # over_current7
        mcp.set_pin_mode('A', 5, 1) # over_current6
        mcp.set_pin_mode('A', 4, 1) # over_current5
        
        mcp.set_pin_mode('A', 3, 0) # sensor_en8
        mcp.set_pin_mode('A', 2, 0) # sensor_en7
        mcp.set_pin_mode('A', 1, 0) # sensor_en6
        mcp.set_pin_mode('A', 0, 0) # sensor_en5
        
        # Blink LED on A0
        while True:
            mcp.write_pin('A', 2, 1)
            mcp.write_pin('B', 1, 1)
            print("On Output")
            print("Over Current1:",mcp.read_pin('B', 5))
            print("Over Current8:",mcp.read_pin('A', 7))
            time.sleep(1)
            
            mcp.write_pin('A', 1, 0)
            mcp.write_pin('B', 1, 0)
            mcp.write_pin('A', 0, 0)
        
            print("Off Output")
            print("Over Current1:",mcp.read_pin('B', 5))
            print("Over Current8:",mcp.read_pin('A', 7))
            time.sleep(1)
            
    except KeyboardInterrupt:
        print("\nCleaning up...")
        mcp.cleanup()
    except Exception as e:
        print(f"Error: {str(e)}")
        mcp.cleanup()