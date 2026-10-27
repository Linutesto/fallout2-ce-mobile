// CE: HD task item 1: paths, PNG validation, per-frame fallback and LRU budget.
#include "hd_art.h"
#include "lodepng.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>
using namespace fallout;
int main()
{
    assert(hdFramePath("ART\\TILES\\edg1000.FRM", 0, 0) == "hd/art/tiles/edg1000/0_0.png");
    assert(hdFramePath("art/critters/hmjmpsaa.fr3", 3, 12) == "hd/art/critters/hmjmpsaa/3_12.png");
    assert(hdFramePath("../bad.frm", 0, 0).empty());
    assert(hdFramePath("/art/bad.frm", 0, 0).empty());
    assert(hdFramePath("art/a.frm", 6, 0).empty());
    assert(hdFramePath("art/a.frm", 0, -1).empty());
    std::map<std::string, std::vector<unsigned char>> files;
    int reads = 0, logs = 0;
    auto png = [](unsigned w, unsigned h) {
        std::vector<unsigned char> data(w*h*4, 127), out;
        assert(lodepng::encode(out, data, w, h) == 0);
        return out;
    };
    files["hd/art/a/0_0.png"] = png(4, 6);
    files["hd/art/a/0_1.png"] = png(4, 6);
    files["hd/art/a/0_2.png"] = png(4, 6);
    files["hd/art/a/0_3.png"] = png(5, 6);
    files["hd/art/a/0_4.png"] = {1, 2, 3};
    HdFrameCache cache(192,
        [&](const std::string& path, std::vector<unsigned char>& out) {
            ++reads; auto it = files.find(path); if (it == files.end()) return false;
            out = it->second; return true;
        }, [&](const std::string&) { ++logs; });
    auto a = cache.get("art/a.frm", 0, 0, 2, 3);
    assert(a && a->width == 4 && a->height == 6 && a->pixels.size() == 24);
    assert(a->pixels[0] == 0x7F7F7F7F);
    assert(cache.bytes() == 96 && reads == 1);
    assert(cache.get("art/a.frm", 0, 1, 2, 3));
    assert(cache.get("art/a.frm", 0, 0, 2, 3) == a && reads == 2);
    assert(cache.get("art/a.frm", 0, 2, 2, 3) && cache.bytes() == 192);
    assert(cache.get("art/a.frm", 0, 0, 2, 3) == a && reads == 3);
    assert(cache.get("art/a.frm", 0, 1, 2, 3) && reads == 4);
    assert(a->pixels[0] == 0x7F7F7F7F); // held frame survives eviction
    assert(!cache.get("art/a.frm", 0, 3, 2, 3));
    assert(!cache.get("art/a.frm", 0, 4, 2, 3));
    assert(!cache.get("art/a.frm", 0, 5, 2, 3));
    int failedReads = reads;
    for (int i = 3; i <= 5; ++i) assert(!cache.get("art/a.frm", 0, i, 2, 3));
    assert(reads == failedReads && logs == 2);
    // Dimensions are part of the lookup's validation even on a cached hit.
    assert(!cache.get("art/a.frm", 0, 1, 3, 3));
    assert(!cache.get("art/a.frm", 0, 1, 0, 3));
    cache.setBudget(96); assert(cache.bytes() <= 96);
    cache.setBudget(0); assert(cache.bytes() == 0);
    assert(cache.get("art/a.frm", 0, 0, 2, 3));
    assert(cache.bytes() == 0);
    cache.clear(); assert(cache.bytes() == 0);
    return 0;
}
