// Host stub of Arduino.h — just enough for JsonReadUtils.h to compile and run
// on a PC. Not used on device.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

class IPAddress {
public:
    IPAddress() { _o[0]=_o[1]=_o[2]=_o[3]=0; }
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { _o[0]=a;_o[1]=b;_o[2]=c;_o[3]=d; }
    uint8_t operator[](int i) const { return _o[i]; }
private:
    uint8_t _o[4];
};
