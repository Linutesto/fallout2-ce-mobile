#include "hd_world.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include "art.h"
#include "color.h"
#include "db.h"
#include "debug.h"
#include "graphics.h"
#include "hd_art.h"
#include "hd_pixels.h"
#include "settings.h"
#include "svga.h"
#include "tile.h"
#include "world_view.h"

namespace fallout {
namespace {
int gWidth = 0;
int gHeight = 0;
bool gInstalled = false;
bool gPaletteDirty = false;
std::vector<uint32_t> gPixels;
std::unique_ptr<HdFrameCache> gCache;
SDL_Texture* gTexture = nullptr;
uint64_t gRevision = 1;
uint64_t gPresentedRevision = 0;
SDL_Rect gPresentedSource = {};
uint64_t gHdCount = 0;
uint64_t gFallbackCount = 0;

uint32_t paletteColor(int index, bool cycling = true)
{
    const unsigned char* palette = cycling && hdIsCycling(index) ? _systemCmap : _cmap;
    return 0xFF000000 | (palette[index*3] * 4 << 16) | (palette[index*3+1] * 4 << 8) | (palette[index*3+2] * 4);
}

bool readPng(const std::string& path, std::vector<unsigned char>& data)
{
    File* file = fileOpen(path.c_str(), "rb");
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
    return gCache->get(id.filePath(), rotation, frame, width, height);
}

// CE: Only fallback sprites pass through the user's pixel-art filter. HD
// pixels are never magnified by MMPX/Scale3x or smeared by that setting.
std::vector<uint32_t> fallbackPixels(const unsigned char* src, int w, int h)
{
    std::vector<uint32_t> base(static_cast<size_t>(w)*h);
    for (size_t i=0; i<base.size(); i++) base[i] = src[i] == 0 ? 0 : paletteColor(src[i]);
    std::vector<uint32_t> out(static_cast<size_t>(w)*h*4);
    int filter = settings.world_view.filter;
    if (filter == 3) {
        graphicsMmpx(base.data(), w, h, out.data());
    } else if (filter == 4) {
        std::vector<uint32_t> three(static_cast<size_t>(w)*h*9);
        graphicsScale3x(base.data(), w, h, three.data());
        for (int y=0; y<h*2; y++) for (int x=0; x<w*2; x++) out[y*w*2+x] = three[(y*3/2)*w*3+x*3/2];
    } else {
        for (int y=0; y<h*2; y++) for (int x=0; x<w*2; x++) {
            if (filter != 1) { out[y*w*2+x] = base[(y/2)*w+x/2]; continue; }
            // Premultiplied bilinear at the centers of the 2x pixels, so
            // transparent black cannot bleed into a lit sprite's edge.
            int x0 = std::clamp((x-1)/2, 0, w-1), y0 = std::clamp((y-1)/2, 0, h-1);
            int x1 = std::min(x0+1, w-1), y1 = std::min(y0+1, h-1);
            int fx = x == 0 ? 0 : (x & 1) ? 1 : 3;
            int fy = y == 0 ? 0 : (y & 1) ? 1 : 3;
            uint32_t samples[] = { base[y0*w+x0], base[y0*w+x1], base[y1*w+x0], base[y1*w+x1] };
            int weights[] = { (4-fx)*(4-fy), fx*(4-fy), (4-fx)*fy, fx*fy };
            int a=0, channels[3] = {};
            for (int i=0;i<4;i++) {
                int coverage = (samples[i] >> 24)*weights[i]; a += coverage;
                for (int c=0;c<3;c++) channels[c] += ((samples[i]>>(c*8))&255)*coverage;
            }
            uint32_t c = (a/16)<<24;
            if (a) for (int i=0;i<3;i++) c |= (channels[i]/a)<<(i*8);
            out[y*w*2+x] = c;
        }
    }
    return out;
}
} // namespace

void hdWorldInit(int width, int height)
{
    hdWorldExit();
    gWidth = width; gHeight = height;
    for (const XBase* base=xbaseGetFirst(); base; base=base->next) {
        std::error_code error;
        std::filesystem::path path(base->path);
        if ((!base->isDbase && std::filesystem::is_directory(path / "hd", error))
            || (base->isDbase && path.filename() == "hd.dat")) gInstalled = true;
    }
    size_t budget = static_cast<size_t>(std::clamp(settings.graphics.hd_cache_mb, 0, 4096))*1024*1024;
    gCache = std::make_unique<HdFrameCache>(budget, readPng,
        [](const std::string& message) { debugPrint("HD art: %s\n", message.c_str()); });
    if (gInstalled) debugPrint("HD art installed; cache budget %zu bytes\n", budget);
}

void hdWorldExit()
{
    hdWorldResetRenderer();
    gPixels.clear(); gPixels.shrink_to_fit();
    gCache.reset(); gInstalled = false; gWidth = gHeight = 0;
}

bool hdWorldInstalled() { return gInstalled; }
bool hdWorldActive() { return gInstalled && settings.graphics.hd_art && worldViewIsEnabled(); }
const uint32_t* hdWorldPixels() { return gPixels.empty() ? nullptr : gPixels.data(); }
void hdWorldResetCounts() { gHdCount = gFallbackCount = 0; }
void hdWorldGetCounts(uint64_t* hd, uint64_t* fallback) { *hd=gHdCount; *fallback=gFallbackCount; }

void hdWorldSettingsChanged()
{
    gPaletteDirty = false;
    if (hdWorldActive()) gPixels.resize(static_cast<size_t>(gWidth)*gHeight*4, 0xFF000000);
    else { gPixels.clear(); gPixels.shrink_to_fit(); }
    worldViewForgetRendered();
    tileWindowRefresh();
}

void hdWorldClear(const Rect& rect)
{
    if (!hdWorldActive()) return;
    ++gRevision;
    if (gPixels.empty()) gPixels.resize(static_cast<size_t>(gWidth)*gHeight*4, 0xFF000000);
    int left = std::max(rect.left, 0)*2, right=std::min(rect.right+1, gWidth)*2;
    for (int y=std::max(rect.top,0)*2; y<std::min(rect.bottom+1,gHeight)*2; y++)
        std::fill(gPixels.begin()+static_cast<size_t>(y)*gWidth*2+left, gPixels.begin()+static_cast<size_t>(y)*gWidth*2+right, 0xFF000000);
}

void hdWorldScrolled(int dx, int dy)
{
    if (!hdWorldActive() || gPixels.empty()) return;
    ++gRevision;
    int w=gWidth*2,h=gHeight*2; dx*=2; dy*=2;
    int left=std::max(-dx,0), right=std::min(w-dx,w);
    int top=std::max(-dy,0), bottom=std::min(h-dy,h);
    if (left>=right || top>=bottom) return;
    int y=dy>=0 ? top : bottom-1, end=dy>=0 ? bottom : top-1, step=dy>=0 ? 1 : -1;
    for (;y!=end;y+=step) memmove(gPixels.data()+static_cast<size_t>(y)*w+left,
        gPixels.data()+static_cast<size_t>(y+dy)*w+left+dx, (right-left)*sizeof(uint32_t));
}

void hdWorldPaletteChanged() { if (hdWorldActive()) { gPaletteDirty=true; ++gRevision; } }

void hdWorldDraw(const FrmId& id, Art* art, int frame, int rotation,
    int x, int y, const Rect& clip, int light, const HdDrawOptions& options)
{
    if (!hdWorldActive() || gPixels.empty()) return;
    ++gRevision;
    int w=artGetWidth(art,frame,static_cast<Rotation>(rotation)), h=artGetHeight(art,frame,static_cast<Rotation>(rotation));
    if (w<=0 || h<=0) return;
    int left=std::max({x,clip.left,0})*2, right=std::min({x+w,clip.right+1,gWidth})*2;
    int top=std::max({y,clip.top,0})*2, bottom=std::min({y+h,clip.bottom+1,gHeight})*2;
    if (left>=right || top>=bottom) return;
    auto hd=frameFor(id,frame,rotation,w,h);
    if (hd) ++gHdCount; else ++gFallbackCount;
    const unsigned char* original=artGetFrameData(art,frame,static_cast<Rotation>(rotation));
    auto fallback = hd ? std::vector<uint32_t>() : fallbackPixels(original,w,h);
    const uint32_t* pixels=hd ? hd->pixels.data() : fallback.data();
    for (int dy=top;dy<bottom;dy++) for (int dx=left;dx<right;dx++) {
        int sx=dx-x*2, sy=dy-y*2;
        unsigned char index=original[(sy/2)*w+sx/2];
        uint32_t color=pixels[static_cast<size_t>(sy)*w*2+sx];
        // Cycling comes from the source's index mask, even if the HD artist
        // painted a different color or alpha at this position.
        if (hdIsCycling(index)) color=paletteColor(index);
        if ((color>>24)==0) continue;
        int intensity=light;
        if (options.lightMap) intensity=options.lightMap[(sy/2)*options.lightPitch+sx/2];
        uint32_t& dest=gPixels[static_cast<size_t>(dy)*gWidth*2+dx];
        if (options.tint>=0) {
            int r=(color>>16)&255,g=(color>>8)&255,b=color&255;
            int row=options.glass ? (r+5*g+4*b)/160 : (b+3*r+6*g)/160;
            uint32_t blended=hdLight(hdBlend(dest,paletteColor(options.tint,false),row),intensity);
            dest=hdOver((blended&0xFFFFFF)|(color&0xFF000000),dest,options.opacity);
        } else {
            if (!hdIsCycling(index)) color=hdLight(color,intensity);
            int opacity=options.opacity;
            if (options.mask) {
                int mask=options.mask[(dy/2-options.maskY)*options.maskPitch+dx/2-options.maskX];
                if (mask) opacity=opacity*std::min(mask,128)/128;
            }
            dest=hdOver(color,dest,opacity);
        }
    }
}

void hdWorldOutline(const FrmId& id, Art* art, int frame, int rotation,
    int x, int y, const Rect& clip, int color, int bandHeight, int bandCount, int tint)
{
    if (!hdWorldActive() || gPixels.empty()) return;
    ++gRevision;
    int w=artGetWidth(art,frame,static_cast<Rotation>(rotation)),h=artGetHeight(art,frame,static_cast<Rotation>(rotation));
    auto hd=frameFor(id,frame,rotation,w,h);
    auto original=artGetFrameData(art,frame,static_cast<Rotation>(rotation));
    auto opaque=[&](int sx,int sy) {
        return sx>=0 && sy>=0 && sx<w*2 && sy<h*2
            && (hd ? (hd->pixels[sy*w*2+sx]>>24)!=0 : original[(sy/2)*w+sx/2]!=0);
    };
    for (int dy=std::max({y-1,clip.top,0})*2;dy<std::min({y+h+1,clip.bottom+1,gHeight})*2;dy++)
        for (int dx=std::max({x-1,clip.left,0})*2;dx<std::min({x+w+1,clip.right+1,gWidth})*2;dx++) {
            int sx=dx-x*2,sy=dy-y*2;
            if (opaque(sx,sy)) continue;
            bool edge=false;
            for (int d=1;d<=2;d++) if (opaque(sx-d,sy)||opaque(sx+d,sy)||opaque(sx,sy-d)||opaque(sx,sy+d)) edge=true;
            if (!edge) continue;
            int index=color;
            if (bandHeight>0 && bandCount>0) index+=((std::max(sy,0)/2)/bandHeight+1)%bandCount;
            uint32_t c=paletteColor(index);
            uint32_t& dest=gPixels[static_cast<size_t>(dy)*gWidth*2+dx];
            if (tint>=0) c=hdBlend(dest,paletteColor(tint,false),5);
            dest=c;
        }
}

void hdWorldCopyIndexedChanges(const Rect& rect, const unsigned char* before)
{
    if (!hdWorldActive() || gPixels.empty()) return;
    ++gRevision;
    int width=rect.right-rect.left+1;
    const auto* buffer=worldViewGetBuffer();
    for (int y=rect.top;y<=rect.bottom;y++) for (int x=rect.left;x<=rect.right;x++) {
        int index=buffer[y*gWidth+x];
        if (index==before[(y-rect.top)*width+x-rect.left]) continue;
        for (int sy=0;sy<2;sy++) for (int sx=0;sx<2;sx++)
            gPixels[(static_cast<size_t>(y)*2+sy)*gWidth*2+x*2+sx]=paletteColor(index);
    }
}

bool hdWorldPresent(SDL_Renderer* renderer, const SDL_Rect& source, const SDL_FRect& dest)
{
    if (!hdWorldActive()) return false;
    // CE: Reuse the exact native traversal when water, fire or cycling
    // outlines change. Rebuilding also preserves blending over cycling art.
    if (gPaletteDirty) { gPaletteDirty=false; tileWindowRefresh(); }
    if (gPixels.empty()) return false;
    int w=source.w*2,h=source.h*2;
    int tw=0,th=0;
    if (gTexture) SDL_QueryTexture(gTexture,nullptr,nullptr,&tw,&th);
    if (!gTexture || tw!=w || th!=h) {
        hdWorldResetRenderer();
        gTexture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h);
        if (!gTexture) return false;
        SDL_SetTextureBlendMode(gTexture,SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(gTexture,SDL_ScaleModeNearest);
    }
    bool sameSource = source.x==gPresentedSource.x && source.y==gPresentedSource.y && source.w==gPresentedSource.w && source.h==gPresentedSource.h;
    if (gPresentedRevision == gRevision && sameSource) {
        SDL_RenderCopyF(renderer,gTexture,nullptr,&dest);
        return true;
    }
    void* data; int pitch;
    if (SDL_LockTexture(gTexture,nullptr,&data,&pitch)!=0) return false;
    for (int y=0;y<h;y++) {
        uint32_t* row=reinterpret_cast<uint32_t*>(static_cast<unsigned char*>(data)+y*pitch);
        const uint32_t* src=gPixels.data()+(static_cast<size_t>(source.y)*2+y)*gWidth*2+source.x*2;
        for (int x=0;x<w;x++) row[x]=screenTransformWorldColor(src[x]);
    }
    SDL_UnlockTexture(gTexture);
    gPresentedRevision=gRevision; gPresentedSource=source;
    SDL_RenderCopyF(renderer,gTexture,nullptr,&dest);
    return true;
}

void hdWorldResetRenderer() { if (gTexture) SDL_DestroyTexture(gTexture); gTexture=nullptr; gPresentedRevision=0; }
} // namespace fallout
