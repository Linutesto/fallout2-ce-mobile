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
// Returns true only for a complete refresh whose rendered scene signature is
// identical to the last command list. The caller may skip scene traversal.
bool hdWorldTryReuseCommands(const Rect& rect, int elevation, uint64_t sceneSignature);
uint64_t hdWorldSceneReuseCount();
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
bool hdWorldVerifyTextureExact(SDL_Renderer* renderer, size_t* changedPixels,
    size_t* outOfTolerance = nullptr);
// GPU composition counters: replayed presents, GPU pixels, presents that had
// to compose on the CPU, cached world textures.
// CPU times for the most recent non-cached HD present (milliseconds).
void hdWorldLastPresentTiming(double* convertMs, double* uploadMs);
// GPU command replay time of the last present (milliseconds).
void hdWorldLastGpuTiming(double* replayMs);
void hdWorldLastUpdateStats(bool* full, int* pixels);
void hdWorldPresentPathStats(bool* grayscale, size_t* cyclingPixels, int* threads);
void hdWorldResetRenderer();
const uint32_t* hdWorldPixels();
uint32_t hdWorldColorAt(int x2, int y2);
void hdWorldResetCounts();
void hdWorldGetCounts(uint64_t* hd, uint64_t* fallback);
// Samples one complete tileWindowRefresh during the HD autotest benchmark.
void hdWorldBeginDrawTiming();
void hdWorldEndDrawTiming(double* lookupMs, double* fallbackMs, double* buildMs, double* pixelsMs, uint64_t* calls);
void hdWorldLastDrawKinds(uint64_t* lightMap, uint64_t* mask, uint64_t* tint, uint64_t* opacity,
    uint64_t* fallback, uint64_t* outline, uint64_t* indexed);
void hdWorldSetGpuFastBenchmark(bool enabled);
// Dev QA: drop the "this image is already on screen" state, so the next
// present replays its commands for real.
void hdWorldInvalidateReplay();
void hdWorldGpuStatsReset();
void hdWorldEnsureCpuReference();
void hdWorldGpuStats(uint64_t* replayFrames, uint64_t* replayPixels, uint64_t* replayCommands,
    uint64_t* cpuFillFrames, uint64_t* missFrames, uint64_t* textures, uint64_t* textureBytes);
} // namespace fallout
#endif
