// graphics.{cc,h}: CODEX-TASK-GRAPHICS items 1/2. Oracle from the supplied
// grading.py / upscale.py, regenerated with tools/graphics-reference.py.
#include "graphics.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace fallout;

static uint32_t read(std::istream& stream)
{
    unsigned char bytes[4];
    stream.read(reinterpret_cast<char*>(bytes), 4);
    assert(stream.good());
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

int main()
{
    std::ifstream stream(GRAPHICS_REFERENCE, std::ios::binary);
    assert(stream.good());
    assert(read(stream) == 0x47584631);
    int colors = read(stream);
    int images = read(stream);
    for (int i = 0; i < colors; i++) {
        uint32_t rgb = read(stream);
        for (int preset = 0; preset < kColorPresetCount; preset++) {
            uint32_t expected = read(stream);
            uint32_t actual = graphicsGradeColor(rgb, preset);
            if (actual != expected) {
                fprintf(stderr, "grade %d %08x: %08x != %08x\n", preset, rgb, actual, expected);
                return 1;
            }
            assert(graphicsGradeColor(rgb & 0xFFFFFF, preset) == (expected & 0xFFFFFF));
        }
    }
    assert(graphicsColorPresetIndex(nullptr) == 0);
    assert(graphicsColorPresetIndex("bogus") == 0);
    for (int i = 0; i < kColorPresetCount; i++) assert(graphicsColorPresetIndex(graphicsColorPresetName(i)) == i);
    size_t pixels = 0;
    for (int image = 0; image < images; image++) {
        int w = read(stream), h = read(stream);
        std::vector<uint32_t> source(w * h);
        for (uint32_t& value : source) value = read(stream);
        for (int factor : { 2, 3 }) {
            // Sentinels catch an output overrun, including width/height 1.
            std::vector<uint32_t> dest(w * h * factor * factor + 2, 0xDEADBEEF);
            if (factor == 2) graphicsMmpx(source.data(), w, h, dest.data() + 1);
            else graphicsScale3x(source.data(), w, h, dest.data() + 1);
            assert(dest.front() == 0xDEADBEEF && dest.back() == 0xDEADBEEF);
            for (size_t p = 1; p + 1 < dest.size(); p++) {
                uint32_t expected = read(stream);
                if (dest[p] != expected) {
                    fprintf(stderr, "image %d factor %d pixel %zu: %08x != %08x\n", image, factor, p-1, dest[p], expected);
                    return 1;
                }
                pixels++;
            }
        }
    }
    assert(stream.peek() == std::char_traits<char>::eof());
    for (int preset = 0; preset < kColorPresetCount; preset++) {
        GraphicsColorTransform transform;
        transform.setPreset(preset);
        for (unsigned i = 0; i < 8192; i++) {
            uint32_t color = (i * 2654435761u);
            assert(transform.apply(color) == graphicsGradeColor(color, preset));
        }
    }
    printf("40960 exact HD grade/alpha comparisons: PASS\n");
    printf("%d colour comparisons (+ alpha checks), %d images x 2 filters, %zu output pixels: PASS\n", colors * kColorPresetCount, images, pixels);
}
