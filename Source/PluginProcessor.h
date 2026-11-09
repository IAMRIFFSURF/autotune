#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "PitchEngine.h"

class SnapTuneProcessor : public juce::AudioProcessor,
                          private juce::AsyncUpdater
{
public:
    SnapTuneProcessor();
    ~SnapTuneProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SnapTune"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- shared with the editor ---
    juce::AudioProcessorValueTreeState apvts;

    static const juce::StringArray& scaleNames();
    static unsigned scaleMask (int index);             // relative to the key (bit 0 = root)
    static const char* noteParamId (int pitchClass);   // "note0" ... "note11"

    unsigned getScaleMaskAbsolute() const;             // key + scale + enabled notes (no MIDI)

    std::atomic<float> meterDetected { 0.0f };         // MIDI note number with decimals
    std::atomic<float> meterTarget   { 0.0f };
    std::atomic<bool>  meterVoiced   { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleAsyncUpdate() override;

    std::array<snaptune::PitchCorrector, 2> engines;
    std::array<bool, 128> held {};
    int lastRange = -1;
    int currentProgram = 0;
    double currentRate = 44100.0;
    std::atomic<int> pendingLatency { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnapTuneProcessor)
};
