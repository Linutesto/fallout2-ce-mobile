#ifndef FALLOUT_GRAPHICS_H_
#define FALLOUT_GRAPHICS_H_

#include <stdint.h>

namespace fallout {

constexpr int kColorPresetCount = 5;
const char* graphicsColorPresetName(int preset);
int graphicsColorPresetIndex(const char* name);
// ARGB8888; alpha is preserved. Matches grading.py's double precision FORMULA.
uint32_t graphicsGradeColor(uint32_t color, int preset);

// Packed ARGB8888, tightly packed rows, clamped image boundaries. The caller
// allocates width * height * factor^2 pixels (2 for MMPX, 3 for Scale3x).
void graphicsMmpx(const uint32_t* source, int width, int height, uint32_t* dest);
void graphicsScale3x(const uint32_t* source, int width, int height, uint32_t* dest);

} // namespace fallout

#endif /* FALLOUT_GRAPHICS_H_ */
