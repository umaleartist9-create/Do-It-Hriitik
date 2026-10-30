#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

namespace vp
{
//==============================================================================
// Simple power-of-two ring buffer with linear-interpolated fractional reads.
struct DelayBuf
{
    std::vector<float> b;
    int w = 0, mask = 0;

    void prepare (int minSize)
    {
        int s = 1;
        while (s < minSize) s <<= 1;
        b.assign ((size_t) s, 0.f);
        mask = s - 1;
        w = 0;
    }
    void push (float x) { w = (w + 1) & mask; b[(size_t) w] = x; }

    // delay = samples back from the most recently pushed sample
    float read (float delay) const
    {
        const float pos = (float) w - delay;
        const int i0 = (int) std::floor (pos);
        const float f = pos - (float) i0;
        return b[(size_t) (i0 & mask)] * (1.f - f) + b[(size_t) ((i0 + 1) & mask)] * f;
    }
};

//==============================================================================
// YIN pitch detector. push() returns true whenever a fresh estimate is ready.
struct PitchDetector
{
    static constexpr int win = 1024, bufSize = 4096, hop = 512;

    float frequency = 0.f; // 0 = unvoiced

    void prepare (double sr)
    {
        sampleRate = sr;
        tauMax = juce::jmin ((int) (sr / 70.0), 1000);
        tauMin = (int) (sr / 1000.0);
        buf.assign (bufSize, 0.f);
        lin.assign ((size_t) (win + tauMax + 1), 0.f);
        d.assign ((size_t) tauMax + 2, 0.f);
        pos = 0; count = 0; frequency = 0.f;
    }

    bool push (float x)
    {
        buf[(size_t) pos] = x;
        pos = (pos + 1) & (bufSize - 1);
        if (++count >= hop) { count = 0; analyse(); return true; }
        return false;
    }

private:
    void analyse()
    {
        const int n = win + tauMax;
        float energy = 0.f;
        for (int i = 0; i < n; ++i)
        {
            lin[(size_t) i] = buf[(size_t) ((pos - n + i) & (bufSize - 1))];
            if (i < win) energy += lin[(size_t) i] * lin[(size_t) i];
        }
        if (std::sqrt (energy / (float) win) < 0.004f) { frequency = 0.f; return; }

        // difference function
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            float sum = 0.f;
            for (int j = 0; j < win; ++j)
            {
                const float diff = lin[(size_t) j] - lin[(size_t) (j + tau)];
                sum += diff * diff;
            }
            d[(size_t) tau] = sum;
        }

        // cumulative mean normalised difference
        float running = 0.f;
        d[0] = 1.f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            running += d[(size_t) tau];
            d[(size_t) tau] = running > 0.f ? d[(size_t) tau] * (float) tau / running : 1.f;
        }

        int est = -1;
        for (int tau = juce::jmax (2, tauMin); tau < tauMax; ++tau)
        {
            if (d[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < tauMax && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau;
                est = tau;
                break;
            }
        }
        if (est < 0) { frequency = 0.f; return; }

        const float x0 = d[(size_t) est - 1], x1 = d[(size_t) est], x2 = d[(size_t) est + 1];
        const float den = x0 - 2.f * x1 + x2;
        const float shift = std::abs (den) > 1e-9f ? 0.5f * (x0 - x2) / den : 0.f;
        frequency = (float) sampleRate / ((float) est + shift);
    }

    double sampleRate = 44100.0;
    int tauMax = 600, tauMin = 40, pos = 0, count = 0;
    std::vector<float> buf, lin, d;
};

//==============================================================================
// Two-tap crossfading delay-line pitch shifter. The window length W is
// driven from the detected pitch period so both taps stay phase-coherent.
struct PitchShifter
{
    DelayBuf line;
    float phase = 0.f;

    void prepare (float startPhase) { line.prepare (16384); phase = startPhase; }

    float process (float in, float ratio, float W)
    {
        line.push (in);
        phase += (1.f - ratio) / W;
        phase -= std::floor (phase);
        float p2 = phase + 0.5f;
        if (p2 >= 1.f) p2 -= 1.f;
        return line.read (phase * W + 1.f) * hann (phase)
             + line.read (p2 * W + 1.f) * hann (p2);
    }

    float tap (float delay) const { return line.read (delay); }

    static float hann (float p) { return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * p); }
};

//==============================================================================
// Scale helpers (0 = chromatic, 1 = major, 2 = natural minor)
inline bool inScale (int midi, int key, int scale)
{
    static const bool tbl[3][12] = {
        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
        { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 },
        { 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0 } };
    return tbl[scale][((midi - key) % 12 + 12) % 12];
}

inline float quantise (float m, int key, int scale)
{
    float best = m, bd = 1e9f;
    const int c = (int) std::floor (m);
    for (int n = c - 2; n <= c + 3; ++n)
        if (inScale (n, key, scale))
        {
            const float dist = std::abs ((float) n - m);
            if (dist < bd) { bd = dist; best = (float) n; }
        }
    return best;
}

inline int stepInScale (int n, int steps, int key, int scale)
{
    const int dir = steps > 0 ? 1 : -1;
    int cnt = 0;
    while (cnt < std::abs (steps))
    {
        n += dir;
        if (inScale (n, key, scale)) ++cnt;
    }
    return n;
}
} // namespace vp

//==============================================================================
class VocalPolishProcessor : public juce::AudioProcessor
{
public:
    VocalPolishProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Vocal Polish"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void onPitch();

    double sr = 44100.0;

    // pitch engine
    vp::PitchDetector detector;
    vp::PitchShifter lead, harm[2];
    float targetShift = 0.f, curShift = 0.f;
    float targetInt[2] { 0.f, 0.f }, curInt[2] { 0.f, 0.f };
    float targetW = 1024.f, curW = 1024.f, gVoiced = 0.f;
    bool voiced = false;

    // cached per-block params used by onPitch()
    bool atOn = true;
    float atAmount = 1.f;
    int keyIdx = 0, scaleIdx = 0, harmChoice[2] { 0, 0 };

    // effects
    juce::dsp::Compressor<float> compressor;
    vp::DelayBuf dblA, dblB, echoL, echoR;
    float lfoA = 0.f, lfoB = 0.25f, lpL = 0.f, lpR = 0.f, tCur = 300.f;
    juce::Reverb reverb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalPolishProcessor)
};
