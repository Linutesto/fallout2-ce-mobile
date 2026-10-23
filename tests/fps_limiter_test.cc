// fps_limiter: graphics item 3. Fake SDL clock verifies presentation subframes
// leave the original logic deadline alone, and do not catch up after stalls.
#include "fps_limiter.h"
#include <SDL.h>
#include <cassert>
#include <cstdio>
#include <initializer_list>

static Uint64 clockUs;
static int frames;
extern "C" Uint32 SDL_GetTicks() { return clockUs / 1000; }
extern "C" Uint64 SDL_GetPerformanceCounter() { return clockUs; }
extern "C" Uint64 SDL_GetPerformanceFrequency() { return 1000000; }
extern "C" void SDL_Delay(Uint32 ms) { clockUs += ms * 1000; }
namespace fallout { void perfMonitorSleep(unsigned int) {} }
static void present() { frames++; }

int main()
{
    fallout::FpsLimiter logic;
    for (unsigned int rate : { 0u, 60u, 90u, 120u, 144u, 240u }) {
        clockUs = 0;
        frames = 0;
        logic.setPresentation(rate, present);
        for (int tick = 0; tick < 60; tick++) {
            logic.mark();
            logic.throttle();
        }
        assert(clockUs == 960000); // Exact existing 1000 / 60 logic budget.
        assert((rate > 60) == (frames > 0));
        if (rate > 60) {
            double achieved = (60 + frames) / 0.96;
            assert(achieved <= rate + 1 && achieved >= rate - 1);
        }
        clockUs = 0;
        frames = 0;
        logic.mark();
        clockUs = 25000; // A slow game frame already used its time.
        logic.throttle();
        assert(frames == 0 && clockUs == 25000);
    }
    logic.setPresentation(120, nullptr);
    clockUs = 0;
    logic.mark();
    logic.throttle();
    assert(clockUs == 16000);
    puts("6 refresh rates, unchanged 60 logic waits, slow frames, disabled callback: PASS");
}
