#include "PluginProcessor.h"

namespace
{
// scale-degree steps for each harmony choice (index 0 = off)
constexpr int kSteps[] = { 0, 2, -2, 4, -4, 5, 7, -7 };
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout VocalPolishProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    auto F = [&] (const char* id, const char* name, float lo, float hi, float def)
    {
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
                                                      NormalisableRange<float> (lo, hi), def));
    };
    auto C = [&] (const char* id, const char* name, StringArray choices, int def)
    {
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, choices, def));
    };
    auto B = [&] (const char* id, const char* name, bool def)
    {
        l.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def));
    };

    const StringArray harmChoices { "Off", "3rd Up", "3rd Down", "5th Up", "5th Down",
                                    "6th Up", "Octave Up", "Octave Down" };

    // Autotune
    B ("atOn",   "Tune On", true);
    C ("key",    "Tune Key", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0);
    C ("scale",  "Tune Scale", { "Chromatic", "Major", "Minor" }, 1);
    F ("speed",  "Tune Retune Speed", 0.f, 1.f, 0.6f);
    F ("amount", "Tune Amount", 0.f, 1.f, 1.f);

    // Harmony
    C ("h1",      "Harmony 1", harmChoices, 1);
    C ("h2",      "Harmony 2", harmChoices, 4);
    F ("hlevel",  "Harmony Level", 0.f, 1.f, 0.35f);
    F ("hspread", "Harmony Width", 0.f, 1.f, 0.7f);

    // Doubler
    F ("dblAmt",  "Doubler Amount", 0.f, 1.f, 0.f);
    F ("dblTime", "Doubler Time (ms)", 10.f, 40.f, 20.f);

    // Compressor
    F ("cthresh", "Comp Threshold (dB)", -40.f, 0.f, -18.f);
    F ("cratio",  "Comp Ratio", 1.f, 20.f, 3.f);
    F ("catk",    "Comp Attack (ms)", 1.f, 100.f, 10.f);
    F ("crel",    "Comp Release (ms)", 20.f, 500.f, 120.f);
    F ("makeup",  "Comp Makeup (dB)", 0.f, 24.f, 3.f);

    // Saturation
    F ("satDrive", "Saturation Drive", 0.f, 1.f, 0.2f);
    F ("satMix",   "Saturation Mix", 0.f, 1.f, 0.5f);

    // Delay
    F ("dlyTime", "Delay Time (ms)", 20.f, 1000.f, 320.f);
    F ("dlyFb",   "Delay Feedback", 0.f, 0.9f, 0.3f);
    F ("dlyTone", "Delay Tone (Hz)", 1000.f, 16000.f, 6000.f);
    F ("dlyMix",  "Delay Mix", 0.f, 1.f, 0.15f);

    // Reverb
    F ("rvbSize", "Reverb Size", 0.f, 1.f, 0.5f);
    F ("rvbDamp", "Reverb Damping", 0.f, 1.f, 0.5f);
    F ("rvbMix",  "Reverb Mix", 0.f, 1.f, 0.2f);

    // Output
    F ("outGain", "Output (dB)", -24.f, 12.f, 0.f);

    return l;
}

//==============================================================================
VocalPolishProcessor::VocalPolishProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
}

bool VocalPolishProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void VocalPolishProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    detector.prepare (sr);
    lead.prepare (0.f);
    harm[0].prepare (0.33f);
    harm[1].prepare (0.66f);
    curShift = targetShift = 0.f;
    curInt[0] = curInt[1] = targetInt[0] = targetInt[1] = 0.f;
    curW = targetW = 1024.f;
    gVoiced = 0.f;
    voiced = false;

    juce::dsp::ProcessSpec spec { sr, (juce::uint32) samplesPerBlock, 2 };
    compressor.prepare (spec);
    compressor.reset();

    dblA.prepare ((int) (sr * 0.1));
    dblB.prepare ((int) (sr * 0.1));
    echoL.prepare ((int) (sr * 2.0));
    echoR.prepare ((int) (sr * 2.0));
    lpL = lpR = 0.f;

    reverb.setSampleRate (sr);
    reverb.reset();
}

//==============================================================================
// Called every 512 samples with a fresh pitch estimate from the detector.
void VocalPolishProcessor::onPitch()
{
    const float f = detector.frequency;
    if (f <= 0.f)
    {
        voiced = false;
        targetShift = 0.f;
        return;
    }

    voiced = true;
    const float m = 69.f + 12.f * std::log2 (f / 440.f);

    // Autotune: shift (in semitones) needed to land on the nearest scale note
    const float qn = vp::quantise (m, keyIdx, scaleIdx);
    targetShift = atOn ? (qn - m) * atAmount : 0.f;

    // Harmonies: scale-aware interval above/below the (corrected) lead note
    const int hs = scaleIdx == 0 ? 1 : scaleIdx; // chromatic -> use major for harmonies
    const int root = juce::roundToInt (vp::quantise (m, keyIdx, hs));
    for (int v = 0; v < 2; ++v)
        if (harmChoice[v] > 0)
            targetInt[v] = (float) (vp::stepInScale (root, kSteps[harmChoice[v]], keyIdx, hs) - root);

    // Window length = a few pitch periods so the two taps stay coherent
    const float T = (float) sr / f;
    const float m4 = juce::jlimit (1.f, 4.f, std::round (0.015f * (float) sr / T));
    targetW = juce::jlimit (256.f, 4096.f, 2.f * m4 * T);
}

//==============================================================================
void VocalPolishProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2) return;

    auto* L = buffer.getWritePointer (0);
    auto* R = buffer.getWritePointer (1);
    auto P = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    const float fs = (float) sr;
    const float pi = juce::MathConstants<float>::pi;

    // ---- cache parameters
    atOn = P ("atOn") > 0.5f;
    atAmount = P ("amount");
    keyIdx = (int) P ("key");
    scaleIdx = (int) P ("scale");
    harmChoice[0] = (int) P ("h1");
    harmChoice[1] = (int) P ("h2");

    const float tauMs = 400.f * std::pow (0.0025f, P ("speed")); // 400 ms .. 1 ms
    const float shiftCoef = 1.f - std::exp (-1.f / (tauMs * 0.001f * fs));
    const float gCoef = 1.f - std::exp (-1.f / (0.01f * fs));
    const float wCoef = 0.00005f;

    const float hLevel = P ("hlevel"), spread = P ("hspread");
    auto panL = [pi] (float p) { return std::cos ((p + 1.f) * pi * 0.25f); };
    auto panR = [pi] (float p) { return std::sin ((p + 1.f) * pi * 0.25f); };
    const float gL1 = panL (-spread), gR1 = panR (-spread), gL2 = panL (spread), gR2 = panR (spread);
    const float lvl1 = harmChoice[0] > 0 ? hLevel : 0.f;
    const float lvl2 = harmChoice[1] > 0 ? hLevel : 0.f;

    // ---- stage 1: pitch correction + harmonies
    for (int i = 0; i < n; ++i)
    {
        const float in = 0.5f * (L[i] + R[i]);
        if (detector.push (in)) onPitch();

        curShift += shiftCoef * (targetShift - curShift);
        curInt[0] += shiftCoef * (targetInt[0] - curInt[0]);
        curInt[1] += shiftCoef * (targetInt[1] - curInt[1]);
        gVoiced += gCoef * ((voiced ? 1.f : 0.f) - gVoiced);
        curW += wCoef * (targetW - curW);

        const float shifted = lead.process (in, std::exp2 (curShift / 12.f), curW);
        const float delayedDry = lead.tap (curW * 0.5f); // latency-matched dry for unvoiced parts
        const float leadOut = gVoiced * shifted + (1.f - gVoiced) * delayedDry;

        const float h1 = harm[0].process (in, std::exp2 ((curShift + curInt[0]) / 12.f), curW) * gVoiced * lvl1;
        const float h2 = harm[1].process (in, std::exp2 ((curShift + curInt[1]) / 12.f), curW) * gVoiced * lvl2;

        L[i] = leadOut + h1 * gL1 + h2 * gL2;
        R[i] = leadOut + h1 * gR1 + h2 * gR2;
    }

    // ---- stage 2: compressor
    compressor.setThreshold (P ("cthresh"));
    compressor.setRatio (P ("cratio"));
    compressor.setAttack (P ("catk"));
    compressor.setRelease (P ("crel"));
    {
        juce::dsp::AudioBlock<float> block (buffer);
        auto stereo = block.getSubsetChannelBlock (0, 2);
        juce::dsp::ProcessContextReplacing<float> ctx (stereo);
        compressor.process (ctx);
    }
    buffer.applyGain (juce::Decibels::decibelsToGain (P ("makeup")));

    // ---- stage 3: saturation, doubler, delay
    const float drive = P ("satDrive"), satMix = P ("satMix");
    const float g = 1.f + 11.f * drive;
    const float satNorm = 0.3f / std::tanh (0.3f * g);

    const float dblAmt = P ("dblAmt"), dblT = P ("dblTime");
    const float dFb = P ("dlyFb"), dMix = P ("dlyMix"), dTarget = P ("dlyTime");
    const float lpA = 1.f - std::exp (-juce::MathConstants<float>::twoPi * P ("dlyTone") / fs);
    const float tSmooth = 1.f - std::exp (-1.f / (0.05f * fs));

    for (int i = 0; i < n; ++i)
    {
        float l = L[i], r = R[i];

        if (drive > 0.001f)
        {
            l = l * (1.f - satMix) + satMix * satNorm * std::tanh (g * l);
            r = r * (1.f - satMix) + satMix * satNorm * std::tanh (g * r);
        }

        // doubler (ADT style): two modulated short delays panned L/R
        dblA.push (0.5f * (l + r));
        dblB.push (0.5f * (l + r));
        lfoA += 0.31f / fs; if (lfoA >= 1.f) lfoA -= 1.f;
        lfoB += 0.23f / fs; if (lfoB >= 1.f) lfoB -= 1.f;
        const float dA = dblA.read ((dblT + 1.5f * std::sin (juce::MathConstants<float>::twoPi * lfoA)) * 0.001f * fs);
        const float dB = dblB.read ((dblT * 1.4f + 2.0f * std::sin (juce::MathConstants<float>::twoPi * lfoB)) * 0.001f * fs);
        l += dblAmt * dA;
        r += dblAmt * dB;

        // ping-pong delay with filtered feedback
        tCur += tSmooth * (dTarget - tCur);
        const float eL = echoL.read (tCur * 0.001f * fs);
        const float eR = echoR.read (juce::jmin (tCur * 1.5f, 1500.f) * 0.001f * fs);
        lpL += lpA * (eL - lpL);
        lpR += lpA * (eR - lpR);
        echoL.push (l + dFb * lpR);
        echoR.push (r + dFb * lpL);
        l += dMix * eL;
        r += dMix * eR;

        L[i] = l;
        R[i] = r;
    }

    // ---- stage 4: reverb
    juce::Reverb::Parameters rp;
    rp.roomSize = P ("rvbSize");
    rp.damping = P ("rvbDamp");
    rp.wetLevel = 0.6f * P ("rvbMix");
    rp.dryLevel = 1.f;
    rp.width = 1.f;
    rp.freezeMode = 0.f;
    reverb.setParameters (rp);
    reverb.processStereo (L, R, n);

    // ---- output trim
    buffer.applyGain (juce::Decibels::decibelsToGain (P ("outGain")));
}

//==============================================================================
juce::AudioProcessorEditor* VocalPolishProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void VocalPolishProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void VocalPolishProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VocalPolishProcessor();
}
