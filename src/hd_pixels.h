#ifndef FALLOUT_HD_PIXELS_H_
#define FALLOUT_HD_PIXELS_H_
#include <algorithm>
#include <cstdint>
namespace fallout {
// CE: RGB forms of the color.cc intensity and blend-table formulas. Alpha
// remains coverage; lighting never makes a sprite more transparent.
inline uint32_t hdLight(uint32_t c, int intensity)
{
    intensity = std::clamp(intensity, 0, 65536);
    uint32_t out = c & 0xFF000000;
    for (int shift : { 0, 8, 16 }) out |= (((c >> shift) & 255) * intensity / 65536) << shift;
    return out;
}
inline bool hdIsCycling(unsigned char index) { return index >= 229 && index <= 254; }
inline uint32_t hdOver(uint32_t source, uint32_t dest, int opacity = 255)
{
    int a = ((source >> 24) * std::clamp(opacity, 0, 255) + 127) / 255;
    uint32_t out = 0xFF000000;
    for (int shift : { 0, 8, 16 }) out |= ((((source >> shift) & 255) * a + ((dest >> shift) & 255) * (255-a) + 127) / 255) << shift;
    return out;
}
// color.cc _buildBlendTable: row 0 is identity, 1..7 mixes the tint,
// 8..13 brightens the tint towards white. Keep RGB precision here.
inline uint32_t hdBlend(uint32_t dest, uint32_t tint, int row)
{
    row = std::clamp(row, 0, 13);
    if (row <= 7) return hdOver(tint | 0xFF000000, dest, row * 255 / 7);
    uint32_t out = 0xFF000000;
    for (int shift : { 0, 8, 16 }) {
        int c = (tint >> shift) & 255;
        out |= (c + (255-c) * (row-8) / 7) << shift;
    }
    return out;
}
} // namespace fallout
#endif
