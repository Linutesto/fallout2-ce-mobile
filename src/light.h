#ifndef LIGHT_H
#define LIGHT_H
#include <cstdint>

namespace fallout {

#define LIGHT_INTENSITY_MIN (65536 / 4)
#define LIGHT_INTENSITY_MAX 65536

typedef void AdjustLightIntensityProc(int elevation, int tile, int intensity);

int lightInit();
void lightReset();
void lightExit();
int lightGetAmbientIntensity();
// Ambient light the map is drawn with: the real one raised by the dark boost
// setting.
int lightGetDrawnAmbientIntensity();
uint64_t lightSceneRevision();
// The dark boost setting changed: redraws the map with it.
void lightApplyDarkBoost();
void lightSetAmbientIntensity(int intensity, bool shouldUpdateScreen);
int lightGetTileIntensity(int elevation, int tile);
int lightGetTrueTileIntensity(int elevation, int tile);
void lightSetTileIntensity(int elevation, int tile, int intensity);
void lightIncreaseTileIntensity(int elevation, int tile, int intensity);
void lightDecreaseTileIntensity(int elevation, int tile, int intensity);
void lightResetTileIntensity();
void lightDecreaseAmbient(int val);
void lightIncreaseAmbient(int val);

} // namespace fallout

#endif /* LIGHT_H */
