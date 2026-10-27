#ifndef FALLOUT_HD_ART_H_
#define FALLOUT_HD_ART_H_
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace fallout {
std::string hdFramePath(const std::string& artPath, int direction, int frame);
struct HdFrame {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};
class HdFrameCache {
public:
    using Reader = std::function<bool(const std::string&, std::vector<unsigned char>&)>;
    using Logger = std::function<void(const std::string&)>;
    HdFrameCache(size_t budget, Reader reader, Logger logger);
    std::shared_ptr<const HdFrame> get(const std::string& artPath, int direction, int frame, int originalWidth, int originalHeight);
    size_t bytes() const { return used_; }
    void setBudget(size_t budget);
    void clear();
    // CE: Fallback magnifiers share the PNG cache's byte budget.
    std::shared_ptr<const HdFrame> generated(const std::string& key, const std::function<std::shared_ptr<HdFrame>()>& build);

private:
    struct Entry {
        std::string key;
        std::shared_ptr<const HdFrame> image;
    };
    size_t budget_;
    size_t used_ = 0;
    Reader reader_;
    Logger logger_;
    std::list<Entry> lru_;
    std::unordered_map<std::string, std::list<Entry>::iterator> entries_;
    std::list<std::string> failedLru_;
    std::unordered_map<std::string, std::list<std::string>::iterator> failed_;
    std::unordered_set<std::string> logged_;
    void evict(size_t needed);
    void store(const std::string& key, std::shared_ptr<const HdFrame> image);
};
} // namespace fallout
#endif
