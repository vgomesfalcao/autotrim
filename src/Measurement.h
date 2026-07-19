// Panel-driven mass measurement. Message thread only: the panel's timer
// calls poll() every frame; the audio threads just accumulate measuredPeak
// while `measuring` is set.
#pragma once

#include <memory>

namespace autotrim
{
struct ChannelShared;
}

namespace autotrim::measurement
{
// Measure every automated channel (panel action).
void start(float durationS);
// Measure a single channel (in-plugin action); no-signal still leaves the
// gain untouched.
void startChannel(const std::shared_ptr<ChannelShared>& channel, float durationS);
void cancel();
// Applies trims when the window has elapsed.
void poll();
bool isRunning();
// 0..1 while running, -1 when idle.
float progress();
} // namespace autotrim::measurement
