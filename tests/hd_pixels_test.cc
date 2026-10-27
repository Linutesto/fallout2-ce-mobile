// CE: HD task item 2/5: independent lighting, cycling and RGB blend vectors.
#include "hd_pixels.h"
#include <cassert>
using namespace fallout;
int main()
{
    assert(hdLight(0x7FC86432, 32768) == 0x7F643219);
    assert(hdLight(0xFFFFFFFF, 65536) == 0xFFFFFFFF);
    assert(hdLight(0xABCDEF12, -1) == 0xAB000000);
    assert(hdLight(0x12345678, 999999) == 0x12345678);
    for (int i=0;i<256;i++) assert(hdIsCycling(i) == (i>=229 && i<=254));
    assert(hdOver(0x00FFFFFF, 0xFF102030) == 0xFF102030);
    assert(hdOver(0xFFFFFFFF, 0xFF000000, 128) == 0xFF808080);
    assert(hdOver(0x80FFFFFF, 0xFF000000) == 0xFF808080);
    assert(hdBlend(0xFF123456, 0xFFAA0000, 0) == 0xFF123456);
    assert(hdBlend(0xFF123456, 0xFFAA0000, 7) == 0xFFAA0000);
    assert(hdBlend(0xFF000000, 0xFF700000, 8) == 0xFF700000);
    assert(hdBlend(0xFF000000, 0xFF700000, 9) == 0xFF842424);
}
