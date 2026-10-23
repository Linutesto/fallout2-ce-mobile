package com.alexbatalov.fallout2ce;

import org.junit.Test;
import static org.junit.Assert.*;

public class RefreshRatePolicyTest {
    @Test public void requestsHighestUsableRate() {
        assertEquals(120f, RefreshRatePolicy.choose(true, new float[]{60, 90, 120}), 0f);
        assertEquals(144f, RefreshRatePolicy.choose(true, new float[]{144, 60, 120}), 0f);
    }
    @Test public void offRequestsSixty() {
        assertEquals(60f, RefreshRatePolicy.choose(false, new float[]{60, 90, 120}), 0f);
    }
    @Test public void invalidAndMissingModesAreSafe() {
        assertEquals(60f, RefreshRatePolicy.choose(true, null), 0f);
        assertEquals(60f, RefreshRatePolicy.choose(true, new float[]{}), 0f);
        assertEquals(60f, RefreshRatePolicy.choose(true, new float[]{0, -120, Float.NaN, Float.POSITIVE_INFINITY, 1000}), 0f);
    }
    @Test public void supportsFractionalAndSixtyOnlyModes() {
        assertEquals(119.88f, RefreshRatePolicy.choose(true, new float[]{59.94f, 119.88f}), 0f);
        assertEquals(60f, RefreshRatePolicy.choose(true, new float[]{60}), 0f);
    }
}
