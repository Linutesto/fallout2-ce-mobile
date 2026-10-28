#include "hd_world.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "art.h"
#include "color.h"
#include "db.h"
#include "debug.h"
#include "dev_autotest.h"
#include "graphics.h"
#include "hd_art.h"
#include "hd_pixels.h"
#include "object.h"
#include "settings.h"
#include "svga.h"
#include "tile.h"
#include "world_view.h"

namespace fallout {
namespace {
    int gWidth = 0;
    int gHeight = 0;
    int gFallbackFilter = 2;
    bool gInstalled = false;
    bool gGrayscale = false;
    // CE: Most pixels contain at most one cycling source. Rare translucent
    // overlaps use a sparse affine expression, so every contributing fire/water
    // index remains live without replaying map traversal during presentation.
    std::vector<unsigned char> gCycleIndex;
    std::vector<uint16_t> gCycleWeight;
    std::unordered_map<size_t, std::array<uint16_t, 26>> gCycleMix;
    uint32_t gCycleColors[256] = { };
    std::unordered_set<std::string> gCountedDraws;
    std::vector<uint32_t> gPixels;
    std::unique_ptr<HdFrameCache> gCache;
    SDL_Texture* gTexture = nullptr;
    uint64_t gRevision = 1;
    // CE: The part of the 2x buffer changed since the last present, so a
    // walking critter re-converts its own rectangle instead of the screen.
    bool gDirtyAll = true;
    int gDirtyLeft = 0, gDirtyTop = 0, gDirtyRight = 0, gDirtyBottom = 0;
    uint64_t gPresentedRevision = 0;
    SDL_Rect gPresentedSource = { };
    uint64_t gHdCount = 0;
    uint64_t gFallbackCount = 0;

    void touch(int left, int top, int right, int bottom)
    {
        if (left >= right || top >= bottom) return;
        if (gDirtyLeft >= gDirtyRight) {
            gDirtyLeft = left, gDirtyTop = top, gDirtyRight = right, gDirtyBottom = bottom;
            return;
        }
        gDirtyLeft = std::min(gDirtyLeft, left);
        gDirtyTop = std::min(gDirtyTop, top);
        gDirtyRight = std::max(gDirtyRight, right);
        gDirtyBottom = std::max(gDirtyBottom, bottom);
    }

    uint32_t paletteColor(int index, bool cycling = false)
    {
        const unsigned char* palette = cycling && hdIsCycling(index) ? _systemCmap : _cmap;
        return 0xFF000000 | (palette[index * 3] * 4 << 16) | (palette[index * 3 + 1] * 4 << 8) | (palette[index * 3 + 2] * 4);
    }

    void writePixel(size_t offset, uint32_t color, int opacity = 255, int cycle = 0, int destinationWeight = 0)
    {
        int alpha = ((color >> 24) * opacity + 127) / 255;
        if (alpha == 0) return;
        if (alpha == 255 && destinationWeight == 0) {
            gPixels[offset] = color | 0xFF000000;
            gCycleIndex[offset] = cycle;
            gCycleWeight[offset] = cycle ? 65535 : 0;
            if (!gCycleMix.empty()) gCycleMix.erase(offset);
            return;
        }
        if (cycle == 0 && gCycleIndex[offset] == 0 && gCycleMix.empty()) {
            gPixels[offset] = hdOver(color, gPixels[offset], opacity);
            return;
        }
        int retain = 255 - alpha + alpha * destinationWeight / 255;
        auto multi = gCycleMix.find(offset);
        if (multi != gCycleMix.end()) {
            for (auto& weight : multi->second)
                weight = static_cast<uint32_t>(weight) * retain / 255;
            if (cycle) multi->second[cycle - 229] = std::min<int>(65535, multi->second[cycle - 229] + alpha * 257);
            if (retain == 0 && !cycle) gCycleMix.erase(multi);
        } else {
            int old = gCycleIndex[offset];
            int weight = static_cast<uint32_t>(gCycleWeight[offset]) * retain / 255;
            if (cycle && old && old != cycle && weight) {
                std::array<uint16_t, 26> mix = { };
                mix[old - 229] = weight;
                mix[cycle - 229] = alpha * 257;
                gCycleMix.emplace(offset, mix);
                gCycleIndex[offset] = 0;
                gCycleWeight[offset] = 0;
            } else {
                if (cycle) {
                    old = cycle;
                    weight = std::min(65535, weight + alpha * 257);
                }
                gCycleIndex[offset] = weight ? old : 0;
                gCycleWeight[offset] = weight;
            }
        }
        gPixels[offset] = hdOver(color, gPixels[offset], opacity);
    }

    uint32_t livePixel(size_t offset)
    {
        uint32_t color = gPixels[offset];
        if (gCycleIndex[offset] == 0 && gCycleMix.empty() && !gGrayscale) return color;
        auto multi = gCycleMix.find(offset);
        int index = gCycleIndex[offset];
        int channels[] = { int((color >> 16) & 255), int((color >> 8) & 255), int(color & 255) };
        auto apply = [&](int cycle, int weight) {
            uint32_t base = paletteColor(cycle);
            uint32_t live = gCycleColors[cycle];
            for (int c = 0; c < 3; c++) {
                int shift = 16 - c * 8;
                channels[c] += ((int((live >> shift) & 255) - int((base >> shift) & 255)) * weight) / 65535;
            }
        };
        if (multi != gCycleMix.end()) {
            for (int i = 0; i < 26; i++)
                if (multi->second[i]) apply(i + 229, multi->second[i]);
        } else if (index)
            apply(index, gCycleWeight[offset]);
        color = 0xFF000000;
        for (int c = 0; c < 3; c++)
            color |= std::clamp(channels[c], 0, 255) << (16 - c * 8);
        if (gGrayscale) {
            int gray = (std::clamp(channels[0], 0, 255) * 30 + std::clamp(channels[1], 0, 255) * 59 + std::clamp(channels[2], 0, 255) * 11) / 100;
            color = 0xFF000000 | gray * 0x010101;
        }
        return color;
    }

    void allocatePixels()
    {
        size_t size = static_cast<size_t>(gWidth) * gHeight * 4;
        if (gPixels.size() == size) return;
        gPixels.assign(size, 0xFF000000);
        gCycleIndex.assign(size, 0);
        gCycleWeight.assign(size, 0);
        gCycleMix.clear();
        gDirtyAll = true;
    }

    bool readPng(const std::string& path, std::vector<unsigned char>& data)
    {
        // CE: DAT entries use Windows separators. The public HD format
        // stays portable; compat_fopen also resolves this spelling for loose files.
        std::string vfsPath = path;
        std::replace(vfsPath.begin(), vfsPath.end(), '/', '\\');
        File* file = fileOpen(vfsPath.c_str(), "rb");
        static int probes = 0;
        if (devAutotestIsEnabled() && probes++ < 4) debugPrint("HD VFS probe %s found=%d\n", vfsPath.c_str(), file != nullptr);
        if (file == nullptr) return false;
        int size = fileGetSize(file);
        bool ok = size > 0 && size <= 64 * 1024 * 1024;
        if (ok) {
            data.resize(size);
            ok = fileRead(data.data(), 1, size, file) == static_cast<size_t>(size);
        }
        fileClose(file);
        // CE: An existing but broken loose PNG wins over the archive too.
        if (!ok) data.clear();
        return true;
    }

    std::shared_ptr<const HdFrame> frameFor(const FrmId& id, int frame, int rotation, int width, int height)
    {
        static int probes = 0;
        if (devAutotestIsEnabled() && probes++ < 4) debugPrint("HD frame probe input=%s key=%s direction=%d frame=%d size=%dx%d\n", id.filePath(), hdFramePath(id.filePath(), rotation, frame).c_str(), rotation, frame, width, height);
        return gCache->get(id.filePath(), rotation, frame, width, height);
    }

    // CE: Only fallback sprites pass through the user's pixel-art filter. HD
    // pixels are never magnified by MMPX/Scale3x or smeared by that setting.
    std::vector<uint32_t> fallbackPixels(const unsigned char* src, int w, int h, int light, const HdDrawOptions& options)
    {
        std::vector<uint32_t> base(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < base.size(); i++) {
            unsigned char index = src[i];
            if (index && options.tint < 0) {
                int intensity = options.lightMap ? options.lightMap[(i / w) * options.lightPitch + i % w] : light;
                index = hdLightIndex(index, intensity, reinterpret_cast<const unsigned char*>(intensityColorTable));
            }
            base[i] = src[i] == 0 ? 0 : paletteColor(index, false);
        }
        std::vector<uint32_t> out(static_cast<size_t>(w) * h * 4);
        int filter = gFallbackFilter;
        if (filter == 3) {
            graphicsMmpx(base.data(), w, h, out.data());
        } else if (filter == 4) {
            std::vector<uint32_t> three(static_cast<size_t>(w) * h * 9);
            graphicsScale3x(base.data(), w, h, three.data());
            for (int y = 0; y < h * 2; y++)
                for (int x = 0; x < w * 2; x++)
                    out[y * w * 2 + x] = three[(y * 3 / 2) * w * 3 + x * 3 / 2];
        } else {
            for (int y = 0; y < h * 2; y++)
                for (int x = 0; x < w * 2; x++) {
                    if (filter != 1) {
                        out[y * w * 2 + x] = base[(y / 2) * w + x / 2];
                        continue;
                    }
                    // Premultiplied bilinear at the centers of the 2x pixels, so
                    // transparent black cannot bleed into a lit sprite's edge.
                    int x0 = std::clamp((x - 1) / 2, 0, w - 1), y0 = std::clamp((y - 1) / 2, 0, h - 1);
                    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
                    int fx = x == 0 ? 0 : (x & 1) ? 1
                                                  : 3;
                    int fy = y == 0 ? 0 : (y & 1) ? 1
                                                  : 3;
                    uint32_t samples[] = { base[y0 * w + x0], base[y0 * w + x1], base[y1 * w + x0], base[y1 * w + x1] };
                    int weights[] = { (4 - fx) * (4 - fy), fx * (4 - fy), (4 - fx) * fy, fx * fy };
                    int a = 0, channels[3] = { };
                    for (int i = 0; i < 4; i++) {
                        int coverage = (samples[i] >> 24) * weights[i];
                        a += coverage;
                        for (int c = 0; c < 3; c++)
                            channels[c] += ((samples[i] >> (c * 8)) & 255) * coverage;
                    }
                    uint32_t c = (a / 16) << 24;
                    if (a)
                        for (int i = 0; i < 3; i++)
                            c |= (channels[i] / a) << (i * 8);
                    out[y * w * 2 + x] = c;
                }
        }
        return out;
    }
} // namespace

void hdWorldInit(int width, int height)
{
    hdWorldExit();
    gWidth = width;
    gHeight = height;
    gFallbackFilter = settings.world_view.filter;
    for (const XBase* base = xbaseGetFirst(); base; base = base->next) {
        std::error_code error;
        std::filesystem::path path(base->path);
        if (devAutotestIsEnabled()) debugPrint("HD VFS base %s archive=%d\n", base->path, base->isDbase);
        if (!base->isDbase) {
            std::filesystem::recursive_directory_iterator it(path / "hd", std::filesystem::directory_options::skip_permission_denied, error), end;
            while (!error && it != end && !gInstalled) {
                std::string extension = it->path().extension().string();
                for (char& c : extension)
                    c = std::tolower(static_cast<unsigned char>(c));
                if (extension == ".png" && it->is_regular_file(error)) gInstalled = true;
                it.increment(error);
            }
        } else if (path.filename() == "hd.dat" && base->dbase) {
            for (int i = 0; i < base->dbase->entriesLength && !gInstalled; i++) {
                std::string name = base->dbase->entries[i].path;
                for (char& c : name)
                    c = c == '\\' ? '/' : std::tolower(static_cast<unsigned char>(c));
                if (name.compare(0, 3, "hd/") == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".png") gInstalled = true;
            }
        }
    }
    size_t budget = static_cast<size_t>(std::clamp(settings.graphics.hd_cache_mb, 0, 4096)) * 1024 * 1024;
    gCache = std::make_unique<HdFrameCache>(budget, readPng,
        [](const std::string& message) { debugPrint("HD art: %s\n", message.c_str()); });
    for (int i = 229; i <= 254; i++)
        gCycleColors[i] = paletteColor(i);
    if (gInstalled) debugPrint("HD art installed; cache budget %zu bytes\n", budget);
}

void hdWorldExit()
{
    hdWorldResetRenderer();
    gPixels.clear();
    gPixels.shrink_to_fit();
    gCycleIndex.clear();
    gCycleIndex.shrink_to_fit();
    gCycleWeight.clear();
    gCycleWeight.shrink_to_fit();
    gCycleMix.clear();
    gCountedDraws.clear();
    gCache.reset();
    gInstalled = false;
    gWidth = gHeight = 0;
}

bool hdWorldInstalled() { return gInstalled; }
bool hdWorldActive() { return gInstalled && settings.graphics.hd_art && worldViewIsEnabled(); }
const uint32_t* hdWorldPixels() { return gPixels.empty() ? nullptr : gPixels.data(); }
uint32_t hdWorldColorAt(int x2, int y2) { return livePixel(static_cast<size_t>(y2) * gWidth * 2 + x2); }
void hdWorldResetCounts() { gHdCount = gFallbackCount = 0; }
void hdWorldGetCounts(uint64_t* hd, uint64_t* fallback)
{
    *hd = gHdCount;
    *fallback = gFallbackCount;
}

void hdWorldSetFilter(int filter)
{
    bool changed = gFallbackFilter != filter;
    gFallbackFilter = filter;
    if (changed && hdWorldActive()) tileWindowRefresh();
}

void hdWorldSettingsChanged()
{
    if (hdWorldActive())
        allocatePixels();
    else {
        gPixels.clear();
        gPixels.shrink_to_fit();
        gCycleIndex.clear();
        gCycleIndex.shrink_to_fit();
        gCycleWeight.clear();
        gCycleWeight.shrink_to_fit();
        gCycleMix.clear();
    }
    worldViewForgetRendered();
    tileWindowRefresh();
}

void hdWorldClear(const Rect& rect)
{
    if (!hdWorldActive()) return;
    ++gRevision;
    allocatePixels();
    gGrayscale = false;
    gCountedDraws.clear();
    int left = std::max(rect.left, 0) * 2, right = std::min(rect.right + 1, gWidth) * 2;
    touch(left, std::max(rect.top, 0) * 2, right, std::min(rect.bottom + 1, gHeight) * 2);
    for (int y = std::max(rect.top, 0) * 2; y < std::min(rect.bottom + 1, gHeight) * 2; y++) {
        size_t offset = static_cast<size_t>(y) * gWidth * 2;
        std::fill(gPixels.begin() + offset + left, gPixels.begin() + offset + right, 0xFF000000);
        std::fill(gCycleIndex.begin() + offset + left, gCycleIndex.begin() + offset + right, 0);
        std::fill(gCycleWeight.begin() + offset + left, gCycleWeight.begin() + offset + right, 0);
    }
    for (auto it = gCycleMix.begin(); it != gCycleMix.end();) {
        int x = it->first % (gWidth * 2), y = it->first / (gWidth * 2);
        if (x >= left && x < right && y >= rect.top * 2 && y < (rect.bottom + 1) * 2)
            it = gCycleMix.erase(it);
        else
            ++it;
    }
}

void hdWorldScrolled(int dx, int dy)
{
    if (!hdWorldActive() || gPixels.empty()) return;
    ++gRevision;
    gDirtyAll = true;
    int w = gWidth * 2, h = gHeight * 2;
    dx *= 2;
    dy *= 2;
    int left = std::max(-dx, 0), right = std::min(w - dx, w);
    int top = std::max(-dy, 0), bottom = std::min(h - dy, h);
    if (left >= right || top >= bottom) return;
    int y = dy >= 0 ? top : bottom - 1, end = dy >= 0 ? bottom : top - 1, step = dy >= 0 ? 1 : -1;
    for (; y != end; y += step) {
        size_t dst = static_cast<size_t>(y) * w + left, src = static_cast<size_t>(y + dy) * w + left + dx;
        memmove(gPixels.data() + dst, gPixels.data() + src, (right - left) * sizeof(uint32_t));
        memmove(gCycleIndex.data() + dst, gCycleIndex.data() + src, right - left);
        memmove(gCycleWeight.data() + dst, gCycleWeight.data() + src, (right - left) * sizeof(uint16_t));
    }
    std::unordered_map<size_t, std::array<uint16_t, 26>> moved;
    for (const auto& entry : gCycleMix) {
        int x = static_cast<int>(entry.first % w) - dx, y = static_cast<int>(entry.first / w) - dy;
        if (x >= left && x < right && y >= top && y < bottom) moved.emplace(static_cast<size_t>(y) * w + x, entry.second);
    }
    gCycleMix.swap(moved);
}

void hdWorldPaletteChanged(int first, int count)
{
    if (!hdWorldActive()) return;
    ++gRevision;
    gDirtyAll = true;
    // CE: Partial updates are the cycle's unfaded palette. Full palette
    // changes are gamma/fades, applied once by the display transform.
    if (first > 0)
        for (int i = std::max(first, 229); i < std::min(first + count, 255); i++)
            gCycleColors[i] = paletteColor(i, true);
}
void hdWorldGrayscale()
{
    if (hdWorldActive()) {
        gGrayscale = true;
        ++gRevision;
        gDirtyAll = true;
    }
}

void hdWorldDraw(const FrmId& id, Art* art, int frame, int rotation,
    int x, int y, const Rect& clip, int light, const HdDrawOptions& options)
{
    if (!hdWorldActive() || gPixels.empty()) return;
    ++gRevision;
    int w = artGetWidth(art, frame, static_cast<Rotation>(rotation)), h = artGetHeight(art, frame, static_cast<Rotation>(rotation));
    if (w <= 0 || h <= 0) return;
    int left = std::max({ x, clip.left, 0 }) * 2, right = std::min({ x + w, clip.right + 1, gWidth }) * 2;
    int top = std::max({ y, clip.top, 0 }) * 2, bottom = std::min({ y + h, clip.bottom + 1, gHeight }) * 2;
    if (left >= right || top >= bottom) return;
    touch(left, top, right, bottom);
    auto hd = frameFor(id, frame, rotation, w, h);
    std::string drawKey = std::string(id.filePath()) + ":" + std::to_string(rotation) + ":" + std::to_string(frame) + ":" + std::to_string(x) + ":" + std::to_string(y);
    if (gCountedDraws.insert(drawKey).second) {
        if (hd)
            ++gHdCount;
        else
            ++gFallbackCount;
    }
    const unsigned char* original = artGetFrameData(art, frame, static_cast<Rotation>(rotation));
    uint32_t cyclingPalette[256] = { };
    for (int i = 229; i <= 254; i++)
        cyclingPalette[i] = paletteColor(i);
    // CE: Fallback art keeps the native palette's quantized lighting. Cache
    // the lit frame before magnifying it; HD itself keeps linear RGB light.
    std::string fallbackKey;
    if (!hd) {
        fallbackKey = std::string(id.filePath()) + ":" + std::to_string(rotation) + ":" + std::to_string(frame) + ":" + std::to_string(gFallbackFilter) + ":" + std::to_string(w) + "x" + std::to_string(h);
        if (options.tint >= 0)
            fallbackKey += ":unlit";
        else if (!options.lightMap)
            fallbackKey += ":" + std::to_string(std::clamp(light, 0, 65536) >> 9);
        else {
            fallbackKey += ":lit:";
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    fallbackKey.push_back(original[y * w + x] ? std::clamp(options.lightMap[y * options.lightPitch + x], 0, 65536) >> 9 : 0);
        }
    }
    auto fallback = hd ? nullptr : gCache->generated(fallbackKey, [&]() {
        auto image = std::make_shared<HdFrame>();
        image->width = w * 2;
        image->height = h * 2;
        image->pixels = fallbackPixels(original, w, h, light, options);
        return image;
    });
    const uint32_t* pixels = hd ? hd->pixels.data() : fallback->pixels.data();
    // CE: Most pixels of a refresh are opaque, unmasked and uncycled; they
    // take a direct store. Everything else goes through writePixel below.
    const bool plain = options.tint < 0 && !options.mask && options.opacity == 255;
    for (int dy = top; dy < bottom; dy++) {
        int sy = dy - y * 2;
        const unsigned char* sourceRow = original + (sy >> 1) * w;
        const uint32_t* pixelRow = pixels + static_cast<size_t>(sy) * w * 2;
        size_t rowOffset = static_cast<size_t>(dy) * gWidth * 2;
        for (int dx = left; dx < right; dx++) {
            int sx = dx - x * 2;
            unsigned char index = sourceRow[sx >> 1];
            uint32_t color = pixelRow[sx];
            if (plain && !hdIsCycling(index)) {
                if ((color >> 24) == 0) continue;
                if (hd) color = hdLight(color, light);
                size_t destOffset = rowOffset + dx;
                if ((color >> 24) != 255) {
                    writePixel(destOffset, color);
                    continue;
                }
                gPixels[destOffset] = color;
                gCycleIndex[destOffset] = 0;
                gCycleWeight[destOffset] = 0;
                if (!gCycleMix.empty()) gCycleMix.erase(destOffset);
                continue;
            }
            // Cycling comes from the source's index mask, even if the HD artist
            // painted a different color or alpha at this position.
            color = hdCyclePixel(color, index, cyclingPalette);
            if ((color >> 24) == 0) continue;
            int intensity = light;
            if (options.lightMap && index != 0) intensity = hdInterpolateLight(options.lightMap, options.lightPitch, w, h, sx, sy, original);
            size_t destOffset = static_cast<size_t>(dy) * gWidth * 2 + dx;
            uint32_t& dest = gPixels[destOffset];
            if (options.tint >= 0) {
                int row = hdBlendRow(color, options.glass);
                uint32_t blended = hdLight(hdBlend(dest, paletteColor(options.tint, false), row), intensity);
                int retain = (7 - std::min(row, 7)) * 255 / 7 * std::clamp(intensity, 0, 65536) / 65536;
                writePixel(destOffset, (blended & 0xFFFFFF) | (color & 0xFF000000), options.opacity, 0, retain);
            } else {
                if (hd && !hdIsCycling(index)) color = hdLight(color, intensity);
                int opacity = options.opacity;
                if (options.mask) {
                    int mask = options.mask[(dy / 2 - options.maskY) * options.maskPitch + dx / 2 - options.maskX];
                    if (mask) opacity = opacity * std::min(mask, 128) / 128;
                }
                writePixel(destOffset, color, opacity, hdIsCycling(index) ? index : 0);
            }
        }
    }
}

void hdWorldOutline(const FrmId& id, Art* art, int frame, int rotation,
    int x, int y, const Rect& clip, int color, int bandHeight, int bandCount, int tint)
{
    if (!hdWorldActive() || gPixels.empty()) return;
    ++gRevision;
    int w = artGetWidth(art, frame, static_cast<Rotation>(rotation)), h = artGetHeight(art, frame, static_cast<Rotation>(rotation));
    auto hd = frameFor(id, frame, rotation, w, h);
    auto original = artGetFrameData(art, frame, static_cast<Rotation>(rotation));
    auto opaque = [&](int sx, int sy) {
        return sx >= 0 && sy >= 0 && sx < w * 2 && sy < h * 2
            && (hd ? (hd->pixels[sy * w * 2 + sx] >> 24) != 0 : original[(sy / 2) * w + sx / 2] != 0);
    };
    int coverWidth = 0, coverHeight = 0, coverPitch = 0;
    const auto* cover = objectSeeThroughCover(&coverWidth, &coverHeight, &coverPitch);
    touch(std::max({ x - 1, clip.left, 0 }) * 2, std::max({ y - 1, clip.top, 0 }) * 2,
        std::min({ x + w + 1, clip.right + 1, gWidth }) * 2, std::min({ y + h + 1, clip.bottom + 1, gHeight }) * 2);
    for (int dy = std::max({ y - 1, clip.top, 0 }) * 2; dy < std::min({ y + h + 1, clip.bottom + 1, gHeight }) * 2; dy++)
        for (int dx = std::max({ x - 1, clip.left, 0 }) * 2; dx < std::min({ x + w + 1, clip.right + 1, gWidth }) * 2; dx++) {
            int sx = dx - x * 2, sy = dy - y * 2;
            if (opaque(sx, sy)) continue;
            bool edge = false;
            for (int d = 1; d <= 2; d++)
                if (opaque(sx - d, sy) || opaque(sx + d, sy) || opaque(sx, sy - d) || opaque(sx, sy + d)) edge = true;
            if (!edge) continue;
            int index = color;
            if (bandHeight > 0 && bandCount > 0) index += ((std::max(sy, 0) / 2) / bandHeight + 1) % bandCount;
            uint32_t c = paletteColor(index);
            size_t destOffset = static_cast<size_t>(dy) * gWidth * 2 + dx;
            uint32_t& dest = gPixels[destOffset];
            int blendRow = hdBlendRow(c, false);
            if (tint >= 0) c = hdBlend(dest, paletteColor(tint, false), blendRow);
            int opacity = 255;
            if (cover && dx / 2 < coverWidth && dy / 2 < coverHeight) {
                if (cover[(dy / 2) * coverPitch + dx / 2] == static_cast<unsigned char>(ObjectSeeThroughCover::Top)) continue;
                bool beside = false, seen = false;
                for (int d = 1; d <= 2; d++)
                    for (const SDL_Point& step : { SDL_Point { -d, 0 }, SDL_Point { d, 0 }, SDL_Point { 0, -d }, SDL_Point { 0, d } }) {
                        int nx = dx + step.x, ny = dy + step.y;
                        if (nx < 0 || ny < 0 || nx >= gWidth * 2 || ny >= gHeight * 2 || !opaque(sx + step.x, sy + step.y)) continue;
                        beside = true;
                        if (cover[(ny / 2) * coverPitch + nx / 2] == static_cast<unsigned char>(ObjectSeeThroughCover::SeeThrough)) seen = true;
                    }
                // The selected cycling target has no static band: it keeps
                // its complete outline, like the native tactical renderer.
                if (beside && !seen && bandCount == 0) opacity = 90;
            }
            writePixel(destOffset, c, opacity, hdIsCycling(index) && tint < 0 ? index : 0, tint >= 0 ? (7 - blendRow) * 255 / 7 : 0);
        }
}

void hdWorldCopyIndexedChanges(const Rect& rect, const unsigned char* before)
{
    if (!hdWorldActive() || gPixels.empty()) return;
    ++gRevision;
    int width = rect.right - rect.left + 1;
    const auto* buffer = worldViewGetBuffer();
    touch(rect.left * 2, rect.top * 2, (rect.right + 1) * 2, (rect.bottom + 1) * 2);
    for (int y = rect.top; y <= rect.bottom; y++)
        for (int x = rect.left; x <= rect.right; x++) {
            int index = buffer[y * gWidth + x];
            if (index == before[(y - rect.top) * width + x - rect.left]) continue;
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++)
                    writePixel((static_cast<size_t>(y) * 2 + sy) * gWidth * 2 + x * 2 + sx, paletteColor(index), 255, hdIsCycling(index) ? index : 0);
        }
}

bool hdWorldPresent(SDL_Renderer* renderer, const SDL_Rect& source, const SDL_FRect& dest)
{
    if (!hdWorldActive()) return false;
    if (gPixels.empty()) return false;
    int w = source.w * 2, h = source.h * 2;
    int tw = 0, th = 0;
    if (gTexture) SDL_QueryTexture(gTexture, nullptr, nullptr, &tw, &th);
    if (!gTexture || tw < w || th < h) {
        hdWorldResetRenderer();
        w = std::max(w, tw);
        h = std::max(h, th);
        SDL_RendererInfo info;
        if (SDL_GetRendererInfo(renderer, &info) == 0 && ((info.max_texture_width > 0 && w > info.max_texture_width) || (info.max_texture_height > 0 && h > info.max_texture_height))) {
            debugPrint("HD view exceeds renderer texture limits; using indexed view\n");
            return false;
        }
        gTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!gTexture) return false;
        SDL_SetTextureBlendMode(gTexture, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(gTexture, SDL_ScaleModeNearest);
    }
    w = source.w * 2;
    h = source.h * 2;
    SDL_Rect textureRect = { 0, 0, w, h };
    bool sameSource = source.x == gPresentedSource.x && source.y == gPresentedSource.y && source.w == gPresentedSource.w && source.h == gPresentedSource.h;
    if (gPresentedRevision == gRevision && sameSource) {
        SDL_RenderCopyF(renderer, gTexture, &textureRect, &dest);
        return true;
    }
    // Only the changed part, in texture coordinates, unless the view moved.
    SDL_Rect update = textureRect;
    if (!gDirtyAll && sameSource && gPresentedRevision != 0) {
        int left = std::max(gDirtyLeft - source.x * 2, 0), top = std::max(gDirtyTop - source.y * 2, 0);
        int right = std::min(gDirtyRight - source.x * 2, w), bottom = std::min(gDirtyBottom - source.y * 2, h);
        update = { left, top, std::max(right - left, 0), std::max(bottom - top, 0) };
    }
    if (update.w > 0 && update.h > 0) {
        void* data;
        int pitch;
        if (SDL_LockTexture(gTexture, &update, &data, &pitch) != 0) return false;
        auto convert = [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<unsigned char*>(data) + y * pitch);
                size_t src = (static_cast<size_t>(source.y) * 2 + update.y + y) * gWidth * 2 + source.x * 2 + update.x;
                for (int x = 0; x < update.w; x++)
                    row[x] = screenTransformWorldColor(livePixel(src + x));
            }
        };
        // CE: A full 2x view is millions of pixels; split it across cores
        // (livePixel and the colour transform only read shared state).
        int threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
        if (static_cast<size_t>(update.w) * update.h < 256 * 1024) threads = 1;
        std::vector<std::thread> workers;
        for (int i = 1; i < threads; i++)
            workers.emplace_back(convert, update.h * i / threads, update.h * (i + 1) / threads);
        convert(0, update.h / threads);
        for (auto& worker : workers)
            worker.join();
        SDL_UnlockTexture(gTexture);
    }
    gDirtyAll = false;
    gDirtyLeft = gDirtyTop = gDirtyRight = gDirtyBottom = 0;
    gPresentedRevision = gRevision;
    gPresentedSource = source;
    SDL_RenderCopyF(renderer, gTexture, &textureRect, &dest);
    return true;
}

void hdWorldResetRenderer()
{
    if (gTexture) SDL_DestroyTexture(gTexture);
    gTexture = nullptr;
    gPresentedRevision = 0;
}
} // namespace fallout
