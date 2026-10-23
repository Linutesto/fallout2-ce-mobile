package com.alexbatalov.fallout2ce;

// Pure policy, tested on the host JVM; Android filters modes by resolution.
final class RefreshRatePolicy {
    static float choose(boolean high, float[] rates) {
        float best = 60.0f;
        if (high && rates != null) {
            for (float rate : rates) {
                if (Float.isFinite(rate) && rate > best && rate <= 240.0f) best = rate;
            }
        }
        return best;
    }
}
