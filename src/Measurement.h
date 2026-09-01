// Panel-driven measurement. Message thread only: every open editor's timer
// calls poll() every frame; the audio threads just accumulate measuredPeak
// while `measuring` is set.
//
// Each channel's measurement is fully independent — its own arm/deadline
// state lives on its ChannelShared (measuring/measArmDeadlineMs), so one
// channel mid-measurement never blocks another from starting, whether that
// other channel was included in a mass "start()" batch or measured
// individually via startChannel(). "Mass" bookkeeping below (isRunning /
// progress / cancel) only concerns the panel-wide "Regular ganhos de todos
// os canais" batch and its Cancelar button.
#pragma once

#include <memory>

namespace autotrim
{
struct ChannelShared;
}

namespace autotrim::measurement
{
// Measure every automated channel that isn't already mid-measurement (panel
// action). A no-op while a previous mass batch is still running.
void start(float durationS);
// Measure a single channel (in-plugin or panel-row action); no-op if that
// channel already has a measurement in flight. No-signal still leaves the
// gain untouched. Always available, independent of any other measurement.
void startChannel(const std::shared_ptr<ChannelShared>& channel, float durationS);
// Sets every channel's Ganho back to 0 dB and clears rider/AGC/protection
// offsets (panel action). Cancels every running measurement first, mass or
// individual.
void resetAll();
// Cancels only the in-flight mass batch (panel's Cancelar button); channels
// the user is measuring individually keep running.
void cancel();
// Cancels one channel's own measurement, whether it started individually or
// as part of a mass batch.
void cancelChannel(const std::shared_ptr<ChannelShared>& channel);
// Applies trims when a channel's window has elapsed. Safe to call from
// multiple open editors every frame.
void poll();
// Is a mass batch (start()) currently in flight? Individual per-channel
// measurements don't affect this — check ChannelShared::measuring for those.
bool isRunning();
// 0..1 across the current mass batch, -1 when idle.
float progress();
} // namespace autotrim::measurement
