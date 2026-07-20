// Unit tests for the pure DSP math. No framework: plain asserts, exits
// non-zero on the first failure.
#include "Dsp.h"

#include <cstdio>
#include <cstdlib>

using namespace autotrim::dsp;

static int failures = 0;

#define CHECK(cond)                                                        \
    do                                                                     \
    {                                                                      \
        if (! (cond))                                                      \
        {                                                                  \
            std::printf("FALHOU: %s (linha %d)\n", #cond, __LINE__);       \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

static bool approx(float a, float b, float eps = 1e-3f)
{
    return std::abs(a - b) < eps;
}

int main()
{
    // dB <-> gain roundtrip
    for (float db : { -24.0f, -6.0f, 0.0f, 6.0f, 24.0f })
        CHECK(approx(gainToDb(dbToGain(db)), db));
    CHECK(approx(dbToGain(0.0f), 1.0f));
    CHECK(approx(dbToGain(-6.0206f), 0.5f));

    // Trim reaches the target
    CHECK(approx(*computeTrimDb(-30.0f, -18.0f, 24.0f), 12.0f));
    CHECK(approx(*computeTrimDb(-6.0f, -18.0f, 24.0f), -12.0f));

    // Clamp honors the panel's configurable limit
    CHECK(approx(*computeTrimDb(-59.0f, -6.0f, 24.0f), 24.0f));
    CHECK(approx(*computeTrimDb(-59.0f, -6.0f, 12.0f), 12.0f));
    CHECK(approx(*computeTrimDb(0.0f, -40.0f, 6.0f), -6.0f));

    // No signal below the -60 dBFS gate; exactly at the gate still counts
    CHECK(! computeTrimDb(-60.1f, -18.0f, 24.0f).has_value());
    CHECK(! computeTrimDb(-90.0f, -18.0f, 24.0f).has_value());
    CHECK(computeTrimDb(-60.0f, -18.0f, 24.0f).has_value());

    const RiderProfile& voice = kProfiles[0];
    const RiderProfile& instrument = kProfiles[1];
    const RiderProfile& drums = kProfiles[2];

    // Rider slew is asymmetric per profile: big positive error moves at the
    // profile's up rate, big negative error at its down rate.
    CHECK(approx(riderOffsetStep(0.0f, -40.0f, 0.0f, -18.0f, voice, 1.0f), voice.upDbPerS));
    CHECK(approx(riderOffsetStep(0.0f, -6.0f, 0.0f, -30.0f, voice, 1.0f), -voice.downDbPerS));

    // The editable speed overrides the profile rates (up explicit, down at
    // the caller's ratio) without touching the ride range.
    CHECK(approx(riderOffsetStep(0.0f, -40.0f, 0.0f, -18.0f, voice, 1.0f, 5.0f, 15.0f), 5.0f));
    CHECK(approx(riderOffsetStep(0.0f, -6.0f, 0.0f, -30.0f, voice, 1.0f, 1.0f, 3.0f), -3.0f));
    CHECK(approx(riderOffsetStep(0.0f, -50.0f, 0.0f, -18.0f, voice, 10.0f, 15.0f, 45.0f),
                 voice.rideRangeDb));

    // The offset is confined to the profile's ride range around the measured
    // trim, regardless of how large the error is (option B).
    {
        float offset = 0.0f;
        for (int i = 0; i < 50; ++i)
            offset = riderOffsetStep(offset, -50.0f, 0.0f, -18.0f, voice, 1.0f);
        CHECK(approx(offset, voice.rideRangeDb));
        offset = 0.0f;
        for (int i = 0; i < 50; ++i)
            offset = riderOffsetStep(offset, -50.0f, 0.0f, -18.0f, instrument, 1.0f);
        CHECK(approx(offset, instrument.rideRangeDb));
    }

    // Rider converges near the target and holds: base trim +16, input -40,
    // target -18 => needed offset +6, settles at the deadband edge (+5).
    {
        float offset = 0.0f;
        for (int i = 0; i < 30; ++i)
            offset = riderOffsetStep(offset, -40.0f, 16.0f, -18.0f, voice, 1.0f);
        CHECK(approx(offset, 6.0f - kRiderToleranceDb));
        CHECK(approx(riderOffsetStep(offset, -40.0f, 16.0f, -18.0f, voice, 1.0f), offset));
    }

    // Inside the deadband the rider never touches a correct trim (the
    // "+0.4 dB creep" bug): output 0.4 dB under target stays untouched.
    CHECK(approx(riderOffsetStep(0.0f, -34.4f, 16.0f, -18.0f, voice, 1.0f), 0.0f));
    // Just outside it, correction happens but stops at the deadband edge.
    CHECK(approx(riderOffsetStep(0.0f, -36.5f, 16.0f, -18.0f, voice, 1.0f), 1.5f));

    // Program-presence window: with target -10 and trim +16 the program input
    // sits at -26. Levels near it count as program; a level 14 dB below
    // (bleed/pause above the sensitivity floor) must NOT be ridden up.
    CHECK(riderSeesProgram(-26.0f, 16.0f, 0.0f, -10.0f, voice));
    CHECK(riderSeesProgram(-33.0f, 16.0f, 0.0f, -10.0f, voice)); // 7 dB under: still program
    CHECK(! riderSeesProgram(-40.0f, 16.0f, 0.0f, -10.0f, voice)); // 14 dB under: pause
    CHECK(! riderSeesProgram(-45.0f, 16.0f, 6.0f, -10.0f, voice)); // deep bleed with offset

    // Idle glide returns the offset to the measured trim at the profile's
    // return rate, without overshooting zero.
    CHECK(approx(riderIdleStep(3.0f, voice, 1.0f), 3.0f - voice.returnDbPerS));
    CHECK(approx(riderIdleStep(-3.0f, voice, 1.0f), -3.0f + voice.returnDbPerS));
    CHECK(approx(riderIdleStep(0.2f, voice, 1.0f), 0.0f));

    // Drum profile is hit-based with a tighter range.
    CHECK(drums.hitBased);
    CHECK(! voice.hitBased && ! instrument.hitBased);
    CHECK(approx(riderOffsetStep(0.0f, -20.0f, 0.0f, -6.0f, drums, 10.0f), drums.rideRangeDb));

    // AGC re-trim correction: nothing within the tolerance, the full error
    // (clamped to the editable range) beyond it, and a pause-like drop is
    // never mistaken for a program change.
    const float agcR = kAgcRangeDb;
    CHECK(! agcCorrectionDb(-26.0f, 16.0f, -10.0f, agcR).has_value()); // on target
    CHECK(! agcCorrectionDb(-28.0f, 16.0f, -10.0f, agcR).has_value()); // within ±3
    CHECK(approx(*agcCorrectionDb(-32.0f, 16.0f, -10.0f, agcR), 6.0f));  // 6 dB quieter
    CHECK(approx(*agcCorrectionDb(-20.0f, 16.0f, -10.0f, agcR), -6.0f)); // 6 dB louder
    CHECK(! agcCorrectionDb(-60.0f, 16.0f, -10.0f, agcR).has_value()); // pause/bleed: hold
    CHECK(approx(*agcCorrectionDb(-2.0f, 16.0f, -10.0f, agcR), -agcR)); // clamped cut
    CHECK(approx(*agcCorrectionDb(-38.0f, 16.0f, -10.0f, agcR), agcR)); // clamped boost
    // A tighter range also tightens the pause guard (range + 3 dB).
    CHECK(! agcCorrectionDb(-38.0f, 16.0f, -10.0f, 6.0f).has_value());
    CHECK(approx(*agcCorrectionDb(-30.0f, 16.0f, -10.0f, 6.0f), 4.0f));

    // Gated hit average (drum measurement): bleed/ghost hits far below the
    // strong ones never drag the level down; one freak rimshot never becomes
    // the reference.
    {
        const float one[1] = { -12.0f };
        CHECK(approx(gatedHitAverageDb(one, 1), -12.0f));
        const float two[2] = { -8.0f, -9.0f };
        CHECK(approx(gatedHitAverageDb(two, 2), -8.5f));
        // Sparse toms: two direct hits + two bleed hits 15 dB down.
        const float toms[4] = { -10.0f, -25.0f, -11.0f, -26.0f };
        CHECK(approx(gatedHitAverageDb(toms, 4), -10.5f));
        // Dense source where bleed is the majority: P90 reference holds.
        float dense[50];
        for (int i = 0; i < 20; ++i)
            dense[i] = -10.0f;
        for (int i = 20; i < 50; ++i)
            dense[i] = -30.0f;
        CHECK(approx(gatedHitAverageDb(dense, 50), -10.0f));
        // One freak hot hit among 99 normal ones: everything is kept.
        float freak[100];
        freak[0] = 0.0f;
        for (int i = 1; i < 100; ++i)
            freak[i] = -12.0f;
        CHECK(approx(gatedHitAverageDb(freak, 100), -11.88f));
        // Regression: with exactly 8 hits the reference must not be the
        // loudest hit — a rimshot among 8 would exclude all the real ones.
        float eight[8];
        eight[0] = 0.0f;
        for (int i = 1; i < 8; ++i)
            eight[i] = -12.0f;
        CHECK(approx(gatedHitAverageDb(eight, 8), -10.5f));
    }

    // AGC bail-out detector: a sustained note fires fast, a picked solo
    // accumulates through its note gaps, a lone spike never fires, and
    // widely spaced spikes never accumulate.
    {
        AgcBailDetector bail;
        // Sustained note: 0.2 s of continuous over fires.
        bool fired = false;
        for (int i = 0; i < 4 && ! fired; ++i)
            fired = bail.step(true, 0.05f);
        CHECK(fired);

        // Lone 50 ms spike: never fires, and a long gap clears the memory.
        bail.reset();
        CHECK(! bail.step(true, 0.05f));
        for (int i = 0; i < 8; ++i)
            CHECK(! bail.step(false, 0.05f));
        CHECK(approx(bail.overAccumS, 0.0f));

        // Picked solo: 50 ms peaks with 200 ms gaps accumulate and fire.
        bail.reset();
        fired = false;
        for (int note = 0; note < 6 && ! fired; ++note)
        {
            fired = bail.step(true, 0.05f);
            for (int i = 0; i < 4 && ! fired; ++i)
                bail.step(false, 0.05f);
        }
        CHECK(fired);

        // Spikes 0.5 s apart: the gap reset always wins, never fires.
        bail.reset();
        fired = false;
        for (int spike = 0; spike < 10; ++spike)
        {
            fired = fired || bail.step(true, 0.05f);
            for (int i = 0; i < 10; ++i)
                fired = fired || bail.step(false, 0.05f);
        }
        CHECK(! fired);
    }

    // AGC observer state machine, driven slot by slot (0.5 s per step).
    // Setup: target -10, trim +16 => program calibrated at -26 pre-trim.
    {
        const float sens = -50.0f, target = -10.0f, applied = 16.0f;
        const float hold = 12.0f, range = 10.0f;
        AgcObserver obs;

        // Quiet section 6 dB under: fires exactly after the hold time
        // (24 slots) with the full +6 correction.
        int fired = -1;
        float corr = 0.0f;
        for (int i = 0; i < 30 && fired < 0; ++i)
        {
            const auto a = obs.step(dbToGain(-32.0f), 0.5f, sens, applied, target, hold, range);
            if (a.correctionDb)
            {
                fired = i;
                corr = *a.correctionDb;
            }
        }
        CHECK(fired == 23);
        CHECK(approx(corr, 6.0f));

        // Short gaps between phrases are bridged by the 3 s peak window:
        // every 3rd slot silent still fires on schedule.
        obs.reset();
        fired = -1;
        for (int i = 0; i < 30 && fired < 0; ++i)
        {
            const float peak = (i % 3 == 2) ? 0.0f : dbToGain(-32.0f);
            if (obs.step(peak, 0.5f, sens, applied, target, hold, range).correctionDb)
                fired = i;
        }
        CHECK(fired == 23);

        // Level back within tolerance restarts the evidence — and the 3 s
        // peak window keeps bridging that on-target moment for 6 more slots
        // before the evidence can start accumulating again.
        obs.reset();
        for (int i = 0; i < 20; ++i)
            obs.step(dbToGain(-32.0f), 0.5f, sens, applied, target, hold, range);
        obs.step(dbToGain(-26.0f), 0.5f, sens, applied, target, hold, range); // on target
        fired = -1;
        for (int i = 0; i < 28 && fired < 0; ++i)
            if (obs.step(dbToGain(-32.0f), 0.5f, sens, applied, target, hold, range)
                    .correctionDb)
                fired = i;
        CHECK(fired == -1); // 5 bridged slots + 23 accumulating (11.5 s) < hold
        CHECK(obs.step(dbToGain(-32.0f), 0.5f, sens, applied, target, hold, range)
                  .correctionDb.has_value());

        // Full 3 s below the sensitivity reads as silence (offset reset).
        obs.reset();
        for (int i = 0; i < 4; ++i)
            obs.step(dbToGain(-32.0f), 0.5f, sens, applied, target, hold, range);
        fired = -1;
        for (int i = 0; i < 6 && fired < 0; ++i)
            if (obs.step(0.0f, 0.5f, sens, applied, target, hold, range).silenceReset)
                fired = i;
        CHECK(fired == 5); // exactly when the window has fully emptied

        // Bleed far below the program (beyond range + pause margin): evidence
        // holds, never fires, never reads as silence.
        obs.reset();
        bool any = false;
        for (int i = 0; i < 40; ++i)
        {
            const auto a = obs.step(dbToGain(-45.0f), 0.5f, sens, applied, target, hold, range);
            any = any || a.correctionDb.has_value() || a.silenceReset;
        }
        CHECK(! any);

        // Sign flip restarts the evidence: loud after quiet fires a cut only
        // after its own full hold time.
        obs.reset();
        for (int i = 0; i < 20; ++i)
            obs.step(dbToGain(-32.0f), 0.5f, sens, applied, target, hold, range);
        fired = -1;
        for (int i = 0; i < 30 && fired < 0; ++i)
        {
            const auto a = obs.step(dbToGain(-18.0f), 0.5f, sens, applied, target, hold, range);
            if (a.correctionDb)
            {
                fired = i;
                corr = *a.correctionDb;
            }
        }
        CHECK(fired == 23);
        CHECK(approx(corr, -8.0f));
    }

    // Clip Guard state machine, driven block by block (dt = 0.1 s).
    // Target -10; an offending peak of +1 dBFS implies an -11 dB cut.
    {
        const float over = dbToGain(1.0f), quiet = dbToGain(-12.0f);
        const float target = -10.0f;

        // 5 short bursts 0.5 s apart (within the 3 s window): cut on the 5th.
        ClipGuard guard;
        float cut = 0.0f;
        int fires = 0;
        for (int burst = 0; burst < 5; ++burst)
        {
            if (const auto c = guard.step(over, quiet, target, cut, 0.1f))
            {
                cut = *c;
                ++fires;
            }
            for (int i = 0; i < 4; ++i)
                guard.step(quiet, quiet, target, cut, 0.1f);
        }
        CHECK(fires == 1);
        CHECK(approx(cut, -11.0f));

        // Only 4 bursts: never fires.
        guard = ClipGuard();
        fires = 0;
        for (int burst = 0; burst < 4; ++burst)
        {
            if (guard.step(over, quiet, target, 0.0f, 0.1f))
                ++fires;
            for (int i = 0; i < 4; ++i)
                guard.step(quiet, quiet, target, 0.0f, 0.1f);
        }
        CHECK(fires == 0);

        // Bursts 1 s apart: 5 events always span 4 s > window, never fires.
        guard = ClipGuard();
        fires = 0;
        for (int burst = 0; burst < 8; ++burst)
        {
            if (guard.step(over, quiet, target, 0.0f, 0.1f))
                ++fires;
            for (int i = 0; i < 9; ++i)
                guard.step(quiet, quiet, target, 0.0f, 0.1f);
        }
        CHECK(fires == 0);

        // Sustained over: re-arms one event per 0.3 s, fires within ~1.5 s.
        guard = ClipGuard();
        fires = 0;
        cut = 0.0f;
        for (int i = 0; i < 15 && fires == 0; ++i)
            if (const auto c = guard.step(over, quiet, target, cut, 0.1f))
            {
                cut = *c;
                ++fires;
            }
        CHECK(fires == 1);
        CHECK(approx(cut, -11.0f));

        // Release: quiet with headroom glides back at 0.5 dB/s only after
        // the 5 s hold; a hot recent output pauses the give-back.
        guard = ClipGuard();
        guard.sinceOverS = 0.0f; // as if an over just happened
        cut = -6.0f;
        bool released = false;
        for (int i = 0; i < 45; ++i)
            released = released
                       || guard.step(quiet, quiet, target, cut, 0.1f).has_value();
        CHECK(! released); // 4.5 s: hold not over yet
        std::optional<float> give;
        for (int i = 0; i < 10 && ! give; ++i)
            give = guard.step(quiet, quiet, target, cut, 0.1f);
        CHECK(give.has_value() && *give > cut && *give < 0.0f);
        CHECK(! guard.step(quiet, dbToGain(-0.5f), target, cut, 0.1f).has_value());
    }

    // Measurement budget: only counts down while the signal is above the arm
    // threshold — a paused source never burns through the window.
    {
        const int sr = 100; // 1 sample = 10 ms, easy to reason about
        MeasBudget budget;
        budget.arm(1.0f, sr); // 1 s = 100 samples

        // Continuous programme: exhausts in exactly 1 s.
        bool done = false;
        int samples = 0;
        for (int i = 0; i < 20 && ! done; ++i)
        {
            samples += 5;
            done = budget.step(5, true);
        }
        CHECK(done);
        CHECK(samples == 100);

        // Half programme, half silence: silence never counts, so it takes
        // twice the wall-clock samples to exhaust the same 100-sample budget.
        budget.arm(1.0f, sr);
        done = false;
        samples = 0;
        for (int i = 0; i < 200 && ! done; ++i)
        {
            const bool above = (i % 2) == 0;
            samples += 1;
            done = budget.step(1, above);
        }
        CHECK(done);
        CHECK(samples == 199); // 100 "above" steps interleaved with 99 silent

        // Pure silence: never exhausts, no matter how long.
        budget.arm(1.0f, sr);
        done = false;
        for (int i = 0; i < 10000 && ! done; ++i)
            done = budget.step(1, false);
        CHECK(! done);
    }

    // Hit detector: capture window, retrigger cooldown, threshold.
    {
        HitDetector det;
        const int window = 4, retrigger = 10;
        CHECK(! det.step(0.4f, 0.5f, window, retrigger).has_value()); // below thr
        CHECK(! det.inHit);
        CHECK(! det.step(0.6f, 0.5f, window, retrigger).has_value()); // hit starts
        CHECK(det.inHit);
        det.step(0.9f, 0.5f, window, retrigger);
        det.step(0.7f, 0.5f, window, retrigger);
        det.step(0.3f, 0.5f, window, retrigger);
        const auto hit = det.step(0.2f, 0.5f, window, retrigger); // window ends
        CHECK(hit.has_value() && approx(*hit, 0.9f));
        // Cooldown (retrigger - window = 6 samples): loud frames don't retrigger.
        for (int i = 0; i < 6; ++i)
        {
            CHECK(! det.step(0.9f, 0.5f, window, retrigger).has_value());
            CHECK(! det.inHit);
        }
        det.step(0.9f, 0.5f, window, retrigger);
        CHECK(det.inHit); // cooldown over: new hit
    }

    // Slot averager: mean dB of program slots; pause slots never enter.
    {
        SlotAverager avg;
        const float arm = dbToGain(-50.0f);
        // Two blocks per 0.5 s slot (dt 0.25).
        avg.step(dbToGain(-6.0f), 0.25f, arm);
        auto a = avg.step(dbToGain(-6.0f), 0.25f, arm);
        CHECK(a.has_value() && approx(*a, -6.0f));
        avg.step(dbToGain(-12.0f), 0.25f, arm);
        a = avg.step(dbToGain(-12.0f), 0.25f, arm);
        CHECK(a.has_value() && approx(*a, -9.0f)); // (-6 + -12) / 2
        // Pause slot: below the arm threshold, average untouched.
        avg.step(0.0f, 0.25f, arm);
        a = avg.step(0.0f, 0.25f, arm);
        CHECK(! a.has_value());
        avg.step(dbToGain(-12.0f), 0.25f, arm);
        a = avg.step(dbToGain(-12.0f), 0.25f, arm);
        CHECK(a.has_value() && approx(*a, -10.0f)); // (-6 -12 -12) / 3
    }

    // Rider peak-hold window: holds the recent peak for the window length,
    // then lets it fall out.
    {
        PeakHoldWindow hold;
        const int numSlots = 4; // 0.4 s window at 0.1 s slots
        CHECK(approx(hold.step(1.0f, 0.1f, numSlots), 1.0f));
        CHECK(approx(hold.step(0.0f, 0.1f, numSlots), 1.0f)); // still held
        CHECK(approx(hold.step(0.0f, 0.1f, numSlots), 1.0f));
        CHECK(approx(hold.step(0.0f, 0.1f, numSlots), 0.0f)); // fell out
    }

    // Envelope attacks faster than it releases
    const float a = onepoleCoef(kEnvAttackS, 48000.0f);
    const float r = onepoleCoef(0.300f, 48000.0f);
    float env = envelopeStep(0.0f, 1.0f, a, r);
    const float attacked = env;
    env = envelopeStep(env, 0.0f, a, r);
    CHECK(attacked > 0.0f);
    CHECK(env < attacked);
    CHECK(env > 0.0f); // release is gradual, not a reset

    if (failures == 0)
    {
        std::printf("todos os testes passaram\n");
        return EXIT_SUCCESS;
    }
    std::printf("%d falha(s)\n", failures);
    return EXIT_FAILURE;
}
