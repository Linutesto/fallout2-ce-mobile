#include "fps_limiter.h"

#include <SDL.h>
#include <algorithm>

#include "perf_monitor.h"

namespace fallout {

FpsLimiter::FpsLimiter(unsigned int fps)
    : _fps(fps)
    , _ticks(0)
{
}

void FpsLimiter::mark()
{
    _ticks = SDL_GetTicks();
    _start = SDL_GetPerformanceCounter();
}

void FpsLimiter::setPresentation(unsigned int hz, void (*present)())
{
    if (hz != _presentationHz || present != _present) _presentationCredit = 0.0;
    _presentationHz = hz;
    _present = present;
}

void FpsLimiter::throttle() const
{
    if (_present != nullptr && _presentationHz > _fps) {
        Uint64 frequency = SDL_GetPerformanceFrequency();
        Uint64 deadline = _start + frequency * (1000 / _fps) / 1000;
        // Include the caller's presentation in the target rate. In particular
        // 90 Hz needs fewer subframes than 120 Hz; the original integer-ms
        // logic wait is 16 ms, not 16.67 ms. Carry fractions between waits.
        _presentationCredit += std::max(0.0, _presentationHz * (1000 / _fps) / 1000.0 - 1.0);
        int extra = static_cast<int>(_presentationCredit);
        _presentationCredit -= extra;
        for (int frame = 1; frame <= extra; frame++) {
            Uint64 next = _start + (deadline - _start) * frame / (extra + 1);
            if (SDL_GetPerformanceCounter() >= deadline) break;
            while (SDL_GetPerformanceCounter() < next) SDL_Delay(1);
            if (SDL_GetPerformanceCounter() >= deadline) break;
            _present();
        }
    }
    if (1000 / _fps > SDL_GetTicks() - _ticks) {
        unsigned int delay = 1000 / _fps - (SDL_GetTicks() - _ticks);
        perfMonitorSleep(delay);
        SDL_Delay(delay);
    }
}

} // namespace fallout
