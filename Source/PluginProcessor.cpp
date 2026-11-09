#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
const float kRangeHz[] = { 65.0f, 85.0f, 130.0f, 180.0f };

struct Preset { const char* name; float speed, amount, stick, humanize, flex, vibrato, formant; };

// Starting points taken from what reviewers and the Auto-Tune / Waves manuals recommend:
// 0 ms = effect, 40-60 ms = natural, Humanize + Flex-Tune keep long notes alive.
const Preset kPresets[] = {
    { "Default",                 0.0f, 100.0f, 20.0f,   0.0f,   0.0f,   0.0f, 0.0f },
    { "Hard Tune (T-Pain)",      0.0f, 100.0f, 15.0f,   0.0f,   0.0f,   0.0f, 0.0f },
    { "Trap Melodic",           12.0f, 100.0f, 20.0f,  15.0f,   0.0f,  20.0f, 0.0f },
    { "Modern Pop",             25.0f,  95.0f, 25.0f,  40.0f,  25.0f,  60.0f, 0.0f },
    { "Natural Fix",            55.0f,  90.0f, 30.0f,  60.0f,  50.0f,  90.0f, 0.0f },
    { "Subtle Polish",         110.0f,  70.0f, 35.0f,  70.0f,  70.0f, 100.0f, 0.0f },
    { "Robot (no glide)",        0.0f, 100.0f,  0.0f,   0.0f,   0.0f,   0.0f, -2.0f },
};

struct ScaleDef { const char* name; unsigned mask; };

constexpr unsigned bits (std::initializer_list<int> l)
{
    unsigned m = 0;
    for (int v : l) m |= 1u << v;
    return m;
}

const ScaleDef kScales[] = {
    { "Chromatic",         0xFFF },
    { "Major",             bits ({ 0, 2, 4, 5, 7, 9, 11 }) },
    { "Natural Minor",     bits ({ 0, 2, 3, 5, 7, 8, 10 }) },
    { "Harmonic Minor",    bits ({ 0, 2, 3, 5, 7, 8, 11 }) },
    { "Pentatonic Major",  bits ({ 0, 2, 4, 7, 9 }) },
    { "Pentatonic Minor",  bits ({ 0, 3, 5, 7, 10 }) },
    { "Blues",             bits ({ 0, 3, 5, 6, 7, 10 }) },
    { "Dorian",            bits ({ 0, 2, 3, 5, 7, 9, 10 }) },
    { "Phrygian",          bits ({ 0, 1, 3, 5, 7, 8, 10 }) },
    { "Mixolydian",        bits ({ 0, 2, 4, 5, 7, 9, 10 }) },
};
}

//==============================================================================
const juce::StringArray& SnapTuneProcessor::scaleNames()
{
    static const juce::StringArray names = [] {
        juce::StringArray a;
        for (auto& s : kScales) a.add (s.name);
        return a;
    }();
    return names;
}

unsigned SnapTuneProcessor::scaleMask (int index)
{
    index = juce::jlimit (0, (int) std::size (kScales) - 1, index);
    return kScales[index].mask;
}

const char* SnapTuneProcessor::noteParamId (int pc)
{
    static const char* ids[] = { "note0", "note1", "note2", "note3", "note4", "note5",
                                 "note6", "note7", "note8", "note9", "note10", "note11" };
    return ids[juce::jlimit (0, 11, pc)];
}

juce::AudioProcessorValueTreeState::ParameterLayout SnapTuneProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "key", 1 }, "Key",
                 StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 1 }, "Scale", scaleNames(), 0));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "speed", 1 }, "Retune Speed",
                 NormalisableRange<float> (0.0f, 400.0f, 0.1f, 0.4f), 0.0f,
                 AudioParameterFloatAttributes().withLabel ("ms")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 1 }, "Amount",
                 NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                 AudioParameterFloatAttributes().withLabel ("%")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "stick", 1 }, "Note Hold",
                 NormalisableRange<float> (0.0f, 50.0f, 0.1f), 20.0f,
                 AudioParameterFloatAttributes().withLabel ("cents")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "formant", 1 }, "Formant",
                 NormalisableRange<float> (-4.0f, 4.0f, 0.01f), 0.0f,
                 AudioParameterFloatAttributes().withLabel ("st")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "tuning", 1 }, "Tuning (A4)",
                 NormalisableRange<float> (415.0f, 466.0f, 0.1f), 440.0f,
                 AudioParameterFloatAttributes().withLabel ("Hz")));

    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "range", 1 }, "Voice Range",
                 StringArray { "Low (65 Hz+)", "Normal (85 Hz+)", "High (130 Hz+)", "Very High (180 Hz+)" }, 1));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "lowlat", 1 }, "Low Latency", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "humanize", 1 }, "Humanize",
                 NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("%")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "flex", 1 }, "Flex-Tune",
                 NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("%")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "vibrato", 1 }, "Vibrato",
                 NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("%")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "tracking", 1 }, "Tracking",
                 NormalisableRange<float> (0.0f, 100.0f, 0.1f), 60.0f, AudioParameterFloatAttributes().withLabel ("%")));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "transpose", 1 }, "Transpose",
                 NormalisableRange<float> (-7.0f, 12.0f, 0.01f), 0.0f, AudioParameterFloatAttributes().withLabel ("st")));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "midi", 1 }, "MIDI Notes", false));

    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int i = 0; i < 12; ++i)
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { noteParamId (i), 1 },
                                                           String ("Note ") + names[i], true));

    return { p.begin(), p.end() };
}

//==============================================================================
SnapTuneProcessor::SnapTuneProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SNAPTUNE", createLayout())
{
}

bool SnapTuneProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in  = l.getMainInputChannelSet();
    const auto out = l.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void SnapTuneProcessor::prepareToPlay (double sampleRate, int)
{
    currentRate = sampleRate;
    const int range = (int) apvts.getRawParameterValue ("range")->load();
    const bool low = apvts.getRawParameterValue ("lowlat")->load() > 0.5f;
    lastRange = range * 2 + (low ? 1 : 0);
    for (auto& e : engines)
    {
        e.prepare (sampleRate, kRangeHz[juce::jlimit (0, 3, range)]);
        e.setMinFreq (kRangeHz[juce::jlimit (0, 3, range)], low);
    }

    held.fill (false);
    setLatencySamples (engines[0].getLatency());
}

void SnapTuneProcessor::handleAsyncUpdate()
{
    setLatencySamples (pendingLatency.load());
}

unsigned SnapTuneProcessor::getScaleMaskAbsolute() const
{
    const int key   = (int) apvts.getRawParameterValue ("key")->load();
    const int scale = (int) apvts.getRawParameterValue ("scale")->load();
    unsigned mask = snaptune::rotateMask (scaleMask (scale), key);

    unsigned enabled = 0;
    for (int i = 0; i < 12; ++i)
        if (apvts.getRawParameterValue (noteParamId (i))->load() > 0.5f)
            enabled |= 1u << i;

    return mask & enabled;
}

void SnapTuneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // --- MIDI note tracking (used when "MIDI Notes" is on) ---
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())            held[(size_t) msg.getNoteNumber()] = true;
        else if (msg.isNoteOff())      held[(size_t) msg.getNoteNumber()] = false;
        else if (msg.isAllNotesOff() || msg.isAllSoundOff()) held.fill (false);
    }

    // --- range change -> new latency ---
    const int range = (int) apvts.getRawParameterValue ("range")->load();
    const bool low = apvts.getRawParameterValue ("lowlat")->load() > 0.5f;
    const int rangeKey = range * 2 + (low ? 1 : 0);
    if (rangeKey != lastRange)
    {
        lastRange = rangeKey;
        for (auto& e : engines)
        {
            e.setMinFreq (kRangeHz[juce::jlimit (0, 3, range)], low);
            e.reset();
        }
        pendingLatency = engines[0].getLatency();
        triggerAsyncUpdate();
    }

    // --- settings ---
    snaptune::Settings s;
    s.noteMask = getScaleMaskAbsolute();

    if (apvts.getRawParameterValue ("midi")->load() > 0.5f)
    {
        unsigned m = 0;
        for (size_t n = 0; n < 128; ++n)
            if (held[n]) m |= 1u << (n % 12);
        if (m != 0) s.noteMask = m;
    }

    s.retuneMs   = apvts.getRawParameterValue ("speed")->load();
    s.amount     = apvts.getRawParameterValue ("amount")->load() * 0.01f;
    s.hysteresis = apvts.getRawParameterValue ("stick")->load() * 0.01f;   // cents -> semitones
    s.formant    = apvts.getRawParameterValue ("formant")->load();
    s.refHz      = apvts.getRawParameterValue ("tuning")->load();
    s.humanize   = apvts.getRawParameterValue ("humanize")->load() * 0.01f;
    s.flex       = apvts.getRawParameterValue ("flex")->load() * 0.01f;
    s.vibrato    = apvts.getRawParameterValue ("vibrato")->load() * 0.01f;
    s.tracking   = apvts.getRawParameterValue ("tracking")->load() * 0.01f;
    s.transpose  = apvts.getRawParameterValue ("transpose")->load();

    const int numCh = juce::jmin (buffer.getNumChannels(), (int) engines.size());
    for (int ch = 0; ch < numCh; ++ch)
    {
        auto& e = engines[(size_t) ch];
        e.setSettings (s);
        float* d = buffer.getWritePointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            d[i] = e.process (d[i]);
    }

    meterDetected = engines[0].getDetectedMidi();
    meterTarget   = engines[0].getTargetMidi();
    meterVoiced   = engines[0].isVoiced();
}

//==============================================================================
int SnapTuneProcessor::getNumPrograms() { return (int) std::size (kPresets); }
const juce::String SnapTuneProcessor::getProgramName (int index)
{
    return kPresets[juce::jlimit (0, getNumPrograms() - 1, index)].name;
}

void SnapTuneProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, getNumPrograms() - 1, index);
    currentProgram = index;
    const auto& p = kPresets[index];

    const auto set = [this] (const char* id, float value)
    {
        if (auto* prm = apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (value));
    };
    set ("speed", p.speed);       set ("amount", p.amount);   set ("stick", p.stick);
    set ("humanize", p.humanize); set ("flex", p.flex);       set ("vibrato", p.vibrato);
    set ("formant", p.formant);   set ("transpose", 0.0f);
}

//==============================================================================
juce::AudioProcessorEditor* SnapTuneProcessor::createEditor()
{
    return new SnapTuneEditor (*this);
}

void SnapTuneProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void SnapTuneProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SnapTuneProcessor();
}
