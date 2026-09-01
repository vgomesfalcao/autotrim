#include "Measurement.h"

#include "Dsp.h"
#include "Registry.h"

#include <algorithm>

namespace autotrim::measurement
{
namespace
{
    // Mass-batch bookkeeping only — for the panel's progress bar and
    // Cancelar button. Individual per-channel measurements are tracked
    // purely on the channel itself (measuring/measArmDeadlineMs) and never
    // touch this state, which is what keeps them independent of each other.
    bool massRunning = false;
    std::vector<std::shared_ptr<ChannelShared>> massInvolved;

    // Drum channels get a longer deadline than the processor's own listening
    // window (registry::measDrumWindowS): the processor's window-based close
    // is what normally fires (and it runs the frequent-peak / corroboration
    // logic on the way out), this deadline is only the safety net for a
    // channel that never gets polled to close on its own.
    double deadlineMsFor(const ChannelShared& ch)
    {
        const int profileIdx = ch.profile != nullptr ? (int) ch.profile->load() : 1;
        const double windowS = dsp::profileFor(profileIdx).hitBased
                                   ? (double) registry::measDrumWindowS.load() + 5.0
                                   : (double) dsp::kMeasArmTimeoutS;
        return juce::Time::getMillisecondCounterHiRes() + windowS * 1000.0;
    }

    void arm(ChannelShared& ch, double deadlineMs)
    {
        ch.measEpoch.fetch_add(1);
        ch.measuredPeak.store(0.0f);
        ch.noSignal.store(false);
        ch.measStarted.store(false);
        ch.measDone.store(false);
        ch.measHitCount.store(0);
        ch.measFinishNow.store(false);
        ch.measArmDeadlineMs.store(deadlineMs);
        ch.measuring.store(true);
    }

    void applyTrim(ChannelShared& ch, float trimDb)
    {
        if (ch.trimParam == nullptr)
            return;
        ch.trimParam->beginChangeGesture();
        ch.trimParam->setValueNotifyingHost(ch.trimParam->convertTo0to1(trimDb));
        ch.trimParam->endChangeGesture();
        ch.riderOffsetDb.store(0.0f);
        ch.agcOffsetDb.store(0.0f);
        // A fresh measurement recalibrates the trim, so the protective cut
        // starts over.
        ch.protectOffsetDb.store(0.0f);
        ch.protectionActive.store(false);
    }

    // No-signal channels are flagged and left completely untouched.
    void finishChannel(ChannelShared& ch, float maxTrim)
    {
        const float peakDb = dsp::gainToDb(ch.measuredPeak.load());
        const float targetDb =
            ch.targetDb != nullptr ? ch.targetDb->load() : dsp::kDefaultTargetDb;
        if (auto trim = dsp::computeTrimDb(peakDb, targetDb, maxTrim))
            applyTrim(ch, *trim);
        else
            ch.noSignal.store(true);
    }

    // Stops a channel's measurement without resolving it into a trim — used
    // by every cancel path.
    void stopMeasuring(ChannelShared& ch)
    {
        ch.measuring.store(false);
        ch.measDone.store(false);
        ch.measFinishNow.store(false);
    }
} // namespace

void start(float)
{
    // The mass batch itself is single-flight (re-clicking it mid-batch is a
    // no-op); this does NOT gate individual per-channel measurements below.
    if (massRunning)
        return;
    massInvolved.clear();
    for (auto& ch : registry::channels())
    {
        if (! ch->isAutomationOn() || ch->measuring.load())
            continue; // off, or already mid-measurement on its own — leave it alone
        arm(*ch, deadlineMsFor(*ch));
        massInvolved.push_back(ch);
    }
    massRunning = ! massInvolved.empty();
}

void startChannel(const std::shared_ptr<ChannelShared>& channel, float)
{
    if (channel == nullptr || channel->measuring.load())
        return; // already has a measurement running (its own, or in a mass batch)
    arm(*channel, deadlineMsFor(*channel));
}

void resetAll()
{
    // Every channel, mass or individual: zeroing while one is still
    // mid-measurement would race the trim it's about to apply.
    for (auto& ch : registry::channels())
        if (ch->measuring.load())
            stopMeasuring(*ch);
    massInvolved.clear();
    massRunning = false;
    for (auto& ch : registry::channels())
    {
        applyTrim(*ch, 0.0f); // Ganho -> 0 dB, clears rider/AGC/protection
        ch->noSignal.store(false);
    }
}

void cancel()
{
    for (auto& ch : massInvolved)
        stopMeasuring(*ch);
    massInvolved.clear();
    massRunning = false;
}

void cancelChannel(const std::shared_ptr<ChannelShared>& channel)
{
    if (channel == nullptr)
        return;
    stopMeasuring(*channel);
    massInvolved.erase(std::remove(massInvolved.begin(), massInvolved.end(), channel),
                       massInvolved.end());
}

void finishChannelNow(const std::shared_ptr<ChannelShared>& channel)
{
    if (channel != nullptr && channel->measuring.load())
        channel->measFinishNow.store(true);
}

void poll()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float maxTrim = registry::maxTrimDb.load();

    // Every channel is checked, not just the last mass batch: an
    // individually-started measurement must finish/time out on its own too.
    for (auto& ch : registry::channels())
    {
        if (ch->measDone.exchange(false))
        {
            finishChannel(*ch, maxTrim);
        }
        else if (ch->measuring.load() && now > ch->measArmDeadlineMs.load())
        {
            // Whatever was captured decides: never-started channels have
            // peak 0 -> "sem sinal", untouched.
            ch->measuring.store(false);
            finishChannel(*ch, maxTrim);
        }
    }

    if (massRunning)
    {
        const bool anyPending = std::any_of(massInvolved.begin(), massInvolved.end(),
                                            [](const auto& ch) { return ch->measuring.load(); });
        if (! anyPending)
        {
            massRunning = false;
            massInvolved.clear();
        }
    }
}

bool isRunning() { return massRunning; }

float progress()
{
    if (! massRunning || massInvolved.empty())
        return massRunning ? 0.0f : -1.0f;
    int finished = 0;
    for (auto& ch : massInvolved)
        if (! ch->measuring.load())
            ++finished;
    return (float) finished / (float) massInvolved.size();
}
} // namespace autotrim::measurement
