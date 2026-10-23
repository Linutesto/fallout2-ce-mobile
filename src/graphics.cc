#include "graphics.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fallout {

namespace {
    struct Grade {
        const char* name;
        double brightness;
        double contrast;
        double gamma;
        double saturation;
        double warmth;
    };

    constexpr Grade kGrades[] = {
        { "original", 0.0, 1.0, 1.0, 1.0, 0.0 },
        { "oled_punch", -0.04, 1.35, 1.12, 1.15, 0.0 },
        { "warm_wasteland", 0.012, 1.18, 1.05, 1.10, 0.02 },
        { "cold_vault", -0.02, 1.22, 1.08, 0.95, -0.02 },
        { "night_readable", 0.0, 0.80, 0.92, 1.00, 0.0 },
    };

    double clamp01(double value)
    {
        return std::clamp(value, 0.0, 1.0);
    }

    int luma(uint32_t p)
    {
        return (int((p >> 16) & 255) + int((p >> 8) & 255) + int(p & 255) + 1) * (256 - int(p >> 24));
    }
} // namespace

const char* graphicsColorPresetName(int preset)
{
    return kGrades[std::clamp(preset, 0, kColorPresetCount - 1)].name;
}

int graphicsColorPresetIndex(const char* name)
{
    for (int index = 0; index < kColorPresetCount; index++) {
        if (name != nullptr && strcmp(name, kGrades[index].name) == 0) {
            return index;
        }
    }
    return 0;
}

uint32_t graphicsGradeColor(uint32_t color, int preset)
{
    if (preset <= 0 || preset >= kColorPresetCount) {
        return color;
    }
    const Grade& p = kGrades[preset];
    double channels[] = { double((color >> 16) & 255) / 255.0, double((color >> 8) & 255) / 255.0, double(color & 255) / 255.0 };
    for (double& x : channels) {
        x = clamp01(x + p.brightness);
        x = clamp01(0.5 + (x - 0.5) * p.contrast);
        x = std::pow(x, p.gamma);
    }
    if (p.saturation != 1.0) {
        double y = 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
        for (double& x : channels) {
            x = clamp01(y + (x - y) * p.saturation);
        }
    }
    if (p.warmth != 0.0) {
        channels[0] = clamp01(channels[0] + p.warmth);
        channels[2] = clamp01(channels[2] - p.warmth);
    }
    uint32_t result = color & 0xFF000000;
    for (int c = 0; c < 3; c++) {
        result |= uint32_t(std::clamp(int(channels[c] * 255.0 + 0.5), 0, 255)) << (16 - c * 8);
    }
    return result;
}

void graphicsScale3x(const uint32_t* source, int width, int height, uint32_t* dest)
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            auto at = [&](int dx, int dy) { return source[std::clamp(y + dy, 0, height - 1) * width + std::clamp(x + dx, 0, width - 1)]; };
            uint32_t A = at(-1, -1), B = at(0, -1), C = at(1, -1);
            uint32_t D = at(-1, 0), E = at(0, 0), F = at(1, 0);
            uint32_t G = at(-1, 1), H = at(0, 1), I = at(1, 1);
            uint32_t* top = dest + y * 3 * width * 3 + x * 3;
            uint32_t* mid = top + width * 3;
            uint32_t* bot = mid + width * 3;
            top[0] = top[1] = top[2] = mid[0] = mid[1] = mid[2] = bot[0] = bot[1] = bot[2] = E;
            if (B != H && D != F) {
                top[0] = D == B ? D : E;
                top[1] = ((D == B && E != C) || (B == F && E != A)) ? B : E;
                top[2] = B == F ? F : E;
                mid[0] = ((D == B && E != G) || (D == H && E != A)) ? D : E;
                mid[2] = ((B == F && E != I) || (H == F && E != C)) ? F : E;
                bot[0] = D == H ? D : E;
                bot[1] = ((D == H && E != I) || (H == F && E != G)) ? H : E;
                bot[2] = H == F ? F : E;
            }
        }
    }
}

// MMPX, Morgan McGuire & Mara Gagiu, JCGT 10(2), 2021.
// Port of the MIT reference via upscale.py. See licenses/mmpx.txt.
// Rule order matters: intersections overwrite slopes, then 2:1 slopes copy
// the current output values. Luma includes alpha, as in the published code.
void graphicsMmpx(const uint32_t* source, int width, int height, uint32_t* dest)
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            auto at = [&](int dx, int dy) { return source[std::clamp(y + dy, 0, height - 1) * width + std::clamp(x + dx, 0, width - 1)]; };
            uint32_t A = at(-1, -1), B = at(0, -1), C = at(1, -1);
            uint32_t D = at(-1, 0), E = at(0, 0), F = at(1, 0);
            uint32_t G = at(-1, 1), H = at(0, 1), I = at(1, 1);
            uint32_t J = E, K = E, L = E, M = E;
            if (!(A == B && B == C && C == D && D == E && E == F && F == G && G == H && H == I)) {
                uint32_t P = at(0, -2), Q = at(-2, 0), R = at(2, 0), S = at(0, 2);
                int Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);
                if (D == B && D != H && D != F && (El >= Dl || E == A)
                    && (E == A || E == C || E == G) && (El < Dl || A != D || E != P || E != Q)) J = D;
                if (B == F && B != D && B != H && (El >= Bl || E == C)
                    && (E == A || E == C || E == I) && (El < Bl || C != B || E != P || E != R)) K = B;
                if (H == D && H != F && H != B && (El >= Hl || E == G)
                    && (E == A || E == G || E == I) && (El < Hl || G != H || E != S || E != Q)) L = H;
                if (F == H && F != B && F != D && (El >= Fl || E == I)
                    && (E == C || E == G || E == I) && (El < Fl || I != H || E != R || E != S)) M = F;

                if (E != F && E == C && C == I && I == D && D == Q && F == B && B == H && F != at(3, 0)) K = M = F;
                if (E != D && E == A && A == G && G == F && F == R && D == B && B == H && D != at(-3, 0)) J = L = D;
                if (E != H && E == G && G == I && I == B && B == P && H == D && D == F && H != at(0, 3)) L = M = H;
                if (E != B && E == A && A == C && C == H && H == S && B == D && D == F && B != at(0, -3)) J = K = B;
                if (Bl < El && E == G && G == H && H == I && I == S && E != A && E != D && E != C && E != F) J = K = B;
                if (Hl < El && E == A && A == B && B == C && C == P && E != D && E != G && E != I && E != F) L = M = H;
                if (Fl < El && E == A && A == D && D == G && G == Q && E != B && E != C && E != I && E != H) K = M = F;
                if (Dl < El && E == C && C == F && F == I && I == R && E != B && E != A && E != G && E != H) J = L = D;

                if (H != B) {
                    if (H != A && H != E && H != C) {
                        if (H == G && G == F && F == R && H != D && H != at(2, -1)) L = M;
                        if (H == I && I == D && D == Q && H != F && H != at(-2, -1)) M = L;
                    }
                    if (B != I && B != G && B != E) {
                        if (B == A && A == F && F == R && B != D && B != at(2, 1)) J = K;
                        if (B == C && C == D && D == Q && B != F && B != at(-2, 1)) K = J;
                    }
                }
                if (F != D) {
                    if (D != I && D != E && D != C) {
                        if (D == A && A == H && H == S && D != B && D != at(1, 2)) J = L;
                        if (D == G && G == B && B == P && D != H && D != at(1, -2)) L = J;
                    }
                    if (F != E && F != A && F != G) {
                        if (F == C && C == H && H == S && F != B && F != at(-1, 2)) K = M;
                        if (F == I && I == B && B == P && F != H && F != at(-1, -2)) M = K;
                    }
                }
            }
            uint32_t* top = dest + y * 2 * width * 2 + x * 2;
            top[0] = J;
            top[1] = K;
            top[width * 2] = L;
            top[width * 2 + 1] = M;
        }
    }
}

} // namespace fallout
