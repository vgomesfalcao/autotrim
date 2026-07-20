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
