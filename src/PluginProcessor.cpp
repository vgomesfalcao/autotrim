#include "PluginProcessor.h"

#include "Dsp.h"
#include "PluginEditor.h"

namespace autotrim
{
namespace
{
    juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { "target", 1 }, "Target",
            NormalisableRange<float>(-60.0f, 0.0f, 0.1f), dsp::kDefaultTargetDb,
            AudioParameterFloatAttributes().withLabel("dBFS")));
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { "trim", 1 }, "Ganho",
            NormalisableRange<float>(-dsp::kTrimParamRangeDb, dsp::kTrimParamRangeDb, 0.1f), 0.0f,
            AudioParameterFloatAttributes().withLabel("dB")));
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { "automation", 1 }, utf8("Automação"), true));
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { "rider", 1 }, utf8("Modo contínuo"), false));
        layout.add(std::make_unique<AudioParameterChoice>(
            ParameterID { "profile", 1 }, "Perfil",
            StringArray { "Voz", "Instrumento", "Bateria" }, 1));
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { "sens", 1 }, "Sensibilidade",
            NormalisableRange<float>(-80.0f, -20.0f, 1.0f),
            dsp::kProfiles[1].sensitivityDb,
            AudioParameterFloatAttributes().withLabel("dBFS")));
        return layout;
    }
} // namespace

AutoTrimProcessor::AutoTrimProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "AutoTrim", makeLayout()),
      shared(registry::registerInstance())
{
    shared->targetDb = apvts.getRawParameterValue("target");
    shared->trimDb = apvts.getRawParameterValue("trim");
    shared->automationOn = apvts.getRawParameterValue("automation");
    shared->riderOn = apvts.getRawParameterValue("rider");
    shared->profile = apvts.getRawParameterValue("profile");
    shared->sensitivityDb = apvts.getRawParameterValue("sens");
    shared->targetParam = apvts.getParameter("target");
    shared->trimParam = apvts.getParameter("trim");
    shared->automationParam = apvts.getParameter("automation");
    shared->profileParam = apvts.getParameter("profile");
    shared->sensParam = apvts.getParameter("sens");
}

AutoTrimProcessor::~AutoTrimProcessor()
{
    registry::unregisterInstance(shared);
}

void AutoTrimProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = sampleRate;
    const auto sr = (float) sampleRate;
    gainCoef = dsp::onepoleCoef(dsp::kGainSmoothS, sr);
    hitWindowSamples = juce::jmax(1, (int) (dsp::kHitWindowS * sr));
    hitRetriggerSamples = juce::jmax(1, (int) (dsp::kHitRetriggerS * sr));
    gainLin = dsp::dbToGain(shared->effectiveTrimDb());
    resetHitState();
}

void AutoTrimProcessor::resetHitState()
{
    for (auto& slot : holdSlots)
        slot = 0.0f;
    holdSlotIndex = 0;
    holdSlotElapsed = 0.0f;
    inHit = false;
    hitPeak = 0.0f;
    hitWindowSamplesLeft = 0;
    hitCooldownSamples = 0;
    hitHistoryCount = 0;
    hitHistoryPos = 0;
    sinceHitS = 1000.0f;
    sinceCorrectionS = 1000.0f;
    resetProtectionWindow();
}

void AutoTrimProcessor::resetProtectionWindow()
{
    protEventIndex = 0;
    protEventCount = 0;
    protOverActive = false;
    protOverElapsedS = 0.0f;
    protOffenderMaxLin = 0.0f;
}

bool AutoTrimProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();
    // mono->mono, stereo->stereo, and Logic's beloved mono->stereo insert.
    return (in == mono && (out == mono || out == stereo))
           || (in == stereo && out == stereo);
}

void AutoTrimProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples == 0)
        return;

    // Mono-to-stereo layouts: duplicate the mono input into the extra output
    // channels before processing.
    for (int ch = getMainBusNumInputChannels(); ch < numChannels; ++ch)
        buffer.copyFrom(ch, 0, buffer, 0, 0, numSamples);

    // A panel-mode instance is management-only: pure passthrough. It must
    // never apply gain, ride, or trigger overload protection — it usually
    // sits on the master bus, where a protective cut would duck the whole
    // show (this happened live).
    if (shared->panelMode.load())
    {
        gainLin = 1.0f;
        if (shared->riderOffsetDb.load() != 0.0f)
            shared->riderOffsetDb.store(0.0f);
        if (shared->protectOffsetDb.load() != 0.0f || shared->protectionActive.load())
        {
            shared->protectOffsetDb.store(0.0f);
            shared->protectionActive.store(false);
        }
        return;
    }

    const float maxTrim = registry::maxTrimDb.load();
    const bool automationOn = shared->isAutomationOn();
    const bool riderOn = automationOn && shared->riderOn->load() > 0.5f;
    const bool measuring = shared->measuring.load();
    const float trimDb = shared->trimDb->load();
    const auto& profile = dsp::profileFor((int) shared->profile->load());
    const float sensDb = shared->sensitivityDb->load();
    const float sensLin = dsp::dbToGain(sensDb);
    const bool detectHits = riderOn && ! measuring && profile.hitBased;
    bool hitCompleted = false;

    float offsetDb = shared->riderOffsetDb.load();
    const float protectDb = shared->protectOffsetDb.load();
    float totalDb = juce::jlimit(-maxTrim, maxTrim, trimDb + offsetDb + protectDb);
    const float targetGain = automationOn ? dsp::dbToGain(totalDb) : 1.0f;

    auto* const* channelData = buffer.getArrayOfWritePointers();
    float blockPeak = 0.0f;
    float blockPeakPost = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float framePeak = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            framePeak = juce::jmax(framePeak, std::abs(channelData[ch][i]));

        blockPeak = juce::jmax(blockPeak, framePeak);

        if (detectHits)
        {
            if (inHit)
            {
                hitPeak = juce::jmax(hitPeak, framePeak);
                if (--hitWindowSamplesLeft <= 0)
                {
                    hitHistoryDb[hitHistoryPos] = dsp::gainToDb(hitPeak);
                    hitHistoryPos = (hitHistoryPos + 1) % dsp::kHitHistory;
                    hitHistoryCount = juce::jmin(hitHistoryCount + 1, dsp::kHitHistory);
                    inHit = false;
                    hitCooldownSamples = hitRetriggerSamples - hitWindowSamples;
                    hitCompleted = true;
                }
            }
            else if (hitCooldownSamples > 0)
            {
                --hitCooldownSamples;
            }
            else if (framePeak > sensLin)
            {
                inHit = true;
                hitPeak = framePeak;
                hitWindowSamplesLeft = hitWindowSamples;
            }
        }

        gainLin += gainCoef * (targetGain - gainLin);
        for (int ch = 0; ch < numChannels; ++ch)
            channelData[ch][i] *= gainLin;
        blockPeakPost = juce::jmax(blockPeakPost, framePeak * gainLin);
    }

    // Metering: max-hold with decay.
    const float dt = (float) numSamples / (float) currentSampleRate;
    const float meterDecay = dsp::dbToGain(-dsp::kMeterDecayDbPerS * dt);
    shared->peakPreTrim.store(
        juce::jmax(shared->peakPreTrim.load() * meterDecay, blockPeak));
    shared->peakPostTrim.store(
        juce::jmax(shared->peakPostTrim.load() * meterDecay, blockPeakPost));

    // Measurement window: this thread is the only writer of measuredPeak; the
    // epoch bump tells us the panel started a new run. Armed: the window only
    // starts counting when signal first crosses the gate, so sources that
    // play at isolated moments (toms) still get a full-window capture.
    const uint32_t epoch = shared->measEpoch.load();
    if (epoch != measEpoch)
    {
        measEpoch = epoch;
        measPeak = 0.0f;
        measStartedLocal = false;
        measSamplesLeft = 0;
    }
    // Overload protection: if the *output* peaks past target + margin too
    // many times inside the window, cut the trim to protect the downstream
    // chain — regardless of the rider being on. A sustained overload re-arms
    // one event per rearm period so it is caught too.
    if (automationOn && ! measuring)
    {
        protClockS += dt;
        const float overThresholdLin =
            dsp::dbToGain(shared->targetDb->load() + dsp::kProtectMarginDb);
        if (blockPeakPost > overThresholdLin)
        {
            protSinceOverS = 0.0f;
            protOffenderMaxLin = juce::jmax(protOffenderMaxLin, blockPeakPost);
            bool newEvent = false;
            if (! protOverActive)
            {
                protOverActive = true;
                protOverElapsedS = 0.0f;
                newEvent = true;
            }
            else
            {
                protOverElapsedS += dt;
                if (protOverElapsedS >= dsp::kProtectRearmS)
                {
                    protOverElapsedS = 0.0f;
                    newEvent = true;
                }
            }

            if (newEvent)
            {
                protEventTimes[protEventIndex] = protClockS;
                protEventIndex = (protEventIndex + 1) % dsp::kProtectHitCount;
                protEventCount = juce::jmin(protEventCount + 1, dsp::kProtectHitCount);
                // After the increment, the next slot holds the oldest event.
                const float oldest = protEventTimes[protEventIndex];
                if (protEventCount >= dsp::kProtectHitCount
                    && protClockS - oldest <= dsp::kProtectWindowS)
                {
                    const float cut =
                        shared->targetDb->load() - dsp::gainToDb(protOffenderMaxLin);
                    shared->protectOffsetDb.store(
                        juce::jlimit(-dsp::kProtectMaxCutDb, 0.0f, protectDb + cut));
                    shared->protectionActive.store(true);
                    resetProtectionWindow();
                }
            }
        }
        else
        {
            protOverActive = false;
            protSinceOverS += dt;
        }

        // The cut is temporary: once the overs stop for the hold period and
        // the recent output peak leaves headroom for the give-back, glide the
        // protection back to zero. Hot signal returning re-triggers the cut.
        if (protectDb < 0.0f && protSinceOverS > dsp::kProtectHoldS)
        {
            const float recentOutDb = dsp::gainToDb(shared->peakPostTrim.load());
            const float thresholdDb = shared->targetDb->load() + dsp::kProtectMarginDb;
            if (recentOutDb + 1.0f <= thresholdDb)
            {
                const float released =
                    juce::jmin(0.0f, protectDb + dsp::kProtectReleaseDbPerS * dt);
                if (released >= -0.05f)
                {
                    shared->protectOffsetDb.store(0.0f);
                    shared->protectionActive.store(false);
                }
                else
                {
                    shared->protectOffsetDb.store(released);
                }
            }
        }
    }

    // A stale rider correction must not keep acting after the mode is off.
    if (! riderOn && offsetDb != 0.0f)
    {
        shared->riderOffsetDb.store(0.0f);
        resetHitState();
    }

    if (measuring)
    {
        if (! measStartedLocal && blockPeak > dsp::dbToGain(dsp::kGateDb))
        {
            measStartedLocal = true;
            shared->measStarted.store(true);
            measSamplesLeft =
                (int) (registry::measDurationS.load() * (float) currentSampleRate);
        }
        if (measStartedLocal)
        {
            measPeak = juce::jmax(measPeak, blockPeak);
            shared->measuredPeak.store(measPeak);
            measSamplesLeft -= numSamples;
            if (measSamplesLeft <= 0)
            {
                shared->measuring.store(false);
                shared->measDone.store(true);
            }
        }
    }
    else if (riderOn)
    {
        // Continuous rider: block-rate adjustment of the internal offset,
        // confined to the profile's ride range around the measured trim (the
        // trim parameter itself is only written from the message thread).
        const float target = shared->targetDb->load();
        float newOffset = offsetDb;

        if (! profile.hitBased)
        {
            // Sliding peak-hold: coarse 100 ms slots over the profile window.
            const int numSlots = juce::jlimit(
                1, dsp::kMaxHoldSlots, (int) (profile.holdS / dsp::kHoldSlotS));
            holdSlots[holdSlotIndex % dsp::kMaxHoldSlots] =
                juce::jmax(holdSlots[holdSlotIndex % dsp::kMaxHoldSlots], blockPeak);
            holdSlotElapsed += dt;
            while (holdSlotElapsed >= dsp::kHoldSlotS)
            {
                holdSlotElapsed -= dsp::kHoldSlotS;
                holdSlotIndex = (holdSlotIndex + 1) % numSlots;
                holdSlots[holdSlotIndex] = 0.0f;
            }
            float holdMax = blockPeak;
            for (int i = 0; i < numSlots; ++i)
                holdMax = juce::jmax(holdMax, holdSlots[i]);

            const float levelDb = dsp::gainToDb(holdMax);
            const bool hasProgram =
                levelDb >= sensDb
                && dsp::riderSeesProgram(levelDb, trimDb, offsetDb, target, profile);
            newOffset = hasProgram
                            ? dsp::riderOffsetStep(offsetDb, levelDb, trimDb, target, profile, dt)
                            : dsp::riderIdleStep(offsetDb, profile, dt);
        }
        else
        {
            sinceHitS += dt;
            sinceCorrectionS += dt;
            if (hitCompleted)
            {
                sinceHitS = 0.0f;
                float sumDb = 0.0f;
                for (int i = 0; i < hitHistoryCount; ++i)
                    sumDb += hitHistoryDb[i];
                const float avgHitDb = sumDb / (float) juce::jmax(1, hitHistoryCount);
                newOffset = dsp::riderOffsetStep(
                    offsetDb, avgHitDb, trimDb, target, profile,
                    juce::jmin(sinceCorrectionS, dsp::kHitCorrectionMaxDtS));
                sinceCorrectionS = 0.0f;
            }
            else if (sinceHitS > dsp::kDrumIdleS)
            {
                newOffset = dsp::riderIdleStep(offsetDb, profile, dt);
            }
        }

        if (newOffset != offsetDb)
            shared->riderOffsetDb.store(newOffset);
    }
}

void AutoTrimProcessor::updateTrackProperties(const TrackProperties& properties)
{
    if (! properties.name.has_value())
        return;
    const juce::ScopedLock lock(shared->nameLock);
    shared->hostName = *properties.name;
}

juce::AudioProcessorEditor* AutoTrimProcessor::createEditor()
{
    return new AutoTrimEditor(*this);
}

void AutoTrimProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock lock(shared->nameLock);
        state.setProperty("channelName", shared->name, nullptr);
    }
    state.setProperty("panelMode", shared->panelMode.load(), nullptr);
    state.setProperty("panelCompact", shared->panelCompact.load(), nullptr);
    // Global panel settings ride along with every instance, but only a panel
    // instance restores them, so a channel's stale copy never wins.
    state.setProperty("maxTrimDb", registry::maxTrimDb.load(), nullptr);
    state.setProperty("measDurationS", registry::measDurationS.load(), nullptr);

    juce::MemoryOutputStream stream(destData, false);
    state.writeToStream(stream);
}

void AutoTrimProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto state = juce::ValueTree::readFromData(data, (size_t) sizeInBytes);
    if (! state.isValid())
        return;

    apvts.replaceState(state);
    {
        const juce::ScopedLock lock(shared->nameLock);
        shared->name = state.getProperty("channelName", "").toString();
    }
    const bool panelMode = (bool) state.getProperty("panelMode", false);
    shared->panelMode.store(panelMode);
    shared->panelCompact.store((bool) state.getProperty("panelCompact", false));
    if (panelMode)
    {
        registry::maxTrimDb.store((float) (double) state.getProperty(
            "maxTrimDb", (double) dsp::kDefaultMaxTrimDb));
        registry::measDurationS.store((float) (double) state.getProperty(
            "measDurationS", (double) dsp::kDefaultMeasDurationS));
    }
}
} // namespace autotrim

// JUCE plugin entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new autotrim::AutoTrimProcessor();
}
