import serial
import minimalmodbus
import time
import struct
class Modbus_Film69():
    # def __init__(self, port="/dev/ttyS2", slaveaddress=1, baudrate=9600):
                 
    #     #self.instrument = minimalmodbus.Instrument(port,slaveaddress=slaveaddress, debug=False)
    #     self.instrument = minimalmodbus.Instrument(port, slaveaddress)
    #     self.instrument.serial.baudrate = baudrate
    #     self.instrument.serial.parity = serial.PARITY_NONE
    #     self.instrument.serial.bytesize = 8
    #     self.instrument.serial.stopbits = 1
    #     self.instrument.mode = minimalmodbus.MODE_RTU
    #     self.instrument.serial.timeout = 0.300
        # instrument.address =1
    
    def __init__(self, port="/dev/ttyS2", slaveaddress=1, baudrate=9600):
        self.instrument = minimalmodbus.Instrument(port, slaveaddress)
        self.instrument.serial.baudrate = baudrate
        self.instrument.serial.parity = serial.PARITY_NONE
        self.instrument.serial.bytesize = 8
        self.instrument.serial.stopbits = 1
        self.instrument.mode = minimalmodbus.MODE_RTU
        self.instrument.serial.timeout = 0.500

    def calculate_crc(self,input):
        input=input+" "
        data = bytearray.fromhex(input)
        crc = 0xFFFF
        polynomial = 0xA001

        for byte in data:
            crc ^= byte
            for _ in range(8):
                if crc & 0x0001:
                    crc >>= 1
                    crc ^= polynomial
                else:
                    crc >>= 1
        
        return input+"{:02X} {:02X}".format(crc & 0xFF, (crc >> 8) & 0xFF)

    def hex_to_float(self,hex_str):
        hex_str=hex_str.replace(" ","")
        # Ensure the hex string is exactly 8 characters long
        if len(hex_str) != 8:
            raise ValueError("Hex string must be 8 characters long")
        # Adjust the byte order (ABCD -> DCBA)
        swapped_hex_str = hex_str[6:8] + hex_str[4:6] + hex_str[2:4] + hex_str[0:2]
        # Convert the swapped hex string to an integer
        int_value = int(swapped_hex_str, 16)
        # Convert the integer to a float using struct
        float_value = struct.unpack('!f', struct.pack('!I', int_value))[0]
        return float_value

    def encode(self,hex):
        return bytes(bytearray.fromhex(self.calculate_crc(hex)))
    
    def decode(self,Bytes):
        return " ".join([f"{x:02X}" for x in Bytes]) , " ({} bytes)".format(len(Bytes))
    def send(self,hex,resopne_len=20,ID=1):
        self.instrument.address = ID
        res = self.instrument._communicate(self.encode(hex),resopne_len)
        return self.decode(res)
    def close(self):
        self.instrument.serial.close()

if __name__ == "__main__":
    ser=Modbus_Film69("/dev/ttyS2")
    while 1:
        rec,len_Bytes=ser.send("01 03 20 00 00 04")
        print(rec,len_Bytes)

