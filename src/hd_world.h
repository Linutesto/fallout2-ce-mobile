#ifndef FALLOUT_HD_WORLD_H_
#define FALLOUT_HD_WORLD_H_
#include "geometry.h"
#include <SDL.h>
#include <cstddef>
#include <cstdint>
namespace fallout {
struct Art;
class FrmId;
struct HdDrawOptions {
    const int* lightMap = nullptr;
    int lightPitch = 0;
    const unsigned char* mask = nullptr;
    int maskPitch = 0;
    int maskX = 0;
    int maskY = 0;
    int tint = -1;
    bool glass = false;
    int opacity = 255;
};
void hdWorldInit(int width, int height);
void hdWorldExit();
bool hdWorldInstalled();
bool hdWorldActive();
void hdWorldSettingsChanged();
void hdWorldSetFilter(int filter);
void hdWorldClear(const Rect& rect);
void hdWorldScrolled(int dx, int dy);
void hdWorldPaletteChanged(int first, int count);
void hdWorldGrayscale();
void hdWorldDraw(const FrmId& id, Art* art, int frame, int rotation,
    int x, int y, const Rect& clip, int light, const HdDrawOptions& options = { });
void hdWorldOutline(const FrmId& id, Art* art, int frame, int rotation,
    int x, int y, const Rect& clip, int color, int bandHeight, int bandCount,
    int tint = -1);
// Preserve text/stencils that are still drawn by the indexed renderer.
void hdWorldCopyIndexedChanges(const Rect& rect, const unsigned char* before);
bool hdWorldPresent(SDL_Renderer* renderer, const SDL_Rect& source, const SDL_FRect& dest);
bool hdWorldVerifyTextureColors(SDL_Renderer* renderer);
// CPU times for the most recent non-cached HD present (milliseconds).
void hdWorldLastPresentTiming(double* convertMs, double* uploadMs);
void hdWorldPresentPathStats(bool* grayscale, size_t* cyclingPixels, int* threads);
void hdWorldResetRenderer();
const uint32_t* hdWorldPixels();
uint32_t hdWorldColorAt(int x2, int y2);
void hdWorldResetCounts();
void hdWorldGetCounts(uint64_t* hd, uint64_t* fallback);
// Samples one complete tileWindowRefresh during the HD autotest benchmark.
void hdWorldBeginDrawTiming();
void hdWorldEndDrawTiming(double* lookupMs, double* fallbackMs, double* buildMs, double* pixelsMs, uint64_t* calls);
} // namespace fallout
#endif
