#ifndef FPS_LIMITER_H
#define FPS_LIMITER_H

namespace fallout {

class FpsLimiter {
public:
    FpsLimiter(unsigned int fps = 60);
    void mark();
    void throttle() const;
    // Extra presentation/input work during the existing logic wait. Never
    // changes the limiter's logic budget or calls game tickers.
    void setPresentation(unsigned int hz, void (*present)());

private:
    const unsigned int _fps;
    unsigned int _ticks;
    unsigned long long _start = 0;
    unsigned int _presentationHz = 0;
    void (*_present)() = nullptr;
    mutable double _presentationCredit = 0.0;
};

} // namespace fallout

#endif /* FPS_LIMITER_H */
