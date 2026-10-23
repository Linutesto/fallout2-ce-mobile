#ifndef FALLOUT_DEV_STORY_TEST_H_
#define FALLOUT_DEV_STORY_TEST_H_
namespace fallout {
// CE: Tick-driven gameplay regressions; false means an HTTP call is pending.
bool devStoryTestTick(int stage);
bool devStoryRealTestTick(int stage);
bool devStoryGuardTestTick(int stage);
} // namespace fallout
#endif
