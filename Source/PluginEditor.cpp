#include "PluginEditor.h"
#include <cmath>

namespace
{
const char* kNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

juce::String noteName (int midi)
{
    const int pc = ((midi % 12) + 12) % 12;
    return juce::String (kNames[pc]) + juce::String (midi / 12 - 1);
}

bool isBlack (int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
}

//==============================================================================
SnapLook::SnapLook()
{
    setColour (juce::ResizableWindow::backgroundColourId, bg());
    setColour (juce::Slider::rotarySliderFillColourId, accent());
    setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff2c3342));
    setColour (juce::Slider::thumbColourId, juce::Colours::white);
    setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, juce::Colour (0xffaab2c5));
    setColour (juce::ComboBox::backgroundColourId, panel());
    setColour (juce::ComboBox::textColourId, juce::Colours::white);
    setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff2c3342));
    setColour (juce::ComboBox::arrowColourId, accent());
    setColour (juce::PopupMenu::backgroundColourId, panel());
    setColour (juce::PopupMenu::textColourId, juce::Colours::white);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent().darker (0.4f));
    setColour (juce::ToggleButton::textColourId, juce::Colours::white);
    setColour (juce::ToggleButton::tickColourId, accent());
}

//==============================================================================
void NoteButton::paintButton (juce::Graphics& g, bool over, bool)
{
    const int key   = (int) proc.apvts.getRawParameterValue ("key")->load();
    const int scale = (int) proc.apvts.getRawParameterValue ("scale")->load();
    const unsigned inScaleMask = snaptune::rotateMask (SnapTuneProcessor::scaleMask (scale), key);
    const bool inScale = (inScaleMask >> pc) & 1u;
    const bool on = getToggleState();

    auto r = getLocalBounds().toFloat().reduced (2.0f);
    juce::Colour fill;
    if (on && inScale)  fill = SnapLook::accent();
    else if (on)        fill = juce::Colour (0xff3a4254);
    else                fill = juce::Colour (0xff171b24);
    if (over) fill = fill.brighter (0.15f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (isBlack (pc) ? juce::Colour (0xff0b0d12) : juce::Colour (0xff2c3342));
    g.drawRoundedRectangle (r, 6.0f, 1.2f);

    g.setColour (on && inScale ? juce::Colour (0xff0b0d12) : (on ? juce::Colours::white : juce::Colour (0xff5b647a)));
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText (kNames[pc], getLocalBounds(), juce::Justification::centred);

    if (! on)
    {
        g.setColour (SnapLook::accent2().withAlpha (0.8f));
        g.drawLine (r.getX() + 6, r.getBottom() - 6, r.getRight() - 6, r.getY() + 6, 1.5f);
    }
}

//==============================================================================
SnapTuneEditor::SnapTuneEditor (SnapTuneProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&look);
    setSize (960, 640);

    histDet.fill (std::nanf (""));
    histTgt.fill (std::nanf (""));

    for (int i = 0; i < proc.getNumPrograms(); ++i)
        presetBox.addItem ("Preset: " + proc.getProgramName (i), i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };
    addAndMakeVisible (presetBox);

    addCombo (keyBox,   keyAtt,   "key");
    addCombo (scaleBox, scaleAtt, "scale");
    addCombo (rangeBox, rangeAtt, "range");

    addAndMakeVisible (midiButton);
    midiAtt = std::make_unique<ButtonAtt> (proc.apvts, "midi", midiButton);
    addAndMakeVisible (lowLatButton);
    lowLatAtt = std::make_unique<ButtonAtt> (proc.apvts, "lowlat", lowLatButton);

    for (int i = 0; i < 12; ++i)
    {
        noteButtons[(size_t) i] = std::make_unique<NoteButton> (proc, i);
        addAndMakeVisible (*noteButtons[(size_t) i]);
        noteAtts[(size_t) i] = std::make_unique<ButtonAtt> (proc.apvts, SnapTuneProcessor::noteParamId (i), *noteButtons[(size_t) i]);
    }

    addKnob (speed,     "speed",     "Retune Speed", " ms");
    addKnob (humanize,  "humanize",  "Humanize",     " %");
    addKnob (flex,      "flex",      "Flex-Tune",    " %");
    addKnob (vibrato,   "vibrato",   "Keep Vibrato", " %");
    addKnob (amount,    "amount",    "Amount",       " %");
    addKnob (hold,      "stick",     "Note Hold",    " ct");
    addKnob (tracking,  "tracking",  "Tracking",     " %");
    addKnob (formant,   "formant",   "Formant",      " st");
    addKnob (transpose, "transpose", "Transpose",    " st");
    addKnob (tuning,    "tuning",    "A4 Tuning",    " Hz");

    speed.slider.setTooltip ("0 ms = hard-tune / robotic effect. Higher = more natural.");

    startTimerHz (30);
}

SnapTuneEditor::~SnapTuneEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void SnapTuneEditor::addKnob (Knob& k, const juce::String& id, const juce::String& name, const juce::String& suffix)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 18);
    k.slider.setTextValueSuffix (suffix);
    k.slider.setNumDecimalPlacesToDisplay (id == "tuning" || id == "formant" || id == "transpose" ? 1 : 0);
    k.slider.setDoubleClickReturnValue (true, id == "amount" ? 100.0 : (id == "stick" ? 20.0 : (id == "tuning" ? 440.0 : (id == "tracking" ? 60.0 : 0.0))));
    addAndMakeVisible (k.slider);

    k.label.setText (name, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (k.label);

    k.att = std::make_unique<SliderAtt> (proc.apvts, id, k.slider);
}

void SnapTuneEditor::addCombo (juce::ComboBox& c, std::unique_ptr<ComboAtt>& att, const juce::String& id)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        c.addItemList (choice->choices, 1);
    addAndMakeVisible (c);
    att = std::make_unique<ComboAtt> (proc.apvts, id, c);
}

//==============================================================================
void SnapTuneEditor::resized()
{
    auto r = getLocalBounds().reduced (20);
    auto top = r.removeFromTop (40);
    top.removeFromLeft (150);                       // title is painted
    presetBox.setBounds (top.removeFromLeft (190).reduced (0, 6));
    top.removeFromLeft (8);
    keyBox.setBounds   (top.removeFromLeft (66).reduced (0, 6));
    top.removeFromLeft (8);
    scaleBox.setBounds (top.removeFromLeft (150).reduced (0, 6));
    top.removeFromLeft (8);
    rangeBox.setBounds (top.removeFromLeft (150).reduced (0, 6));
    midiButton.setBounds (top.removeFromRight (105).reduced (0, 6));
    lowLatButton.setBounds (top.removeFromRight (115).reduced (0, 6));

    r.removeFromTop (8);
    graphArea = r.removeFromTop (200);

    r.removeFromTop (12);
    auto notes = r.removeFromTop (44);
    const int w = notes.getWidth() / 12;
    for (int i = 0; i < 12; ++i)
        noteButtons[(size_t) i]->setBounds (notes.removeFromLeft (w));

    r.removeFromTop (10);
    Knob* rows[2][5] = { { &speed, &humanize, &flex, &vibrato, &amount },
                         { &hold, &tracking, &formant, &transpose, &tuning } };
    for (auto& row : rows)
    {
        auto strip = r.removeFromTop (118);
        const int kw = strip.getWidth() / 5;
        for (auto* k : row)
        {
            auto c = strip.removeFromLeft (kw);
            k->label.setBounds (c.removeFromTop (18));
            k->slider.setBounds (c.reduced (10, 0));
        }
    }
}

//==============================================================================
void SnapTuneEditor::timerCallback()
{
    voiced = proc.meterVoiced.load();
    lastDet = proc.meterDetected.load();
    lastTgt = proc.meterTarget.load();

    if (voiced)
    {
        histDet[(size_t) histPos] = lastDet;
        histTgt[(size_t) histPos] = lastTgt;
        viewCentre += (lastTgt - viewCentre) * 0.08f;
    }
    else
    {
        histDet[(size_t) histPos] = std::nanf ("");
        histTgt[(size_t) histPos] = std::nanf ("");
    }
    histPos = (histPos + 1) % kHist;

    repaint();
}

void SnapTuneEditor::paint (juce::Graphics& g)
{
    g.fillAll (SnapLook::bg());

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText ("Snap", 20, 20, 70, 40, juce::Justification::centredLeft);
    g.setColour (SnapLook::accent());
    g.drawText ("Tune", 82, 20, 80, 40, juce::Justification::centredLeft);

    g.setColour (juce::Colour (0xff5b647a));
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("latency " + juce::String (juce::roundToInt (proc.getLatencySamples() * 1000.0 / juce::jmax (1.0, proc.getSampleRate()))) + " ms",
                getLocalBounds().reduced (20).removeFromBottom (14), juce::Justification::centredRight);

    drawGraph (g, graphArea);
}

void SnapTuneEditor::drawGraph (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (SnapLook::panel());
    g.fillRoundedRectangle (area.toFloat(), 10.0f);

    g.saveState();
    g.reduceClipRegion (area);

    const float span = 18.0f;                        // semitones visible
    const float pxPerSemi = (float) area.getHeight() / span;
    const auto yFor = [&] (float midi) { return (float) area.getCentreY() - (midi - viewCentre) * pxPerSemi; };

    const unsigned mask = proc.getScaleMaskAbsolute();
    const int lo = (int) std::floor (viewCentre - span * 0.5f) - 1;
    const int hi = (int) std::ceil  (viewCentre + span * 0.5f) + 1;
    for (int n = lo; n <= hi; ++n)
    {
        const float y = yFor ((float) n);
        const bool allowed = (mask >> (((n % 12) + 12) % 12)) & 1u;
        g.setColour (allowed ? SnapLook::accent().withAlpha (0.22f) : juce::Colours::white.withAlpha (0.04f));
        g.drawHorizontalLine ((int) y, (float) area.getX(), (float) area.getRight());
        if (allowed)
        {
            g.setColour (SnapLook::accent().withAlpha (0.7f));
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (noteName (n), area.getX() + 6, (int) y - 8, 40, 16, juce::Justification::centredLeft);
        }
    }

    const auto trace = [&] (const std::array<float, kHist>& h, juce::Colour c, float thick)
    {
        juce::Path path;
        bool pen = false;
        for (int i = 0; i < kHist; ++i)
        {
            const float v = h[(size_t) ((histPos + i) % kHist)];
            const float x = (float) area.getX() + 50.0f + (float) i / (float) (kHist - 1) * (float) (area.getWidth() - 60);
            if (std::isnan (v)) { pen = false; continue; }
            if (! pen) { path.startNewSubPath (x, yFor (v)); pen = true; }
            else         path.lineTo (x, yFor (v));
        }
        g.setColour (c);
        g.strokePath (path, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };

    trace (histDet, juce::Colours::white.withAlpha (0.45f), 1.6f);
    trace (histTgt, SnapLook::accent2(), 2.6f);
    g.restoreState();

    // readout
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    if (voiced)
    {
        const int nearest = (int) std::lround (lastDet);
        const int cents = (int) std::lround ((lastDet - (float) nearest) * 100.0f);
        const int tgt = (int) std::lround (lastTgt);
        g.setColour (juce::Colours::white);
        g.drawText (noteName (nearest) + (cents >= 0 ? "  +" : "  ") + juce::String (cents) + " ct   \xe2\x86\x92   " + noteName (tgt),
                    area.reduced (12, 8), juce::Justification::topRight);
    }
    else
    {
        g.setColour (juce::Colour (0xff5b647a));
        g.drawText ("no pitch", area.reduced (12, 8), juce::Justification::topRight);
    }
}
