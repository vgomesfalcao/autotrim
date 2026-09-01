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
        // AGC: slow re-trim on sustained program-level changes. Works well for
        // some sources only, so it ships off.
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { "agc", 1 }, "AGC", false));
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { "agctime", 1 }, "Tempo do AGC",
            NormalisableRange<float>(dsp::kAgcHoldMinS, dsp::kAgcHoldMaxS, 1.0f),
            dsp::kAgcHoldS, AudioParameterFloatAttributes().withLabel("s")));
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { "agcrange", 1 }, utf8("Máx. do AGC"),
            NormalisableRange<float>(dsp::kAgcRangeMinDb, dsp::kAgcRangeMaxDb, 0.5f),
            dsp::kAgcRangeDb, AudioParameterFloatAttributes().withLabel("dB")));
        // Clip Guard: the emergency cut on real clipping (> 0 dBFS). On by
        // default; the toggle lives in the Avançado section.
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { "clipguard", 1 }, "Clip Guard", true));
        layout.add(std::make_unique<AudioParameterChoice>(
            ParameterID { "profile", 1 }, "Perfil",
            StringArray { "Voz", "Instrumento", "Bateria" }, 1));
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { "sens", 1 }, "Sensibilidade",
            NormalisableRange<float>(-80.0f, -20.0f, 1.0f),
            dsp::kProfiles[1].sensitivityDb,
            AudioParameterFloatAttributes().withLabel("dBFS")));
        // Rider ride-up speed; switching profile resets it to that profile's
        // default (like the sensitivity).
        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID { "speed", 1 }, "Velocidade",
            NormalisableRange<float>(dsp::kSpeedMinDbPerS, dsp::kSpeedMaxDbPerS, 0.1f),
            dsp::kProfiles[1].upDbPerS,
            AudioParameterFloatAttributes().withLabel("dB/s")));
        // Measurement algorithm: true = highest peak (default), false =
        // average. Per channel; peak is the safe default (calibrate to the
        // loudest moment), average suits sources where the typical level
        // matters more than the odd spike.
        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID { "peakmode", 1 }, "Medir por pico", true));
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
    shared->agcOn = apvts.getRawParameterValue("agc");
    shared->agcHoldS = apvts.getRawParameterValue("agctime");
    shared->agcRangeDb = apvts.getRawParameterValue("agcrange");
    shared->clipGuardOn = apvts.getRawParameterValue("clipguard");
    shared->profile = apvts.getRawParameterValue("profile");
    shared->sensitivityDb = apvts.getRawParameterValue("sens");
    shared->speedDbPerS = apvts.getRawParameterValue("speed");
    shared->measPeakMode = apvts.getRawParameterValue("peakmode");
    shared->targetParam = apvts.getParameter("target");
    shared->trimParam = apvts.getParameter("trim");
    shared->automationParam = apvts.getParameter("automation");
    shared->profileParam = apvts.getParameter("profile");
    shared->sensParam = apvts.getParameter("sens");
    shared->speedParam = apvts.getParameter("speed");
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
    resetAgcState();
    measFloor.reset();

    for (int ch = 0; ch < 2; ++ch)
    {
        lufsShelf[ch] = dsp::makeKShelf(sr);
        lufsHighpass[ch] = dsp::makeKHighpass(sr);
        drumDetectHpf[ch] = dsp::makeHighpass(sr, dsp::kDrumDetectHpfHz);
    }
    for (int i = 0; i < dsp::kLufsSlots; ++i)
    {
        lufsSlotSumSq[i] = 0.0;
        lufsSlotSamples[i] = 0;
    }
    lufsSlotIndex = 0;
    lufsSlotElapsed = 0.0f;
}

// Short-term LUFS (BS.1770): K-weighted mean square over a 3 s window,
// channels summed. Analysis only — the buffer is never modified.
void AutoTrimProcessor::analyzeLoudness(const juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(2, buffer.getNumChannels());

    double sumSq = 0.0;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = buffer.getReadPointer(ch);
        auto& shelf = lufsShelf[ch];
        auto& highpass = lufsHighpass[ch];
        for (int i = 0; i < numSamples; ++i)
        {
            const float y = highpass.process(shelf.process(data[i]));
            sumSq += (double) y * (double) y;
        }
    }

    lufsSlotSumSq[lufsSlotIndex] += sumSq;
    lufsSlotSamples[lufsSlotIndex] += numSamples;
    lufsSlotElapsed += (float) numSamples / (float) currentSampleRate;
    while (lufsSlotElapsed >= dsp::kLufsSlotS)
    {
        lufsSlotElapsed -= dsp::kLufsSlotS;
        lufsSlotIndex = (lufsSlotIndex + 1) % dsp::kLufsSlots;
        lufsSlotSumSq[lufsSlotIndex] = 0.0;
        lufsSlotSamples[lufsSlotIndex] = 0;
    }

    double totalSq = 0.0;
    long totalSamples = 0;
    for (int i = 0; i < dsp::kLufsSlots; ++i)
    {
        totalSq += lufsSlotSumSq[i];
        totalSamples += lufsSlotSamples[i];
    }
    float lufs = dsp::kLufsFloorDb;
    if (totalSamples > 0 && totalSq > 0.0)
        lufs = juce::jmax(dsp::kLufsFloorDb,
                          -0.691f + 10.0f * (float) std::log10(totalSq / (double) totalSamples));
    shared->lufsShort.store(lufs);
}

void AutoTrimProcessor::resetHitState()
{
    riderHold.reset();
    hitDetector.reset();
    hitHistoryCount = 0;
    hitHistoryPos = 0;
    sinceHitS = 1000.0f;
    sinceCorrectionS = 1000.0f;
    clipGuard.resetWindow();
}

void AutoTrimProcessor::resetAgcState()
{
    agcObserver.reset();
    agcBail.reset();
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

    // Liveness stamp for the UI meters (so a stopped transport drains the bar
    // instead of freezing it on a peak).
    shared->lastProcessMs.store(juce::Time::getMillisecondCounterHiRes());

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
        if (shared->agcOffsetDb.load() != 0.0f)
            shared->agcOffsetDb.store(0.0f);
        if (shared->protectOffsetDb.load() != 0.0f || shared->protectionActive.load())
        {
            shared->protectOffsetDb.store(0.0f);
            shared->protectionActive.store(false);
        }
        // Analysis only: the master mix passes through untouched.
        analyzeLoudness(buffer);
        return;
    }

    const float maxTrim = registry::maxTrimDb.load();
    const bool automationOn = shared->isAutomationOn();
    const bool riderOn = automationOn && shared->riderOn->load() > 0.5f;
    const bool agcOn = automationOn && shared->agcOn->load() > 0.5f;
    const bool clipGuardOn = automationOn && shared->clipGuardOn->load() > 0.5f;
    const bool measuring = shared->measuring.load();
    // Measurement algorithm for this channel: average (default) vs highest peak.
    const bool peakMode = shared->measPeakMode->load() > 0.5f;
    const float trimDb = shared->trimDb->load();
    const auto& profile = dsp::profileFor((int) shared->profile->load());
    const float sensDb = shared->sensitivityDb->load();
    const float sensLin = dsp::dbToGain(sensDb);
    const bool detectHits = riderOn && ! measuring && profile.hitBased;
    // Drum measurement counts hits (the rider's hit machinery is idle then).
    const bool countMeasHits = measuring && profile.hitBased;
    const float measGateLin = dsp::dbToGain(dsp::kGateDb);
    // Measurement arms at the channel's own sensitivity threshold (never
    // below the no-signal gate): the per-channel knob that rejects bleed.
    // Cross-channel dominance was tried and reverted — pre-trim levels are
    // not comparable across preamps, so a quiet channel could never win.
    const float measArmLin = juce::jmax(sensLin, measGateLin);
    // Drum measurement arms relative to the channel's own recent floor
    // (bleed + noise), not a fixed dBFS number: change the preamp gain
    // upstream and both the floor and the hits move together, so the margin
    // above it keeps meaning the same thing. Never below the no-signal gate,
    // so a channel that's genuinely silent still reads as silent.
    const float measArmLinDrum =
        juce::jmax(dsp::dbToGain(measFloor.thresholdDb(dsp::kDrumArmMarginDb)), measGateLin);
    bool hitCompleted = false;

    float offsetDb = shared->riderOffsetDb.load();
    const float agcDb = shared->agcOffsetDb.load();
    const float protectDb = shared->protectOffsetDb.load();
    float totalDb = juce::jlimit(-maxTrim, maxTrim, trimDb + offsetDb + agcDb + protectDb);
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
            if (const auto hit =
                    hitDetector.step(framePeak, sensLin, hitWindowSamples, hitRetriggerSamples))
            {
                hitHistoryDb[hitHistoryPos] = dsp::gainToDb(*hit);
                hitHistoryPos = (hitHistoryPos + 1) % dsp::kHitHistory;
                hitHistoryCount = juce::jmin(hitHistoryCount + 1, dsp::kHitHistory);
                hitCompleted = true;
            }
        }
        else if (countMeasHits)
        {
            // High-passed detection signal: recovers real separation between
            // a direct hit and broadband kit bleed on the same mic (see
            // dsp::makeHighpass). The captured LEVEL still comes from the raw
            // signal (levelPeak = framePeak below) — only the arm/threshold
            // decision uses the filtered view.
            float detectPeak = 0.0f;
            for (int ch = 0; ch < numChannels && ch < 2; ++ch)
                detectPeak =
                    juce::jmax(detectPeak, std::abs(drumDetectHpf[ch].process(channelData[ch][i])));
            if (const auto hit = hitDetector.step(detectPeak, framePeak, measArmLinDrum,
                                                  hitWindowSamples, hitRetriggerSamples))
            {
                // Each hit's peak enters the gated average: hits far below
                // the strong ones (bleed, ghost notes) are dropped so they
                // never drag the measured level down. Peak mode instead keeps
                // the single loudest hit.
                measPeaksDb[measCount % dsp::kMeasMaxHits] = dsp::gainToDb(*hit);
                ++measCount;
                measPeakMaxLin = juce::jmax(measPeakMaxLin, *hit);
                shared->measuredPeak.store(
                    peakMode ? measPeakMaxLin
                             : dsp::dbToGain(dsp::gatedHitAverageDb(
                                 measPeaksDb, juce::jmin(measCount, dsp::kMeasMaxHits))));
                shared->measHitCount.store((uint32_t) measCount);
            }
            if (hitDetector.inHit && ! measStartedLocal)
            {
                measStartedLocal = true;
                shared->measStarted.store(true);
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

    // Runs continuously (not just while measuring), so it's already
    // converged by the time a drum measurement starts instead of ramping up
    // from a cold start.
    measFloor.step(dsp::gainToDb(blockPeak), dt);

    // Measurement window: this thread is the only writer of measuredPeak; the
    // epoch bump tells us the panel started a new run. Armed: the window only
    // starts counting when signal first crosses the gate, so sources that
    // play at isolated moments (toms) still get a full-window capture.
    const uint32_t epoch = shared->measEpoch.load();
    if (epoch != measEpoch)
    {
        measEpoch = epoch;
        measAverager.reset();
        measPeakMaxLin = 0.0f;
        measCount = 0;
        measStartedLocal = false;
        measBudgetArmed = false;
        measArmedS = 0.0f;
        hitDetector.reset();
        drumDetectHpf[0].reset();
        drumDetectHpf[1].reset();
        // A fresh measurement recalibrates the trim: AGC evidence restarts.
        resetAgcState();
    }
    // Clip Guard: if the *output* peaks past 0 dBFS (real clipping) too many
    // times inside the window, cut the trim to protect the downstream chain —
    // regardless of the rider being on. A sustained overload re-arms one
    // event per rearm period so it is caught too.
    if (clipGuardOn && ! measuring)
    {
        if (const auto newCut = clipGuard.step(blockPeakPost, shared->peakPostTrim.load(),
                                               shared->targetDb->load(), protectDb, dt))
        {
            shared->protectOffsetDb.store(*newCut);
            shared->protectionActive.store(*newCut < 0.0f);
        }
    }

    // Clip Guard off: a residual protective cut must not keep acting.
    if (! clipGuardOn
        && (shared->protectOffsetDb.load() != 0.0f || shared->protectionActive.load()))
    {
        shared->protectOffsetDb.store(0.0f);
        shared->protectionActive.store(false);
        clipGuard.resetWindow();
    }

    // A stale rider correction must not keep acting after the mode is off.
    if (! riderOn && offsetDb != 0.0f)
    {
        shared->riderOffsetDb.store(0.0f);
        resetHitState();
    }

    if (measuring)
    {
        // Publishes the final measured level and closes the window. In average
        // mode it applies the frequent-peak protection: when loud peaks are
        // frequent (the mean undersells the material, gain would run hot) the
        // level is pulled back so the loudest peak lands at +3 dB over target;
        // a rare stray peak keeps the mean. Drum peak mode instead corroborates
        // the max against the rest of the capture (a lone outlier hit — mic
        // bump, stick on the rim — is rejected in favor of the next loudest).
        // Continuous peak mode is already calibrated to the loudest block, so
        // it's left as-is.
        const auto closeMeasurement = [&]
        {
            if (profile.hitBased && peakMode)
            {
                const int n = juce::jmin(measCount, dsp::kMeasMaxHits);
                if (n > 0)
                    shared->measuredPeak.store(dsp::dbToGain(dsp::corroboratedPeakDb(measPeaksDb, n)));
            }
            else if (! peakMode)
            {
                const float meanDb = dsp::gainToDb(shared->measuredPeak.load());
                const float level = dsp::peakLimitedLevelDb(
                    meanDb, measPeaksDb, juce::jmin(measCount, dsp::kMeasMaxHits),
                    registry::peakFrequentPct.load() / 100.0f);
                shared->measuredPeak.store(dsp::dbToGain(level));
            }
            shared->measuring.store(false);
            shared->measDone.store(true);
        };

        if (profile.hitBased)
        {
            // Percussive sources close on a LISTENING WINDOW (registry::
            // measDrumWindowS), not a hit count: a source like a tom can
            // enter a song late, so waiting a fixed number of seconds is
            // what gives it time to be struck at all, rather than closing
            // on kit bleed before it ever is. Hits are detected and averaged
            // (with the bleed gate) in the sample loop above; here we just
            // decide when to stop listening. "Concluir agora" (a channel or
            // panel-row button) ends it early with whatever was captured.
            measArmedS += dt;
            const bool finishNow = shared->measFinishNow.exchange(false);
            if (finishNow || measArmedS >= registry::measDrumWindowS.load())
                closeMeasurement();
        }
        else
        {
            // Sustained instruments measure a seconds window that only counts
            // *programme* time: a block below the sensitivity pauses it instead
            // of burning through the budget, so a source with gaps still gets a
            // full window of real signal (this is what lets it be short).
            if (! measStartedLocal && blockPeak > measArmLin)
            {
                measStartedLocal = true;
                shared->measStarted.store(true);
            }
            if (measStartedLocal)
            {
                if (! measBudgetArmed)
                {
                    measBudgetArmed = true;
                    measBudget.arm(registry::measDurationS.load(), currentSampleRate);
                }

                // Average mode: mean of 0.5 s slot peaks above the arm
                // threshold — the trim calibrates the typical peak; each slot
                // peak is also kept for the sporadic-peak check. Peak mode:
                // the single loudest block instead (updated every block).
                if (peakMode)
                {
                    if (blockPeak > measArmLin)
                    {
                        measPeakMaxLin = juce::jmax(measPeakMaxLin, blockPeak);
                        shared->measuredPeak.store(measPeakMaxLin);
                    }
                }
                else if (const auto res = measAverager.step(blockPeak, dt, measArmLin))
                {
                    measPeaksDb[measCount % dsp::kMeasMaxHits] = res->slotPeakDb;
                    ++measCount;
                    shared->measuredPeak.store(dsp::dbToGain(res->avgDb));
                }

                if (measBudget.step(numSamples, blockPeak > measArmLin))
                    closeMeasurement();
            }
        }
    }
    else if (riderOn)
    {
        // Continuous rider: block-rate adjustment of the internal offset,
        // confined to the profile's ride range around the measured trim (the
        // trim parameter itself is only written from the message thread).
        const float target = shared->targetDb->load();
        // The editable speed is the ride-up rate; ride-down keeps the
        // profile's down/up ratio (attenuation always faster than boost).
        const float upRate = shared->speedDbPerS->load();
        const float downRate =
            upRate * (profile.downDbPerS / juce::jmax(0.1f, profile.upDbPerS));
        float newOffset = offsetDb;

        if (! profile.hitBased)
        {
            // Sliding peak-hold: coarse 100 ms slots over the profile window.
            const int numSlots = juce::jlimit(
                1, dsp::kMaxHoldSlots, (int) (profile.holdS / dsp::kHoldSlotS));
            const float holdMax = riderHold.step(blockPeak, dt, numSlots);
            const float levelDb = dsp::gainToDb(holdMax);
            // The rider rides around the AGC-corrected base, so the two
            // loops close on the same output and never fight.
            const float baseDb = trimDb + agcDb;
            const bool hasProgram =
                levelDb >= sensDb
                && dsp::riderSeesProgram(levelDb, baseDb, offsetDb, target, profile);
            newOffset = hasProgram
                            ? dsp::riderOffsetStep(offsetDb, levelDb, baseDb, target, profile,
                                                   dt, upRate, downRate)
                            : dsp::riderIdleStep(offsetDb, profile, dt);
        }
        else
        {
            sinceHitS += dt;
            sinceCorrectionS += dt;
            if (hitCompleted)
            {
                sinceHitS = 0.0f;
                // Gated average of the recent hits, same as the measurement:
                // a plain mean would be dragged down by bleed and ghost notes
                // (weak "hits" from other drums), making the rider think the
                // level is low and boost until the real hits blow past the
                // target. The gate drops hits far below the strong ones.
                const float levelDb = dsp::gatedHitAverageDb(hitHistoryDb, hitHistoryCount);
                newOffset = dsp::riderOffsetStep(
                    offsetDb, levelDb, trimDb + agcDb, target, profile,
                    juce::jmin(sinceCorrectionS, dsp::kHitCorrectionMaxDtS), upRate, downRate);
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

    // AGC: observes all the time and, when the program level stays shifted
    // from the target for the full hold period, corrects the gain in one
    // step, like a re-measurement (not a rider). The correction lives in
    // agcOffsetDb — separate from the measured trim, shown in yellow — and
    // 3 s of silence resets it, returning the channel to the measured gain.
    if (! agcOn)
    {
        if (agcDb != 0.0f)
            shared->agcOffsetDb.store(0.0f);
        // Unconditional: guarantees a fresh window and evidence on re-enable.
        resetAgcState();
    }
    else if (! measuring)
    {
        // Sudden-loud bail-out: any meaningful boost with the output
        // *sustained* past target + 5 dB — a surprise guitar solo — proves
        // the measured calibration was right. Accumulated over-time (gap
        // tolerant) fires within a second on a picked solo but survives a
        // lone spike. The rider offset is zeroed too: it was tracking the
        // boosted output, and keeping it would bury the solo below the
        // target for seconds while it re-converges at the slow up-rate.
        if (agcDb > dsp::kAgcBailMinBoostDb)
        {
            const bool over =
                blockPeakPost
                > dsp::dbToGain(shared->targetDb->load() + dsp::kAgcBailMarginDb);
            if (agcBail.step(over, dt))
            {
                shared->agcOffsetDb.store(0.0f);
                shared->riderOffsetDb.store(0.0f);
                agcObserver.resetEvidence();
            }
        }
        else
        {
            agcBail.reset();
        }

        // Pre-rider, pre-Clip Guard: the AGC judges the *base* calibration
        // (trim + its own pending correction). The rider's temporary
        // correction would mask exactly the permanent shifts the AGC exists
        // to fix, and a protective cut is intentional attenuation, not
        // calibration.
        const float agcRange = shared->agcRangeDb->load();
        const auto action = agcObserver.step(blockPeak, dt, sensDb, trimDb + agcDb,
                                             shared->targetDb->load(),
                                             shared->agcHoldS->load(), agcRange);
        // 3 s below the sensitivity: the channel returns to the measured
        // gain (the AGC correction is temporary by design).
        if (action.silenceReset && shared->agcOffsetDb.load() != 0.0f)
            shared->agcOffsetDb.store(0.0f);
        if (action.correctionDb)
            shared->agcOffsetDb.store(
                juce::jlimit(-agcRange, agcRange, agcDb + *action.correctionDb));
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
    state.setProperty("order", shared->order.load(), nullptr);
    // Global panel settings ride along with every instance, but only a panel
    // instance restores them, so a channel's stale copy never wins.
    state.setProperty("maxTrimDb", registry::maxTrimDb.load(), nullptr);
    state.setProperty("measDurationS", registry::measDurationS.load(), nullptr);
    state.setProperty("measDrumWindowS", registry::measDrumWindowS.load(), nullptr);
    state.setProperty("peakFrequentPct", registry::peakFrequentPct.load(), nullptr);

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
    shared->order.store((int) state.getProperty("order", (int) shared->id));
    if (panelMode)
    {
        registry::maxTrimDb.store((float) (double) state.getProperty(
            "maxTrimDb", (double) dsp::kDefaultMaxTrimDb));
        registry::measDurationS.store((float) (double) state.getProperty(
            "measDurationS", (double) dsp::kDefaultMeasDurationS));
        registry::measDrumWindowS.store((float) (double) state.getProperty(
            "measDrumWindowS", (double) dsp::kDrumWindowDefaultS));
        registry::peakFrequentPct.store(
            (int) state.getProperty("peakFrequentPct", dsp::kDefaultPeakFrequentPct));
    }
}
} // namespace autotrim

// JUCE plugin entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new autotrim::AutoTrimProcessor();
}
