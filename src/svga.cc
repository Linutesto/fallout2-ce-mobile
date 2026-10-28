#include "svga.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cmath>

#include <SDL.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "color.h"
#include "config.h"
#include "dev_autotest.h"
#include "dinput.h"
#include "draw.h"
#include "game.h"
#include "graphics.h"
#include "hd_world.h"
#include "input.h"
#include "interface.h"
#include "memory.h"
#include "mouse.h"
#include "movie.h"
#include "mui.h"
#include "perf_monitor.h"
#include "scan_unimplemented.h"
#include "settings.h"
#include "text_font.h"
#include "tile.h"
#include "touch_controls.h"
#include "win32.h"
#include "window_manager_private.h"
#include "world_view.h"

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();
static void screenUpdatePalette(int first, int count);
static void screenConvertPaletteRange(int first, int count);
static void screenConvertRect(int x, int y, int width, int height);

// screen rect
Rect _scr_size;

// 0x6ACA18 scr_blit
void (*_scr_blit)(unsigned char* src, int src_pitch, int unused, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y) = _GNW95_ShowRect;

// 0x6ACA1C zero_mem
void (*_zero_mem)() = nullptr;

SDL_Window* gSdlWindow = nullptr;
SDL_Surface* gSdlSurface = nullptr;
SDL_Renderer* gSdlRenderer = nullptr;
SDL_Texture* gSdlTexture = nullptr;
SDL_Surface* gSdlTextureSurface = nullptr;

// CE: Layer tags of screen pixels, parallel to `gSdlSurface` (pitch is equal
// to surface width).
static unsigned char* gScreenLayers = nullptr;
static unsigned char gScreenBlitLayer = kScreenLayerUi;
static const unsigned char* gScreenBlitLayerSource = nullptr;

// Current palette in UI texture pixel format.
static Uint32 gScreenPalette[256];
static int gColorPreset = 0;

// CE: Part of the UI texture surface changed since it was uploaded last.
static bool gScreenDirty = false;
static SDL_Rect gScreenDirtyRect;

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

// 0x4CAD08 init_mode_320_200
int _init_mode_320_200()
{
    return _GNW95_init_mode_ex(320, 200, 8);
}

// 0x4CAD40 init_mode_320_400
int _init_mode_320_400()
{
    return _GNW95_init_mode_ex(320, 400, 8);
}

// 0x4CAD5C init_mode_640_480_16
int _init_mode_640_480_16()
{
    return -1;
}

// 0x4CAD64 init_mode_640_480
int _init_mode_640_480()
{
    return _init_vesa_mode(640, 480);
}

// 0x4CAD94 init_mode_640_400
int _init_mode_640_400()
{
    return _init_vesa_mode(640, 400);
}

// 0x4CADA8 init_mode_800_600
int _init_mode_800_600()
{
    return _init_vesa_mode(800, 600);
}

// 0x4CADBC init_mode_1024_768
int _init_mode_1024_768()
{
    return _init_vesa_mode(1024, 768);
}

// 0x4CADD0 init_mode_1280_1024
int _init_mode_1280_1024()
{
    return _init_vesa_mode(1280, 1024);
}

// 0x4CADF8
void _get_start_mode_()
{
}

// 0x4CADFC zero_vid_mem
void _zero_vid_mem()
{
    if (_zero_mem) {
        _zero_mem();
    }
}

// 0x4CAE1C GNW95_init_mode_ex
int _GNW95_init_mode_ex(int width, int height, int bpp)
{
    width = settings.screen.resolution_x;
    height = settings.screen.resolution_y;
    int scale = settings.screen.scale;

#ifdef __ANDROID__
    // CE: The phone's screen decides: the game's 480 lines (the original's
    // height, what the mobile UI is laid out against) and as many columns as
    // the screen's proportions give, full screen. A config copied from a
    // computer (a window of its size) doesn't matter.
    SDL_DisplayMode displayMode;
    if (SDL_GetDesktopDisplayMode(0, &displayMode) == 0 && displayMode.w > 0 && displayMode.h > 0) {
        int longSide = std::max(displayMode.w, displayMode.h);
        int shortSide = std::min(displayMode.w, displayMode.h);
        height = 480;
        width = std::max(640, 480 * longSide / shortSide);
        scale = 1;
        settings.screen.resolution_x = width;
        settings.screen.resolution_y = height;
        settings.screen.windowed = WindowMode::Fullscreen;
    }
#endif

    // Only allow scaling if resulting game resolution is >= 640x480
    if ((width / scale) < 640 || (height / scale) < 480) {
        scale = 1;
    } else {
        width /= scale;
        height /= scale;
    }

    if (_GNW95_init_window(width, height, settings.screen.windowed, scale) == -1) {
        return -1;
    }

    if (directDrawInit(width, height, bpp) == -1) {
        return -1;
    }

    // macOS seems to require dequeuing NSApp events in order for window to
    // become visible. There is no concrete number of calls required to make
    // it happen. Sadly there is no particular event to watch for because SDL
    // marks window as shown immediately after creation (see
    // `SDL_FinishWindowCreation`).
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
    }

    _scr_size.left = 0;
    _scr_size.top = 0;
    _scr_size.right = width - 1;
    _scr_size.bottom = height - 1;

    _mouse_blit_trans = nullptr;
    _scr_blit = _GNW95_ShowRect;
    _zero_mem = _GNW95_zero_vid_mem;
    _mouse_blit = _GNW95_ShowRect;

    return 0;
}

// 0x4CAECC init_vesa_mode
int _init_vesa_mode(int width, int height)
{
    return _GNW95_init_mode_ex(width, height, 8);
}

// 0x4CAEDC GNW95_init_window
int _GNW95_init_window(int width, int height, WindowMode mode, int scale)
{
    if (gSdlWindow == nullptr) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

        // CE: Naming a driver turns SDL's command batching off; the mobile UI
        // and the tiled world view make many small draws a frame.
        SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");

        Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;

        if (mode == WindowMode::Fullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN;
        } else if (mode == WindowMode::WindowedFullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }

        gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width * scale, height * scale, windowFlags);
        if (gSdlWindow == nullptr) {
            return -1;
        }

        if (!createRenderer(width, height)) {
            destroyRenderer();

            SDL_DestroyWindow(gSdlWindow);
            gSdlWindow = nullptr;

            return -1;
        }
    }

    return 0;
}

// 0x4CAF9C GNW95_init_DirectDraw
int directDrawInit(int width, int height, int bpp)
{
    if (gSdlSurface != nullptr) {
        unsigned char* palette = directDrawGetPalette();
        directDrawFree();

        if (directDrawInit(width, height, bpp) == -1) {
            return -1;
        }

        directDrawSetPalette(palette);

        return 0;
    }

    gSdlSurface = SDL_CreateRGBSurface(0, width, height, bpp, 0, 0, 0, 0);
    if (gSdlSurface == nullptr || gSdlSurface->format->palette == nullptr) {
        directDrawFree();
        return -1;
    }

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

    gScreenLayers = reinterpret_cast<unsigned char*>(SDL_calloc(static_cast<size_t>(width) * height, 1));
    if (gScreenLayers == nullptr) {
        directDrawFree();
        return -1;
    }

    gColorPreset = graphicsColorPresetIndex(settings.graphics.color_preset.c_str());
    screenUpdatePalette(0, 256);

    return 0;
}

// 0x4CB1B0 GNW95_reset_mode
void directDrawFree()
{
    if (gSdlSurface != nullptr) {
        SDL_FreeSurface(gSdlSurface);
        gSdlSurface = nullptr;
    }

    if (gScreenLayers != nullptr) {
        SDL_free(gScreenLayers);
        gScreenLayers = nullptr;
    }
}

// 0x4CB310 GNW95_SetPaletteEntries
void directDrawSetPaletteInRange(unsigned char* palette, int start, int count)
{
    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, start, count);
        screenUpdatePalette(start, count);
        screenConvertPaletteRange(start, count);
    }
}

// 0x4CB568 GNW95_SetPalette
void directDrawSetPalette(unsigned char* palette)
{
    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);
        gColorPreset = graphicsColorPresetIndex(settings.graphics.color_preset.c_str());
    screenUpdatePalette(0, 256);
        screenConvertRect(0, 0, gSdlSurface->w, gSdlSurface->h);
    }
}

// 0x4CB68C GNW95_GetPalette
unsigned char* directDrawGetPalette()
{
    // 0x6ACA24
    static unsigned char palette[768];

    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color* colors = gSdlSurface->format->palette->colors;

        for (int index = 0; index < 256; index++) {
            SDL_Color* color = &(colors[index]);
            palette[index * 3] = color->r >> 2;
            palette[index * 3 + 1] = color->g >> 2;
            palette[index * 3 + 2] = color->b >> 2;
        }
    }

    return palette;
}

// 0x4CB850 GNW95_ShowRect
void _GNW95_ShowRect(unsigned char* src, int srcPitch, int unused, int srcX, int srcY, int srcWidth, int srcHeight, int destX, int destY)
{
    (void)unused;

    blitBufferToBuffer(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX, gSdlSurface->pitch);

    unsigned char* layers = gScreenLayers + gSdlSurface->w * destY + destX;
    if (gScreenBlitLayerSource != nullptr) {
        blitBufferToBuffer(gScreenBlitLayerSource + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, layers, gSdlSurface->w);
    } else {
        bufferFill(layers, srcWidth, srcHeight, gSdlSurface->w, static_cast<Color>(gScreenBlitLayer));
    }

    screenConvertRect(destX, destY, srcWidth, srcHeight);
}

void screenLayersSetBlitLayer(unsigned char layer)
{
    gScreenBlitLayer = layer;
}

void screenLayersSetBlitSource(const unsigned char* layers)
{
    gScreenBlitLayerSource = layers;
}

// Entries which really changed with the last palette update (the game sets
// all cycling entries when any of them moves).
static bool gScreenPaletteChanged[256];

static double gShadowLift = 1.0;
static GraphicsColorTransform gWorldColorTransform;
static unsigned char gWorldDisplayChannels[3][256];
static unsigned char gWorldFastChannels[3][256];
static bool gWorldFastChannelsReady = false;
static unsigned char gShadowLiftTable[256];

static Uint32 screenLiftShadows(Uint32 color)
{
    if (gShadowLift == 1.0) {
        return color;
    }
    return (color & 0xFF000000)
        | (gShadowLiftTable[(color >> 16) & 0xFF] << 16)
        | (gShadowLiftTable[(color >> 8) & 0xFF] << 8)
        | gShadowLiftTable[color & 0xFF];
}

static void screenUpdatePalette(int first, int count)
{
    SDL_Color* colors = gSdlSurface->format->palette->colors;
    memset(gScreenPaletteChanged, 0, sizeof(gScreenPaletteChanged));
    for (int index = first; index < first + count; index++) {
        Uint32 color = screenLiftShadows(graphicsGradeColor(0xFF000000 | (colors[index].r << 16) | (colors[index].g << 8) | colors[index].b, gColorPreset));
        gScreenPaletteChanged[index] = gScreenPalette[index] != color;
        gScreenPalette[index] = color;
    }

    // CE: The HD surface keeps RGB precision through composition. A small
    // channel LUT applies gamma/fades; the shared grade keeps its exact RGB formula.
    if (hdWorldInstalled() && first == 0 && count == 256) {
        double sumBase[3] = { }, sumNow[3] = { };
        for (int i = 1; i < 229; i++)
            for (int c = 0; c < 3; c++) {
                sumBase[c] += _currentGammaTable[std::min<int>(_cmap[i * 3 + c], 63)] * 4;
                sumNow[c] += c == 0 ? colors[i].r : c == 1 ? colors[i].g
                                                           : colors[i].b;
            }
        for (int c = 0; c < 3; c++)
            for (int v = 0; v < 256; v++) {
                int lo = std::min(v / 4, 63), hi = std::min(lo + 1, 63);
                int gamma = _currentGammaTable[lo] * (4 - v % 4) + _currentGammaTable[hi] * (v % 4);
                double fade = sumBase[c] > 0 ? sumNow[c] / sumBase[c] : 1.0;
                gWorldDisplayChannels[c][v] = std::clamp<int>(std::lround(gamma * fade), 0, 255);
            }
        gWorldColorTransform.setPreset(gColorPreset);
        // Original and night_readable grade each RGB channel independently.
        // Fold gamma, grade and shadow lift once per palette update instead
        // of recomputing them for millions of HD pixels every present.
        gWorldFastChannelsReady = gColorPreset == 0 || gColorPreset == 4;
        if (gWorldFastChannelsReady) {
            for (int c = 0; c < 3; c++)
                for (int v = 0; v < 256; v++) {
                    int gamma = gWorldDisplayChannels[c][v];
                    Uint32 color = 0xFF000000 | (gamma * 0x010101);
                    if (gColorPreset != 0) color = gWorldColorTransform.apply(color);
                    color = screenLiftShadows(color);
                    gWorldFastChannels[c][v] = (color >> (16 - c * 8)) & 255;
                }
        }
    }
    worldViewInvalidatePalette(first, count);
}

Uint32 screenTransformWorldColor(Uint32 color, int preset)
{
    int r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
    if (preset < 0 && gWorldFastChannelsReady)
        return 0xFF000000 | (gWorldFastChannels[0][r] << 16)
            | (gWorldFastChannels[1][g] << 8) | gWorldFastChannels[2][b];
    if (preset >= 0) return screenLiftShadows(graphicsGradeColor(0xFF000000
            | (gWorldDisplayChannels[0][r] << 16) | (gWorldDisplayChannels[1][g] << 8) | gWorldDisplayChannels[2][b],
        preset));
    if (gColorPreset == 0) return screenLiftShadows(0xFF000000
        | (gWorldDisplayChannels[0][r] << 16) | (gWorldDisplayChannels[1][g] << 8) | gWorldDisplayChannels[2][b]);
    Uint32 gamma = 0xFF000000 | (gWorldDisplayChannels[0][r] << 16)
        | (gWorldDisplayChannels[1][g] << 8) | gWorldDisplayChannels[2][b];
    return screenLiftShadows(gWorldColorTransform.apply(gamma));
}

const Uint32* screenGetPalette()
{
    return gScreenPalette;
}

void screenSetColorPreset(int preset)
{
    gColorPreset = std::clamp(preset, 0, kColorPresetCount - 1);
    if (gSdlSurface != nullptr) {
        screenUpdatePalette(0, 256);
        screenConvertRect(0, 0, gSdlSurface->w, gSdlSurface->h);
    }
}

void screenSetShadowLift(double lift)
{
    lift = std::clamp(lift, 1.0, 3.0);
    if (lift == gShadowLift) {
        return;
    }
    gShadowLift = lift;
    for (int value = 0; value < 256; value++) {
        gShadowLiftTable[value] = static_cast<unsigned char>(std::lround(std::pow(value / 255.0, 1.0 / lift) * 255.0));
    }
    if (gSdlSurface != nullptr) {
        screenUpdatePalette(0, 256);
        screenConvertRect(0, 0, gSdlSurface->w, gSdlSurface->h);
    }
}

static void screenMarkDirty(int x, int y, int width, int height)
{
    SDL_Rect rect = { x, y, width, height };
    if (gScreenDirty) {
        SDL_UnionRect(&gScreenDirtyRect, &rect, &gScreenDirtyRect);
    } else {
        gScreenDirtyRect = rect;
        gScreenDirty = true;
    }
}

// Converts only pixels with palette entries [first, first + count) (palette
// cycling changes a few entries many times a second).
static void screenConvertPaletteRange(int first, int count)
{
    if (gSdlTextureSurface == nullptr) {
        return;
    }

    if (first == 0 && count >= 256) {
        screenConvertRect(0, 0, gSdlSurface->w, gSdlSurface->h);
        return;
    }

    int last = first + count - 1;
    bool anyChanged = false;
    for (int index = first; index <= last; index++) {
        anyChanged = anyChanged || gScreenPaletteChanged[index];
    }
    if (!anyChanged) {
        return;
    }

    int top = -1;
    int bottom = -1;
    int left = gSdlSurface->w;
    int right = -1;
    for (int row = 0; row < gSdlSurface->h; row++) {
        const unsigned char* src = reinterpret_cast<unsigned char*>(gSdlSurface->pixels) + gSdlSurface->pitch * row;
        const unsigned char* layers = gScreenLayers + gSdlSurface->w * row;
        Uint32* dest = reinterpret_cast<Uint32*>(reinterpret_cast<unsigned char*>(gSdlTextureSurface->pixels) + gSdlTextureSurface->pitch * row);
        for (int column = 0; column < gSdlSurface->w; column++) {
            unsigned char index = src[column];
            if (!gScreenPaletteChanged[index]) {
                continue;
            }

            Uint32 color = gScreenPalette[index];
            if (layers[column] == kScreenLayerWorld) {
                color &= 0x00FFFFFF;
            }
            dest[column] = color;

            if (top == -1) {
                top = row;
            }
            bottom = row;
            left = std::min(left, column);
            right = std::max(right, column);
        }
    }

    if (top != -1) {
        screenMarkDirty(left, top, right - left + 1, bottom - top + 1);
    }
}

// Converts palette indices to UI texture pixels. Pixels belonging to the world
// layer are fully transparent so the world texture rendered below shows
// through.
static void screenConvertRect(int x, int y, int width, int height)
{
    if (gSdlTextureSurface == nullptr) {
        return;
    }

    for (int row = 0; row < height; row++) {
        const unsigned char* src = reinterpret_cast<unsigned char*>(gSdlSurface->pixels) + gSdlSurface->pitch * (y + row) + x;
        const unsigned char* layers = gScreenLayers + gSdlSurface->w * (y + row) + x;
        Uint32* dest = reinterpret_cast<Uint32*>(reinterpret_cast<unsigned char*>(gSdlTextureSurface->pixels) + gSdlTextureSurface->pitch * (y + row)) + x;
        for (int column = 0; column < width; column++) {
            Uint32 color = gScreenPalette[src[column]];
            if (layers[column] == kScreenLayerWorld) {
                color &= 0x00FFFFFF;
            }
            dest[column] = color;
        }
    }

    screenMarkDirty(x, y, width, height);
}

// Clears drawing surface.
//
// 0x4CBBC8 GNW95_zero_vid_mem
void _GNW95_zero_vid_mem()
{
    if (!gProgramIsActive) {
        return;
    }

    unsigned char* surface = (unsigned char*)gSdlSurface->pixels;
    for (int y = 0; y < gSdlSurface->h; y++) {
        memset(surface, 0, gSdlSurface->w);
        surface += gSdlSurface->pitch;
    }

    memset(gScreenLayers, kScreenLayerUi, static_cast<size_t>(gSdlSurface->w) * gSdlSurface->h);
    screenConvertRect(0, 0, gSdlSurface->w, gSdlSurface->h);
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&_scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&_scr_size);
}

int screenGetVisibleHeight()
{
    int windowBottomMargin = 0;

    // CE: Touch HUD floats over the map, there is no bar at the bottom.
    if (!settings.ui.iface_bar_mode && !settings.touch.hud) {
        windowBottomMargin = INTERFACE_BAR_HEIGHT;
    }
    return screenGetHeight() - windowBottomMargin;
}

// returns true if the game is running in fullscreen mode, false otherwise (including windowed fullscreen mode)
bool screenIsExclusiveFullscreen()
{
    Uint32 flags = SDL_GetWindowFlags(gSdlWindow);
    return (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN;
}

// Presentation-only subframe: pump touches and draw, with no inputGetInput,
// tickersExecute, per-frame script hooks or devAutotestTick. Gestures may
// post the same user commands as on a regular input frame.
static void screenPresentSubframe()
{
    if (!gProgramIsActive || gSdlRenderer == nullptr || muiHoldsFrame()) return;
    _GNW95_process_message();
    if (!gProgramIsActive) return;
    touchControlsProcessGestures();
    renderFpsCounter();
    renderPresent();
}

static void screenConfigureRefresh()
{
    unsigned int rate = 60;
#ifdef __ANDROID__
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env != nullptr && activity != nullptr) {
        jclass cls = env->GetObjectClass(activity);
        jmethodID method = env->GetMethodID(cls, "getRefreshRate", "()F");
        if (method != nullptr) rate = static_cast<unsigned int>(env->CallFloatMethod(activity, method) + 0.5f);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
    }
#else
    SDL_DisplayMode mode;
    if (gSdlWindow != nullptr && SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(gSdlWindow), &mode) == 0 && mode.refresh_rate > 0) {
        rate = mode.refresh_rate;
    }
#endif
    rate = std::clamp(rate, 60u, 240u);
    sharedFpsLimiter.setPresentation(settings.graphics.high_refresh ? rate : 60, screenPresentSubframe);
}

void screenSetHighRefresh(bool enabled)
{
    settings.graphics.high_refresh = enabled;
#ifdef __ANDROID__
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env != nullptr && activity != nullptr) {
        jclass cls = env->GetObjectClass(activity);
        jmethodID method = env->GetMethodID(cls, "setHighRefresh", "(Z)V");
        if (method != nullptr) env->CallVoidMethod(activity, method, static_cast<jboolean>(enabled));
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
    }
#endif
    if (gSdlRenderer != nullptr) {
        // Keep the original swap interval 0. A vsync wait in a supplemental
        // frame can consume the next logic tick on 90 Hz / throttled displays.
        // The limiter paces presentation inside the original logic budget;
        // Android's compositor presents the submitted buffers at display rate.
        SDL_RenderSetVSync(gSdlRenderer, 0);
    }
    screenConfigureRefresh();
}

static bool createRenderer(int width, int height)
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == nullptr) {
        return false;
    }

    if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
        return false;
    }

    screenSetHighRefresh(settings.graphics.high_refresh);

    // CE: UI texture has alpha channel to let world layer show through.
    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture == nullptr) {
        return false;
    }

    SDL_SetTextureBlendMode(gSdlTexture, SDL_BLENDMODE_BLEND);
    // New texture has undefined content.
    screenMarkDirty(0, 0, width, height);

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == nullptr) {
        return false;
    }

    return true;
}

static void destroyRenderer()
{
    worldViewResetRenderer();
    muiResetRenderer();

    if (gSdlTextureSurface != nullptr) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = nullptr;
    }

    if (gSdlTexture != nullptr) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = nullptr;
    }

    if (gSdlRenderer != nullptr) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = nullptr;
    }
}

void handleWindowSizeChanged()
{
    movieHandleRendererReset();
    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());
    if (gSdlSurface != nullptr) {
        screenConvertRect(0, 0, gSdlSurface->w, gSdlSurface->h);
    }
    mouseDeviceRefreshWindowMapping();
}

static unsigned int gPresentedFrames = 0;

void renderFpsCounter()
{
    if (!settings.debug.show_fps || gSdlSurface == nullptr || gSdlTextureSurface == nullptr) {
        return;
    }

    static unsigned int sampleStartTicks = 0;
    static int sampleFrames = 0;
    static double fps = 0.0;

    unsigned int now = SDL_GetTicks();
    if (sampleStartTicks == 0) {
        sampleStartTicks = now;
    }

    sampleFrames = static_cast<int>(gPresentedFrames);

    unsigned int elapsed = now - sampleStartTicks;
    if (elapsed >= 500) {
        fps = sampleFrames * 1000.0 / elapsed;
        gPresentedFrames = 0;
        sampleFrames = 0;
        sampleStartTicks = now;
    }

    char text[32];
    snprintf(text, sizeof(text), "FPS: %.1f", fps);

    ScopedFont font(101);

    constexpr int kPadding = 2;
    int textWidth = fontGetStringWidth(text);
    int textHeight = fontGetLineHeight();
    int width = textWidth + kPadding * 2;
    int height = textHeight + kPadding * 2;

    if (width > gSdlSurface->w) {
        width = gSdlSurface->w;
    }

    if (height > gSdlSurface->h) {
        height = gSdlSurface->h;
    }

    bufferFill(static_cast<unsigned char*>(gSdlSurface->pixels), width, height, gSdlSurface->pitch, COLOR_BLACK);
    if (width > kPadding * 2 && height > kPadding * 2) {
        fontDrawText(static_cast<unsigned char*>(gSdlSurface->pixels) + gSdlSurface->pitch * kPadding + kPadding, text, width - kPadding * 2, gSdlSurface->pitch, COLOR_LIGHT_GREY);
    }

    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = width;
    rect.h = height;
    screenConvertRect(rect.x, rect.y, rect.w, rect.h);
}

void renderPresent()
{
    // CE: Mobile UI screen switch in progress, the last frame stays.
    if (muiHoldsFrame()) {
        return;
    }

    // Follow display changes / LTPO policy without crossing JNI each frame.
    static Uint32 refreshCheck = 0;
    if (SDL_GetTicks() - refreshCheck >= 1000) {
        refreshCheck = SDL_GetTicks();
        screenConfigureRefresh();
    }
    PerfFrame perf;
    perfMonitorMark(&perf.start);
    SDL_RenderClear(gSdlRenderer);
    // CE: Under a mobile UI screen covering everything the map and the
    // game's screen would be drawn only to be painted over.
    if (!muiCoversScreen()) {
        // Only what changed since the last upload.
        if (gScreenDirty) {
            Uint64 start;
            perfMonitorMark(&start);
            const unsigned char* pixels = reinterpret_cast<const unsigned char*>(gSdlTextureSurface->pixels)
                + gScreenDirtyRect.y * gSdlTextureSurface->pitch
                + gScreenDirtyRect.x * gSdlTextureSurface->format->BytesPerPixel;
            SDL_UpdateTexture(gSdlTexture, &gScreenDirtyRect, pixels, gSdlTextureSurface->pitch);
            perfMonitorCount(PerfCount::ScreenPixels, static_cast<long long>(gScreenDirtyRect.w) * gScreenDirtyRect.h);
            perfMonitorSpan(PerfSpan::ScreenUpload, start);
            gScreenDirty = false;
        }
        worldViewRender(gSdlRenderer);
        Uint64 start;
        perfMonitorMark(&start);
        SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
        perfMonitorSpan(PerfSpan::ScreenDraw, start);
    }
    // render movie SDL texture if present
    movieRenderDirectOverlay();
    perfMonitorMark(&perf.uiStart);
    muiRender(gSdlRenderer);
    perfMonitorMark(&perf.uiEnd);
    devAutotestPump();
    devAutotestCapture(gSdlRenderer);
    SDL_RenderPresent(gSdlRenderer);
    gPresentedFrames++;
    perfMonitorMark(&perf.end);
    perfMonitorFrame(perf);
}

} // namespace fallout
