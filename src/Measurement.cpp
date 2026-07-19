#include "Measurement.h"

#include "Dsp.h"
#include "Registry.h"

namespace autotrim::measurement
{
namespace
{
    bool running = false;
    double startMs = 0.0;
    double durationMs = 0.0;
    // Non-null while measuring a single channel instead of all of them.
    std::shared_ptr<ChannelShared> soloChannel;

    void arm(ChannelShared& ch)
    {
        ch.measEpoch.fetch_add(1);
        ch.measuredPeak.store(0.0f);
        ch.noSignal.store(false);
        ch.measuring.store(true);
    }

    void finishChannel(ChannelShared& ch, float maxTrim);

    void applyTrim(ChannelShared& ch, float trimDb)
    {
        if (ch.trimParam == nullptr)
            return;
        ch.trimParam->beginChangeGesture();
        ch.trimParam->setValueNotifyingHost(ch.trimParam->convertTo0to1(trimDb));
        ch.trimParam->endChangeGesture();
        ch.riderOffsetDb.store(0.0f);
        // A fresh measurement recalibrates the trim, so the protective cut
        // starts over.
        ch.protectOffsetDb.store(0.0f);
        ch.protectionActive.store(false);
    }

    // No-signal channels are flagged and left completely untouched.
    void finishChannel(ChannelShared& ch, float maxTrim)
    {
        if (! ch.measuring.exchange(false))
            return;
        const float peakDb = dsp::gainToDb(ch.measuredPeak.load());
        const float targetDb =
            ch.targetDb != nullptr ? ch.targetDb->load() : dsp::kDefaultTargetDb;
        if (auto trim = dsp::computeTrimDb(peakDb, targetDb, maxTrim))
            applyTrim(ch, *trim);
        else
            ch.noSignal.store(true);
    }
} // namespace

void start(float durationS)
{
    if (running)
        return;
    for (auto& ch : registry::channels())
    {
        if (! ch->isAutomationOn())
            continue;
        arm(*ch);
    }
    soloChannel = nullptr;
    running = true;
    startMs = juce::Time::getMillisecondCounterHiRes();
    durationMs = juce::jmax(0.5f, durationS) * 1000.0;
}

void startChannel(const std::shared_ptr<ChannelShared>& channel, float durationS)
{
    if (running || channel == nullptr)
        return;
    arm(*channel);
    soloChannel = channel;
    running = true;
    startMs = juce::Time::getMillisecondCounterHiRes();
    durationMs = juce::jmax(0.5f, durationS) * 1000.0;
}

void cancel()
{
    if (! running)
        return;
    running = false;
    if (soloChannel != nullptr)
        soloChannel->measuring.store(false);
    else
        for (auto& ch : registry::channels())
            ch->measuring.store(false);
    soloChannel = nullptr;
}

void poll()
{
    if (! running || juce::Time::getMillisecondCounterHiRes() - startMs < durationMs)
        return;
    running = false;

    const float maxTrim = registry::maxTrimDb.load();
    if (soloChannel != nullptr)
    {
        finishChannel(*soloChannel, maxTrim);
        soloChannel = nullptr;
        return;
    }
    for (auto& ch : registry::channels())
        finishChannel(*ch, maxTrim);
}

bool isRunning() { return running; }

float progress()
{
    if (! running)
        return -1.0f;
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - startMs;
    return juce::jlimit(0.0, 1.0, elapsed / durationMs);
}
} // namespace autotrim::measurement
