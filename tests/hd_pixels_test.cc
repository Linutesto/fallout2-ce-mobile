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
    for (int i = 0; i < 256; i++)
        assert(hdIsCycling(i) == (i >= 229 && i <= 254));
    unsigned char lightTable[256 * 256] = { };
    lightTable[7 * 256 + 64] = 23;
    assert(hdLightIndex(7, 32768, lightTable) == 23);
    assert(hdLightIndex(0, 32768, lightTable) == 0);
    assert(hdLightIndex(229, 32768, lightTable) == 229);
    uint32_t palette[256] = { };
    palette[229] = 0xFF00ABCD;
    assert(hdCyclePixel(0x12345678, 229, palette) == 0xFF00ABCD);
    assert(hdCyclePixel(0x12345678, 228, palette) == 0x12345678);
    assert(hdCyclePixel(0x12345678, 255, palette) == 0x12345678);
    int light[] = { 0, 65536, 32768, 65536 };
    assert(hdInterpolateLight(light, 2, 2, 2, 0, 0) == 0);
    assert(hdInterpolateLight(light, 2, 2, 2, 1, 0) == 32768);
    assert(hdInterpolateLight(light, 2, 2, 2, 1, 1) == 40960);
    assert(hdInterpolateLight(light, 2, 2, 2, 3, 3) == 65536);
    assert(hdOver(0x00FFFFFF, 0xFF102030) == 0xFF102030);
    assert(hdOver(0xFFFFFFFF, 0xFF000000, 128) == 0xFF808080);
    assert(hdOver(0x80FFFFFF, 0xFF000000) == 0xFF808080);
    assert(hdBlendRow(0xFFFFFFFF, false) == 7);
    assert(hdBlendRow(0xFFFFFFFF, true) == 7);
    assert(hdBlendRow(0xFF800000, false) == 1);
    assert(hdBlendRow(0xFF800000, true) == 0);
    assert(hdBlendRow(0xFF008000, false) == 2);
    assert(hdBlend(0xFF123456, 0xFFAA0000, 0) == 0xFF123456);
    assert(hdBlend(0xFF123456, 0xFFAA0000, 7) == 0xFFAA0000);
    assert(hdBlend(0xFF000000, 0xFF700000, 8) == 0xFF700000);
    assert(hdBlend(0xFF000000, 0xFF700000, 9) == 0xFF842424);
}
