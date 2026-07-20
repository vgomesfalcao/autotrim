// autotrim-sim: offline harness that runs the REAL AutoTrimProcessor over a
// WAV file (a recorded live channel) and reports the gain trajectory —
// measurement, rider, AGC, Clip Guard — flagging bumps, overs and every
// automatic action with a timestamp. This is how recorded shows become
// regression tests for "no sudden loud bumps".
#include "Measurement.h"
#include "PluginProcessor.h"
#include "Presets.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdio>
#include <deque>

using namespace autotrim;

namespace
{
juce::String formatTime(double seconds)
{
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    return juce::String::formatted("%02d:%04.1f", m, s);
}

void logEvent(double t, const juce::String& text)
{
    std::printf("[%s] %s\n", formatTime(t).toRawUTF8(), text.toRawUTF8());
}

struct Options
{
    juce::String file;
    float targetDb = dsp::kDefaultTargetDb;
    int profile = 1; // Instrumento
    float sensDb = -1000.0f; // sentinel: use the profile default
    bool rider = false;
    bool agc = false;
    float agcTimeS = dsp::kAgcHoldS;
    float agcRangeDb = dsp::kAgcRangeDb;
    float measS = dsp::kDefaultMeasDurationS;
    juce::String csvPath;
};
} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg(argv[i]);
        auto next = [&]() -> juce::String
        { return i + 1 < argc ? juce::String(argv[++i]) : juce::String(); };
        if (arg == "--target")
            opt.targetDb = next().getFloatValue();
        else if (arg == "--profile")
        {
            const auto p = next().toLowerCase();
            opt.profile = p.startsWith("voz") ? 0 : p.startsWith("bat") ? 2 : 1;
        }
        else if (arg == "--sens")
            opt.sensDb = next().getFloatValue();
        else if (arg == "--rider")
            opt.rider = true;
        else if (arg == "--agc")
            opt.agc = true;
        else if (arg == "--agctime")
            opt.agcTimeS = next().getFloatValue();
        else if (arg == "--agcrange")
            opt.agcRangeDb = next().getFloatValue();
        else if (arg == "--meas")
            opt.measS = next().getFloatValue();
        else if (arg == "--csv")
            opt.csvPath = next();
        else if (! arg.startsWith("-"))
            opt.file = arg;
    }

    if (opt.file.isEmpty())
    {
        std::printf(
            "uso: autotrim-sim arquivo.wav [--target dB] [--profile voz|instrumento|bateria]\n"
            "                 [--sens dB] [--rider] [--agc] [--agctime s] [--agcrange dB]\n"
            "                 [--meas s] [--csv saida.csv]\n");
        return 1;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File inputFile(juce::File::getCurrentWorkingDirectory().getChildFile(opt.file));
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(inputFile));
    if (reader == nullptr)
    {
        std::printf("erro: não consegui ler %s\n", opt.file.toRawUTF8());
        return 1;
    }

    const double sr = reader->sampleRate;
    const auto totalSamples = (juce::int64) reader->lengthInSamples;
    const double totalS = (double) totalSamples / sr;
    const auto& profile = dsp::kProfiles[opt.profile];
    const char* profileNames[] = { "Voz", "Instrumento", "Bateria" };

    std::printf("== autotrim-sim ==\n");
    std::printf("arquivo: %s (%.0f Hz, %d canais, %s)\n", opt.file.toRawUTF8(), sr,
                (int) reader->numChannels, formatTime(totalS).toRawUTF8());
    std::printf("config: target %.1f dBFS, perfil %s, rider %s, AGC %s (tempo %.0f s, máx "
                "%.0f dB), medição %.0f s\n\n",
                opt.targetDb, profileNames[opt.profile], opt.rider ? "ON" : "off",
                opt.agc ? "ON" : "off", opt.agcTimeS, opt.agcRangeDb, opt.measS);

    AutoTrimProcessor proc;
    presets::writeParam(proc.apvts.getParameter("target"), opt.targetDb);
    presets::writeParam(proc.apvts.getParameter("profile"), (float) opt.profile);
    presets::writeParam(proc.apvts.getParameter("sens"),
                        opt.sensDb > -999.0f ? opt.sensDb : profile.sensitivityDb);
    presets::writeParam(proc.apvts.getParameter("rider"), opt.rider ? 1.0f : 0.0f);
    presets::writeParam(proc.apvts.getParameter("agc"), opt.agc ? 1.0f : 0.0f);
    presets::writeParam(proc.apvts.getParameter("agctime"), opt.agcTimeS);
    presets::writeParam(proc.apvts.getParameter("agcrange"), opt.agcRangeDb);
    registry::measDurationS.store(opt.measS);

    const int blockSize = 512;
    proc.prepareToPlay(sr, blockSize);
    measurement::startChannel(proc.shared, opt.measS);
    logEvent(0.0, utf8("medição armada (aguardando sinal…)"));

    std::unique_ptr<juce::FileOutputStream> csv;
    if (opt.csvPath.isNotEmpty())
    {
        auto csvFile = juce::File::getCurrentWorkingDirectory().getChildFile(opt.csvPath);
        csvFile.deleteFile();
        csv = csvFile.createOutputStream();
        if (csv != nullptr)
            csv->writeText("t;inPeakDb;outPeakDb;trim;rider;agc;clip\n", false, false, nullptr);
    }

    juce::AudioBuffer<float> block(2, blockSize);
    juce::MidiBuffer midi;

    // Trajectory metrics
    const int bumpWindowBlocks = juce::jmax(2, (int) (0.1 * sr / blockSize) + 1);
    std::deque<float> trimWindow;
    bool measWasRunning = true, measStartedSeen = false;
    double measDoneT = -1000.0;
    float prevAgc = 0.0f, prevClip = 0.0f;
    float worstSwing = 0.0f;
    double worstSwingT = 0.0;
    int bumpCount = 0;
    double lastBumpT = -1000.0, lastAgcEventT = -1000.0;
    double overClipS = 0.0, overTargetS = 0.0, programS = 0.0;
    double lastOutOverTargetT = -1000.0;
    float maxOutDb = -200.0f;

    for (juce::int64 pos = 0; pos < totalSamples; pos += blockSize)
    {
        const int n = (int) juce::jmin((juce::int64) blockSize, totalSamples - pos);
        block.clear();
        reader->read(&block, 0, n, pos, true, true);
        if (n < blockSize)
            block.setSize(2, n, true, true, true);

        const float inPeak = block.getMagnitude(0, n);
        proc.processBlock(block, midi);
        measurement::poll();

        const double t = (double) pos / sr;
        const double dt = (double) n / sr;
        const float outPeak = block.getMagnitude(0, n);
        const float outDb = dsp::gainToDb(outPeak);
        maxOutDb = juce::jmax(maxOutDb, outDb);
        if (outPeak > 1.0f)
            overClipS += dt;
        if (outDb > opt.targetDb + dsp::kAgcBailMarginDb)
        {
            overTargetS += dt;
            lastOutOverTargetT = t;
        }
        if (dsp::gainToDb(inPeak) >= dsp::kGateDb)
            programS += dt;

        // Measurement lifecycle
        if (! measStartedSeen && proc.shared->measStarted.load())
        {
            measStartedSeen = true;
            logEvent(t, utf8("medição iniciou (sinal chegou)"));
        }
        if (measWasRunning && ! measurement::isRunning())
        {
            measWasRunning = false;
            measDoneT = t;
            if (proc.shared->noSignal.load())
                logEvent(t, utf8("medição terminou SEM SINAL (canal intocado)"));
            else
                logEvent(t, utf8("medição concluída: nível medido ")
                                 + juce::String(dsp::gainToDb(proc.shared->measuredPeak.load()), 1)
                                 + utf8(" dBFS -> Ganho ")
                                 + juce::String(proc.shared->trimDb->load(), 1) + " dB");
        }

        // AGC actions
        const float agcNow = proc.shared->agcOffsetDb.load();
        if (std::abs(agcNow - prevAgc) > 0.5f)
        {
            lastAgcEventT = t;
            if (agcNow == 0.0f && prevAgc > dsp::kAgcBailMinBoostDb
                && t - lastOutOverTargetT < 1.0)
                logEvent(t, utf8("AGC bail-out: boost de +") + juce::String(prevAgc, 1)
                                 + utf8(" dB zerado (fonte alta sustentada)"));
            else if (agcNow == 0.0f)
                logEvent(t, utf8("AGC resetou (silêncio): era ")
                                 + juce::String(prevAgc, 1) + " dB");
            else
                logEvent(t, utf8("AGC corrigiu: ") + juce::String(prevAgc, 1) + " -> "
                                 + juce::String(agcNow, 1) + " dB");
        }
        prevAgc = agcNow;

        // Clip Guard actions
        const float clipNow = proc.shared->protectOffsetDb.load();
        if (clipNow < -0.05f && prevClip >= -0.05f)
            logEvent(t, utf8("CLIP GUARD cortou ") + juce::String(clipNow, 1) + " dB");
        else if (clipNow >= -0.05f && prevClip < -0.05f)
            logEvent(t, utf8("clip guard liberou o corte"));
        prevClip = clipNow;

        // Bump detection: worst effective-gain swing inside a 100 ms window.
        trimWindow.push_back(proc.shared->effectiveTrimDb());
        if ((int) trimWindow.size() > bumpWindowBlocks)
            trimWindow.pop_front();
        float lo = trimWindow.front(), hi = trimWindow.front();
        for (float v : trimWindow)
        {
            lo = juce::jmin(lo, v);
            hi = juce::jmax(hi, v);
        }
        const float swing = hi - lo;
        // A bump is a fast *rise*: fast drops are protection by design (bail,
        // Clip Guard) and already logged as their own events. Rises explained
        // by a just-logged event (measurement apply, AGC step) don't count —
        // what's left is genuinely unexplained gain movement, i.e. a bug.
        const float rise = trimWindow.back() - lo;
        const bool explained = t - measDoneT < 0.3 || t - lastAgcEventT < 0.3;
        if (swing > worstSwing)
        {
            worstSwing = swing;
            worstSwingT = t;
        }
        if (rise > 3.0f && ! explained && t - lastBumpT > 1.0)
        {
            ++bumpCount;
            lastBumpT = t;
            logEvent(t, utf8("BUMP: ganho subiu ") + juce::String(rise, 1)
                             + utf8(" dB em 100 ms sem evento que explique"));
        }

        if (csv != nullptr)
            csv->writeText(juce::String(t, 3) + ";" + juce::String(dsp::gainToDb(inPeak), 2)
                               + ";" + juce::String(outDb, 2) + ";"
                               + juce::String(proc.shared->trimDb->load(), 2) + ";"
                               + juce::String(proc.shared->riderOffsetDb.load(), 2) + ";"
                               + juce::String(agcNow, 2) + ";" + juce::String(clipNow, 2)
                               + "\n",
                           false, false, nullptr);
    }

    std::printf("\nresumo:\n");
    std::printf("  pico máximo de saída: %.1f dBFS\n", maxOutDb);
    std::printf("  maior variação de ganho em 100 ms: %.1f dB em %s%s\n", worstSwing,
                formatTime(worstSwingT).toRawUTF8(),
                worstSwingT - measDoneT < 0.3 && measDoneT >= 0.0
                    ? utf8(" (aplicação da medição — esperado)").toRawUTF8()
                    : "");
    std::printf("  bumps (subida >3 dB em 100 ms sem evento que explique): %d\n", bumpCount);
    std::printf("  tempo com saída acima de target+5: %.1f s (%.1f%% do programa)\n",
                overTargetS, programS > 0.0 ? 100.0 * overTargetS / programS : 0.0);
    std::printf("  tempo acima de 0 dBFS (clip): %.2f s\n", overClipS);
    std::printf("  offsets finais: rider %.1f, AGC %.1f, clip %.1f dB\n",
                proc.shared->riderOffsetDb.load(), proc.shared->agcOffsetDb.load(),
                proc.shared->protectOffsetDb.load());
    const bool ok = bumpCount == 0 && overClipS < 0.01;
    std::printf("veredito: %s\n",
                ok ? "OK — sem bumps nem clipping"
                   : utf8("ATENÇÃO — revisar eventos acima").toRawUTF8());
    return ok ? 0 : 2;
}
