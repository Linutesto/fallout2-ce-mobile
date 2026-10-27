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
    if (intensity == 65536) return c;
    uint32_t out = c & 0xFF000000;
    for (int shift : { 0, 8, 16 })
        out |= (((c >> shift) & 255) * intensity / 65536) << shift;
    return out;
}
inline bool hdIsCycling(unsigned char index) { return index >= 229 && index <= 254; }
inline uint32_t hdCyclePixel(uint32_t hd, unsigned char index, const uint32_t* palette)
{
    return hdIsCycling(index) ? palette[index] : hd;
}
inline int hdInterpolateLight(const int* map, int pitch, int width, int height, int x2, int y2, const unsigned char* mask = nullptr)
{
    int x = x2 / 2, y = y2 / 2;
    int x1 = std::min(x + 1, width - 1), y1 = std::min(y + 1, height - 1);
    int fx = x2 & 1, fy = y2 & 1;
    int center = map[y * pitch + x];
    auto sample = [&](int sx, int sy) { return mask && mask[sy * width + sx] == 0 ? center : map[sy * pitch + sx]; };
    return (center * (2 - fx) * (2 - fy) + sample(x1, y) * fx * (2 - fy) + sample(x, y1) * (2 - fx) * fy + sample(x1, y1) * fx * fy) / 4;
}
inline uint32_t hdOver(uint32_t source, uint32_t dest, int opacity = 255)
{
    int a = ((source >> 24) * std::clamp(opacity, 0, 255) + 127) / 255;
    uint32_t out = 0xFF000000;
    for (int shift : { 0, 8, 16 })
        out |= ((((source >> shift) & 255) * a + ((dest >> shift) & 255) * (255 - a) + 127) / 255) << shift;
    return out;
}
inline int hdBlendRow(uint32_t color, bool glass)
{
    // CE: object.cc's gray tables start with Color2RGB's five-bit channels.
    int r = (color >> 19) & 31, g = (color >> 11) & 31, b = (color >> 3) & 31;
    return (glass ? (r + 5 * g + 4 * b) / 10 : (b + 3 * r + 6 * g) / 10) / 4;
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
        out |= (c + (255 - c) * (row - 8) / 7) << shift;
    }
    return out;
}
} // namespace fallout
#endif
