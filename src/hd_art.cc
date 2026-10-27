#include "hd_art.h"
#include <algorithm>
#include <cctype>
#include <lodepng.h>
#include <utility>
namespace fallout {
std::string hdFramePath(const std::string& artPath, int direction, int frame)
{
    if (direction < 0 || direction >= 6 || frame < 0) return { };
    std::string path = artPath;
    for (char& c : path)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (path.compare(0, 4, "art/") != 0 || path.find(':') != std::string::npos || path.find('\0') != std::string::npos) return { };
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        std::string part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") return { };
        if (end == std::string::npos) break;
        start = end + 1;
    }
    size_t dot = path.find_last_of('.'), slash = path.find_last_of('/');
    if (dot == std::string::npos || dot <= slash + 1 || dot + 1 == path.size()) return { };
    return "hd/" + path.substr(0, dot) + "/" + std::to_string(direction) + "_" + std::to_string(frame) + ".png";
}

HdFrameCache::HdFrameCache(size_t budget, Reader reader, Logger logger)
    : budget_(budget)
    , reader_(std::move(reader))
    , logger_(std::move(logger))
{
}

void HdFrameCache::evict(size_t needed)
{
    while (!lru_.empty() && (needed > budget_ || used_ > budget_ - needed)) {
        auto last = std::prev(lru_.end());
        used_ -= last->image->pixels.size() * sizeof(uint32_t);
        entries_.erase(last->key);
        lru_.erase(last);
    }
}

void HdFrameCache::setBudget(size_t budget)
{
    budget_ = budget;
    evict(0);
}

void HdFrameCache::clear()
{
    entries_.clear();
    lru_.clear();
    failed_.clear();
    failedLru_.clear();
    used_ = 0;
    // CE: A malformed file is logged once per cache lifetime, including
    // across eviction and explicit flushes.
}

std::shared_ptr<const HdFrame> HdFrameCache::get(const std::string& artPath, int direction, int frame, int originalWidth, int originalHeight)
{
    std::string path = hdFramePath(artPath, direction, frame);
    // CE: Bound output allocation before inspecting or decoding IDAT data.
    if (path.empty() || originalWidth <= 0 || originalHeight <= 0 || originalWidth > 32767 || originalHeight > 32767
        || static_cast<uint64_t>(originalWidth) * originalHeight > 4 * 1024 * 1024) return nullptr;
    std::string key = path + ":" + std::to_string(originalWidth) + "x" + std::to_string(originalHeight);
    auto found = entries_.find(key);
    if (found != entries_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second);
        return found->second->image;
    }
    auto negative = failed_.find(key);
    if (negative != failed_.end()) {
        failedLru_.splice(failedLru_.begin(), failedLru_, negative->second);
        return nullptr;
    }
    std::vector<unsigned char> encoded;
    bool exists = reader_(path, encoded);
    unsigned w = 0, h = 0;
    lodepng::State state;
    unsigned error = exists && !encoded.empty() && encoded.size() <= 64 * 1024 * 1024
        ? lodepng_inspect(&w, &h, &state, encoded.data(), encoded.size())
        : 1;
    std::vector<unsigned char> rgba;
    if (error == 0 && w == static_cast<unsigned>(originalWidth) * 2 && h == static_cast<unsigned>(originalHeight) * 2) {
        // CE: A palette or grayscale PNG is also lossless; normalize every
        // supported encoding to RGBA8 rather than rejecting optimized packs.
        state.info_raw.colortype = LCT_RGBA;
        state.info_raw.bitdepth = 8;
        state.decoder.zlibsettings.max_output_size = static_cast<size_t>(w) * h * 8 + 1024 * 1024;
        error = lodepng::decode(rgba, w, h, state, encoded);
    } else
        error = 1;
    if (error || rgba.size() != static_cast<size_t>(w) * h * 4) {
        failedLru_.push_front(key);
        failed_[key] = failedLru_.begin();
        if (failed_.size() > 4096) {
            failed_.erase(failedLru_.back());
            failedLru_.pop_back();
        }
        if (exists && logged_.insert(path).second && logger_) logger_("Invalid or wrong-size PNG: " + path);
        return nullptr;
    }
    auto image = std::make_shared<HdFrame>();
    image->width = w;
    image->height = h;
    image->pixels.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < image->pixels.size(); i++) {
        const unsigned char* p = rgba.data() + i * 4;
        image->pixels[i] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }
    store(key, image);
    return image;
}

void HdFrameCache::store(const std::string& key, std::shared_ptr<const HdFrame> image)
{
    size_t bytes = image->pixels.size() * sizeof(uint32_t);
    // CE: Oversized frames are valid but never become a resident exception.
    if (bytes <= budget_) {
        evict(bytes);
        lru_.push_front({ key, image });
        entries_[key] = lru_.begin();
        used_ += bytes;
    }
}

std::shared_ptr<const HdFrame> HdFrameCache::generated(const std::string& name, const std::function<std::shared_ptr<HdFrame>()>& build)
{
    std::string key = "generated:" + name;
    auto found = entries_.find(key);
    if (found != entries_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second);
        return found->second->image;
    }
    auto image = build();
    if (image) store(key, image);
    return image;
}
} // namespace fallout
