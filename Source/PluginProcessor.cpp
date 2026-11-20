#include "PluginProcessor.h"

namespace
{
    struct Scale { const char* name; std::vector<int> notes; };

    const std::vector<Scale>& getScales()
    {
        static const std::vector<Scale> scales = {
            { "Minor Pentatonic",    { 0, 3, 5, 7, 10 } },
            { "Minor",               { 0, 2, 3, 5, 7, 8, 10 } },
            { "Phrygian",            { 0, 1, 3, 5, 7, 8, 10 } },
            { "Phrygian Dominant",   { 0, 1, 4, 5, 7, 8, 10 } },
            { "Harmonic Minor",      { 0, 2, 3, 5, 7, 8, 11 } },
            { "Blues",               { 0, 3, 5, 6, 7, 10 } },
            { "Dorian",              { 0, 2, 3, 5, 7, 9, 10 } },
            { "Major",               { 0, 2, 4, 5, 7, 9, 11 } },
            { "Chromatic",           { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } }
        };
        return scales;
    }

    // Stateless hash so the same seed always yields the same pattern.
    uint64_t mix (uint64_t x)
    {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    int64_t floorDiv (int64_t a, int64_t b)
    {
        int64_t q = a / b;
        if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
        return q;
    }
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout AcidRandomizerProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    StringArray scaleNames;
    for (auto& s : getScales()) scaleNames.add (s.name);

    auto fl = [] (float lo, float hi) { return NormalisableRange<float> (lo, hi); };

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "root", 1 }, "Root Note",
        StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "octave", 1 }, "Base Octave", 0, 4, 2));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", scaleNames, 0));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "steps", 1 }, "Steps", 1, 32, 16));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "rate", 1 }, "Rate",
        StringArray { "1/4", "1/8", "1/16", "1/32" }, 2));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "density", 1 }, "Density", fl (0.f, 1.f), 0.8f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "rootBias", 1 }, "Root Bias", fl (0.f, 1.f), 0.45f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "octUp", 1 }, "Octave Up", fl (0.f, 1.f), 0.2f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "octDown", 1 }, "Octave Down", fl (0.f, 1.f), 0.12f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "accent", 1 }, "Accent Amount", fl (0.f, 1.f), 0.3f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "slide", 1 }, "Slide Amount", fl (0.f, 1.f), 0.25f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "gate", 1 }, "Gate", fl (0.05f, 0.95f), 0.5f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "swing", 1 }, "Swing", fl (0.f, 0.5f), 0.f));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "vel", 1 }, "Normal Velocity", 1, 127, 85));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "accVel", 1 }, "Accent Velocity", 1, 127, 120));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "seed", 1 }, "Seed", 0, 9999, 1));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "evolve", 1 }, "New Pattern Each Loop", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "anchor", 1 }, "Anchor First Step On Root", true));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "channel", 1 }, "MIDI Channel", 1, 16, 1));

    return { std::make_move_iterator (p.begin()), std::make_move_iterator (p.end()) };
}

//==============================================================================
AcidRandomizerProcessor::AcidRandomizerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    pRoot     = apvts.getRawParameterValue ("root");
    pOctave   = apvts.getRawParameterValue ("octave");
    pScale    = apvts.getRawParameterValue ("scale");
    pSteps    = apvts.getRawParameterValue ("steps");
    pRate     = apvts.getRawParameterValue ("rate");
    pDensity  = apvts.getRawParameterValue ("density");
    pRootBias = apvts.getRawParameterValue ("rootBias");
    pOctUp    = apvts.getRawParameterValue ("octUp");
    pOctDown  = apvts.getRawParameterValue ("octDown");
    pAccent   = apvts.getRawParameterValue ("accent");
    pSlide    = apvts.getRawParameterValue ("slide");
    pGate     = apvts.getRawParameterValue ("gate");
    pSwing    = apvts.getRawParameterValue ("swing");
    pVel      = apvts.getRawParameterValue ("vel");
    pAccVel   = apvts.getRawParameterValue ("accVel");
    pSeed     = apvts.getRawParameterValue ("seed");
    pEvolve   = apvts.getRawParameterValue ("evolve");
    pAnchor   = apvts.getRawParameterValue ("anchor");
    pChannel  = apvts.getRawParameterValue ("channel");
}

void AcidRandomizerProcessor::prepareToPlay (double, int)
{
    currentNote = -1;
    slideHeld = false;
    lastEndPpq = -1.0;
}

bool AcidRandomizerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

//==============================================================================
void AcidRandomizerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    midi.clear();

    const int numSamples = buffer.getNumSamples();

    auto killNote = [&] (int offset)
    {
        if (currentNote >= 0)
        {
            midi.addEvent (juce::MidiMessage::noteOff (currentChannel, currentNote), offset);
            currentNote = -1;
        }
        slideHeld = false;
    };

    // --- transport -----------------------------------------------------------
    bool playing = false;
    double startPpq = 0.0, bpm = 120.0;

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto pp = pos->getPpqPosition()) startPpq = *pp; else playing = false;
            bpm = pos->getBpm().orFallback (120.0);
        }

    if (! playing)
    {
        killNote (0);
        lastEndPpq = -1.0;
        return;
    }

    if (bpm <= 0.0) bpm = 120.0;

    const double samplesPerPpq = getSampleRate() * 60.0 / bpm;
    const double endPpq = startPpq + numSamples / samplesPerPpq;

    if (lastEndPpq >= 0.0 && startPpq < lastEndPpq - 1e-3)   // loop or jump back
        killNote (0);
    lastEndPpq = endPpq;

    auto flushOff = [&]
    {
        if (currentNote >= 0 && noteOffPpq < endPpq)
            killNote (juce::jlimit (0, numSamples - 1, (int) ((noteOffPpq - startPpq) * samplesPerPpq)));
    };
    flushOff();

    // --- parameters ----------------------------------------------------------
    static const double rateLengths[] = { 1.0, 0.5, 0.25, 0.125 };
    const double L        = rateLengths[juce::jlimit (0, 3, (int) pRate->load())];
    const int    steps    = juce::jlimit (1, 32, (int) pSteps->load());
    const double swing    = pSwing->load();
    const float  density  = pDensity->load();
    const float  rootBias = pRootBias->load();
    const float  octUp    = pOctUp->load();
    const float  octDown  = pOctDown->load();
    const float  accentP  = pAccent->load();
    const float  slideP   = pSlide->load();
    const double gate     = pGate->load();
    const int    velNorm  = (int) pVel->load();
    const int    velAcc   = (int) pAccVel->load();
    const uint64_t seed   = (uint64_t) pSeed->load();
    const bool   evolve   = pEvolve->load() > 0.5f;
    const bool   anchor   = pAnchor->load() > 0.5f;
    const int    channel  = juce::jlimit (1, 16, (int) pChannel->load());
    const int    baseNote = 12 * ((int) pOctave->load() + 1) + (int) pRoot->load();

    const auto& scale = getScales()[(size_t) juce::jlimit (0, (int) getScales().size() - 1, (int) pScale->load())].notes;
    const int scaleLen = (int) scale.size();

    auto stepPos = [&] (int64_t k)
    {
        return (double) k * L + ((k & 1) ? swing * L * 0.5 : 0.0);
    };

    struct StepData { bool active; int note; bool accent; bool slide; };

    // Deterministic step generator: same seed + step index -> same step.
    auto gen = [&] (int64_t k) -> StepData
    {
        const int idx = (int) (((k % steps) + steps) % steps);
        const int64_t cycle = evolve ? floorDiv (k, steps) : 0;

        juce::Random r ((juce::int64) mix (mix (mix (seed) + (uint64_t) cycle) + (uint64_t) idx));

        const bool first      = (idx == 0 && anchor);
        const bool active     = first || r.nextFloat() < density;
        const float rootRoll  = r.nextFloat();
        const int   degRoll   = r.nextInt (scaleLen);
        const float octRoll   = r.nextFloat();
        const float accRoll   = r.nextFloat();
        const float slideRoll = r.nextFloat();

        const int degree = (first || rootRoll < rootBias) ? 0 : degRoll;

        int shift = 0;
        if (! first)
        {
            if (octRoll < octUp)               shift = 12;
            else if (octRoll > 1.0f - octDown) shift = -12;
        }

        const int note = juce::jlimit (0, 127, baseNote + scale[(size_t) degree] + shift);
        return { active, note, accRoll < accentP, slideRoll < slideP };
    };

    // --- step events in this block --------------------------------------------
    for (int64_t k = (int64_t) std::floor (startPpq / L) - 1; ; ++k)
    {
        const double pos = stepPos (k);
        if (pos >= endPpq) break;
        if (pos < startPpq) continue;

        const int offset = juce::jlimit (0, numSamples - 1, (int) ((pos - startPpq) * samplesPerPpq));
        const StepData s = gen (k);

        if (! s.active)
        {
            if (slideHeld) killNote (offset);   // safety: slid into a rest
            continue;
        }

        // A slide only makes sense if the next step actually plays.
        const bool slide = s.slide && gen (k + 1).active;
        const int vel = juce::jlimit (1, 127, s.accent ? velAcc : velNorm);

        if (slideHeld && currentNote >= 0)
        {
            // 303 slide: new note-on arrives BEFORE the old note-off (legato).
            if (s.note != currentNote)
            {
                const int oldNote = currentNote, oldChannel = currentChannel;
                midi.addEvent (juce::MidiMessage::noteOn (channel, s.note, (juce::uint8) vel), offset);
                midi.addEvent (juce::MidiMessage::noteOff (oldChannel, oldNote),
                               juce::jmin (offset + 1, numSamples - 1));
                currentNote = s.note;
                currentChannel = channel;
            }
            // same pitch: tie, the note just keeps sounding
        }
        else
        {
            killNote (offset);
            midi.addEvent (juce::MidiMessage::noteOn (channel, s.note, (juce::uint8) vel), offset);
            currentNote = s.note;
            currentChannel = channel;
        }

        slideHeld = slide;
        noteOffPpq = slide ? stepPos (k + 1) + L                         // released by the next step
                           : juce::jmin (pos + gate * L, stepPos (k + 1) - 1e-4);
        flushOff();
    }
}

//==============================================================================
class AcidRandomizerEditor : public juce::AudioProcessorEditor
{
public:
    explicit AcidRandomizerEditor (AcidRandomizerProcessor& p)
        : AudioProcessorEditor (&p), proc (p), generic (p)
    {
        addAndMakeVisible (generic);
        addAndMakeVisible (randomButton);
        randomButton.setButtonText ("Randomize pattern");
        randomButton.onClick = [this]
        {
            if (auto* seed = proc.apvts.getParameter ("seed"))
            {
                seed->beginChangeGesture();
                seed->setValueNotifyingHost (juce::Random::getSystemRandom().nextFloat());
                seed->endChangeGesture();
            }
        };
        setSize (420, 700);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        randomButton.setBounds (b.removeFromTop (44).reduced (6));
        generic.setBounds (b);
    }

private:
    AcidRandomizerProcessor& proc;
    juce::GenericAudioProcessorEditor generic;
    juce::TextButton randomButton;
};

juce::AudioProcessorEditor* AcidRandomizerProcessor::createEditor()
{
    return new AcidRandomizerEditor (*this);
}

//==============================================================================
void AcidRandomizerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AcidRandomizerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AcidRandomizerProcessor();
}
