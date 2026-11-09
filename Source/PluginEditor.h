#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include "PluginProcessor.h"

//==============================================================================
class SnapLook : public juce::LookAndFeel_V4
{
public:
    SnapLook();
    static juce::Colour accent()  { return juce::Colour (0xff35e0b4); }
    static juce::Colour accent2() { return juce::Colour (0xffff5e8a); }
    static juce::Colour bg()      { return juce::Colour (0xff12151c); }
    static juce::Colour panel()   { return juce::Colour (0xff1b202b); }
};

//==============================================================================
class NoteButton : public juce::ToggleButton
{
public:
    NoteButton (SnapTuneProcessor& p, int pitchClass) : proc (p), pc (pitchClass) {}
    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

private:
    SnapTuneProcessor& proc;
    int pc;
};

//==============================================================================
class SnapTuneEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit SnapTuneEditor (SnapTuneProcessor&);
    ~SnapTuneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAtt> att;
    };

    void timerCallback() override;
    void addKnob (Knob& k, const juce::String& paramId, const juce::String& name, const juce::String& suffix);
    void addCombo (juce::ComboBox& c, std::unique_ptr<ComboAtt>& att, const juce::String& paramId);
    void drawGraph (juce::Graphics&, juce::Rectangle<int> area);

    SnapTuneProcessor& proc;
    SnapLook look;

    juce::ComboBox presetBox, keyBox, scaleBox, rangeBox;
    std::unique_ptr<ComboAtt> keyAtt, scaleAtt, rangeAtt;
    juce::ToggleButton midiButton { "MIDI notes" }, lowLatButton { "Low latency" };
    std::unique_ptr<ButtonAtt> midiAtt, lowLatAtt;

    std::array<std::unique_ptr<NoteButton>, 12> noteButtons;
    std::array<std::unique_ptr<ButtonAtt>, 12> noteAtts;

    Knob speed, humanize, flex, vibrato, amount, hold, tracking, formant, transpose, tuning;

    static constexpr int kHist = 360;
    std::array<float, kHist> histDet {}, histTgt {};
    int histPos = 0;
    float viewCentre = 57.0f;
    bool voiced = false;
    float lastDet = 0.0f, lastTgt = 0.0f;

    juce::Rectangle<int> graphArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnapTuneEditor)
};
