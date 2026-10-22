// sfall_virtual_buffer.h: bounded, big-endian storage for sfall fs_* opcodes.
#include "sfall_virtual_buffer.h"
#include <cassert>
#include <cstdint>
#include <vector>
using fallout::SfallVirtualBuffer;
int main() {
    SfallVirtualBuffer b(8);
    assert(b.size() == 8 && b.pos() == 0);
    assert(b.write(0x12345678, 4));
    assert(b.write(0xFE, 1));
    assert(b.pos() == 5);
    assert(!b.write(0x12345678, 4) && b.pos() == 5);
    assert(!b.seek(-1) && !b.seek(9) && b.pos() == 5);
    assert(b.seek(0) && b.read(4) == 0x12345678);
    assert(b.read(1) == 0xFE && b.pos() == 5);
    assert(b.read(4) == 0 && b.pos() == 5);
    assert(b.resize(3) && b.pos() == 0 && b.size() == 3);
    assert(b.read(2) == 0x1234 && b.read(1) == 0x56);
    assert(b.resize(8) && b.pos() == 0);
    assert(b.read(4) == 0x12345600);
    assert(!b.resize(-1) && !b.resize(10 * 1024 * 1024 + 1));
    assert(b.seek(0) && !b.write(3, 0) && !b.write(3, 5));
    assert(b.pos() == 0);
    SfallVirtualBuffer c(std::vector<unsigned char>{0,1,2,3});
    assert(c.size() == 4 && c.read(4) == 0x00010203);
    assert(c.bytes().size() == 4 && c.bytes()[3] == 3);
    SfallVirtualBuffer z(0);
    assert(z.size() == 0 && z.seek(0) && z.read(1) == 0 && !z.write(1,1));
}
