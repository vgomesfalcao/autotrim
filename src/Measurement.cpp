#include "Measurement.h"

#include "Dsp.h"
#include "Registry.h"

namespace autotrim::measurement
{
namespace
{
    bool running = false;
    double armDeadlineMs = 0.0;
    std::vector<std::shared_ptr<ChannelShared>> involved;

    void arm(ChannelShared& ch)
    {
        ch.measEpoch.fetch_add(1);
        ch.measuredPeak.store(0.0f);
        ch.noSignal.store(false);
        ch.measStarted.store(false);
        ch.measDone.store(false);
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

    void begin()
    {
        running = true;
        armDeadlineMs =
            juce::Time::getMillisecondCounterHiRes() + dsp::kMeasArmTimeoutS * 1000.0;
    }
} // namespace

void start(float)
{
    if (running)
        return;
    involved.clear();
    for (auto& ch : registry::channels())
    {
        if (! ch->isAutomationOn())
            continue;
        arm(*ch);
        involved.push_back(ch);
    }
    begin();
}

void startChannel(const std::shared_ptr<ChannelShared>& channel, float)
{
    if (running || channel == nullptr)
        return;
    involved.clear();
    arm(*channel);
    involved.push_back(channel);
    begin();
}

void cancel()
{
    if (! running)
        return;
    running = false;
    for (auto& ch : involved)
    {
        ch->measuring.store(false);
        ch->measDone.store(false);
    }
    involved.clear();
}

void poll()
{
    if (! running)
        return;

    const float maxTrim = registry::maxTrimDb.load();
    const bool timedOut = juce::Time::getMillisecondCounterHiRes() > armDeadlineMs;
    bool anyPending = false;

    for (auto& ch : involved)
    {
        if (ch->measDone.exchange(false))
        {
            finishChannel(*ch, maxTrim);
        }
        else if (ch->measuring.load())
        {
            if (timedOut)
            {
                // Whatever was captured decides: never-started channels have
                // peak 0 -> "sem sinal", untouched.
                ch->measuring.store(false);
                finishChannel(*ch, maxTrim);
            }
            else
            {
                anyPending = true;
            }
        }
    }

    if (! anyPending)
    {
        running = false;
        involved.clear();
    }
}

bool isRunning() { return running; }

float progress()
{
    if (! running || involved.empty())
        return running ? 0.0f : -1.0f;
    int finished = 0;
    for (auto& ch : involved)
        if (! ch->measuring.load())
            ++finished;
    return (float) finished / (float) involved.size();
}
} // namespace autotrim::measurement
