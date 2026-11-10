#include "ModMatrixEditor.h"
#include "Controls.h"
#include "Theme.h"

namespace {
using namespace juce;
using APVTS = AudioProcessorValueTreeState;
using rs::ModMatrix;

int fxTypeOf (ReverseSamplerProcessor& p, int slot) {
    auto* v = p.getValueTreeState().getRawParameterValue (rs::fxID (slot, "type"));
    return v != nullptr ? jlimit (0, 6, roundToInt (v->load())) : 0;
}

String sourceName (ReverseSamplerProcessor& p, int source) {
    if (source >= rs::mod::macro1 && source <= rs::mod::macro4) return p.getMacroName (source - rs::mod::macro1);
    return isPositiveAndBelow (source, (int) rs::mod::numSources) ? String (rs::mod::sourceNames[source]) : String();
}

String signedPercent (float amount) {
    const auto v = amount * 100.0f;
    return (v > 0.05f ? "+" : "") + String (v, 0) + " %";
}

bool fxParameterUsedBy (int type, const String& parameter) {
    if (type <= 0 || type >= rs::numFxTypes) return false;
    const char* shared[] { "p1", "p2", "p3" };
    for (int k = 0; k < 3; ++k)
        if (parameter == shared[k]) return rs::fxKnobNames[type][k][0] != 0;
    if (parameter == "mix") return true;
    struct Owner { const char* parameter; int type; };
    constexpr Owner owners[] { { "diffusion", 1 }, { "bias", 1 },
                               { "eqAir", 2 }, { "eqLowCut", 2 }, { "eqHighCut", 2 },
                               { "rvPreDelay", 4 }, { "rvLowCut", 4 }, { "rvChar", 4 },
                               { "chDelay", 5 }, { "chFeedback", 5 }, { "chOutput", 5 }, { "chVibRate", 5 }, { "chVibDepth", 5 },
                               { "cpKnee", 6 }, { "cpAttack", 6 }, { "cpRelease", 6 } };
    for (const auto& o : owners)
        if (parameter == o.parameter) return type == o.type;
    return false;
}

String shortDestinationLabel (ReverseSamplerProcessor& p, const ModMatrix::Destination& d) {
    if (d.fxSlot >= 0) {
        const int type = fxTypeOf (p, d.fxSlot);
        const char* shared[] { "p1", "p2", "p3", "mix" };
        for (int k = 0; k < 4; ++k)
            if (d.fxParameter == shared[k]) {
                const String knob = rs::fxKnobNames[type][k];
                return knob.isNotEmpty() ? knob : (k == 3 ? String ("Mix") : "Knob " + String (k + 1));
            }
        return d.name.fromFirstOccurrenceOf ("FX " + String (d.fxSlot + 1) + " ", false, false);
    }
    if (d.layer >= 0) return d.name.fromFirstOccurrenceOf ("Layer " + String (d.layer + 1) + " ", false, false);
    return d.name;
}

String fxSlotTitle (ReverseSamplerProcessor& p, int slot) {
    const int type = fxTypeOf (p, slot);
    return "FX " + String (slot + 1) + (type > 0 ? " " + String (rs::fxTypeNames[type]) : String());
}

constexpr int unavailableItemID = 1 << 20;

PopupMenu buildDestinationMenu (ReverseSamplerProcessor& p, int current) {
    const auto& list = ModMatrix::destinations();
    const auto itemID = [] (int destination) { return destination + 1; };
    PopupMenu menu;
    menu.addItem (itemID (0), "None", true, current == 0);

    const auto addGroup = [&] (const String& title, const std::function<bool (int, const ModMatrix::Destination&)>& member) {
        PopupMenu group;
        bool ticked = false, any = false;
        for (int d = 1; d < (int) list.size(); ++d) {
            const auto& entry = list[(size_t) d];
            if (! member (d, entry)) continue;
            any = true;
            ticked = ticked || d == current;
            group.addItem (itemID (d), shortDestinationLabel (p, entry), true, d == current);
        }
        if (any) menu.addSubMenu (title, group, true, std::unique_ptr<Drawable>(), ticked);
    };

    addGroup ("Global", [] (int, const ModMatrix::Destination& d) { return d.layer < 0 && d.fxSlot < 0 && ! d.paramID.startsWith ("lfo"); });
    for (int layer = 0; layer < 2; ++layer)
        addGroup ("Layer " + String (layer + 1), [layer] (int, const ModMatrix::Destination& d) { return d.layer == layer; });

    bool anyFx = false;
    for (int slot = 0; slot < rs::fxSlots; ++slot) {
        const int type = fxTypeOf (p, slot);
        const bool holdsCurrent = current > 0 && list[(size_t) current].fxSlot == slot;
        if (type == 0 && ! holdsCurrent) continue;
        anyFx = true;
        addGroup (fxSlotTitle (p, slot), [slot, type, current] (int d, const ModMatrix::Destination& entry) {
            return entry.fxSlot == slot && (fxParameterUsedBy (type, entry.fxParameter) || d == current);
        });
    }
    if (! anyFx) menu.addItem (unavailableItemID, "FX: no effect loaded", false);

    addGroup ("LFO", [] (int, const ModMatrix::Destination& d) { return d.paramID.startsWith ("lfo"); });
    return menu;
}

class DestinationBox final : public ComboBox {
public:
    explicit DestinationBox (ReverseSamplerProcessor& p) : processor (p) {}
    void showPopup() override {
        auto menu = buildDestinationMenu (processor, getSelectedId() - 1);
        menu.setLookAndFeel (&getLookAndFeel());
        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()),
                            [safe = SafePointer<DestinationBox> (this)] (int result) {
                                if (safe == nullptr) return;
                                safe->hidePopup();
                                if (result > 0 && result < unavailableItemID) safe->setSelectedId (result);
                            });
    }
private:
    ReverseSamplerProcessor& processor;
};

void drawPanel (Graphics& g, Rectangle<int> area, const String& title) {
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (area.toFloat(), 8.0f);
    g.setColour (Theme::border);
    g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 8.0f, 1.0f);
    g.setColour (Theme::modulation);
    g.setFont (FontOptions (13.0f, Font::bold));
    g.drawText (title, area.reduced (10, 4).removeFromTop (18), Justification::centredLeft);
}

void drawSourceMeter (Graphics& g, Rectangle<int> area, float value) {
    auto bar = area.toFloat();
    g.setColour (Theme::panelLight);
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (Theme::modulation);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * jlimit (0.0f, 1.0f, value)), 2.0f);
}

void styleToggle (TextButton& b) {
    b.setClickingTogglesState (true);
    b.setColour (TextButton::buttonOnColourId, Theme::modulation);
    b.setColour (TextButton::textColourOnId, Theme::background);
    b.setWantsKeyboardFocus (false);
}

std::unique_ptr<LabelledKnob> makeKnob (APVTS& state, const String& id, const String& caption) {
    auto knob = std::make_unique<LabelledKnob> (state, id, caption, Theme::modulation);
    knob->label.setFont (FontOptions (12.0f));
    return knob;
}

void fillChoices (ComboBox& c, const RangedAudioParameter* p) {
    if (p == nullptr) return;
    int id = 1;
    for (const auto& s : p->getAllValueStrings()) c.addItem (s, id++);
}

constexpr int menuRemoveBase = 1000, menuOpenID = 2000;
}

String modui::destinationLabel (ReverseSamplerProcessor& p, int destination) {
    const auto& list = ModMatrix::destinations();
    if (! isPositiveAndBelow (destination, (int) list.size())) return "None";
    const auto& d = list[(size_t) destination];
    if (d.fxSlot < 0) return d.name;
    return fxSlotTitle (p, d.fxSlot) + " " + shortDestinationLabel (p, d);
}

Slider* modui::findTaggedSlider (Component* component) {
    for (auto* c = component; c != nullptr; c = c->getParentComponent())
        if (auto* s = dynamic_cast<Slider*> (c))
            if (taggedParameter (*s).isNotEmpty()) return s;
    return nullptr;
}

void modui::showModulationMenu (ReverseSamplerProcessor& p, const String& paramID, Component& target, std::function<void()> openMatrix) {
    const int destination = ModMatrix::destinationIndex (paramID);
    if (destination <= 0) return;
    PopupMenu menu, add;
    menu.addSectionHeader ("Modulate " + destinationLabel (p, destination));
    for (int s = 1; s < rs::mod::numSources; ++s) add.addItem (s, sourceName (p, s));
    menu.addSubMenu ("Add modulation from", add);
    bool any = false;
    for (int r = 0; r < rs::mod::numRows; ++r) {
        const auto row = p.getModMatrix().readRow (r);
        if (row.destination != destination || row.source == rs::mod::none) continue;
        if (! any) menu.addSeparator();
        any = true;
        menu.addItem (menuRemoveBase + r, "Remove " + sourceName (p, row.source) + " (" + signedPercent (row.amount) + ")");
    }
    menu.addSeparator();
    menu.addItem (menuOpenID, "Open Mod Matrix");
    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&target), [&p, paramID, openMatrix] (int result) {
        if (result <= 0) return;
        if (result < menuRemoveBase) {
            if (p.addModulation (result, paramID) < 0 && openMatrix) openMatrix();
        } else if (result < menuOpenID) {
            p.clearModulationRow (result - menuRemoveBase);
        } else if (openMatrix) {
            openMatrix();
        }
    });
}

ModulationOverlay::ModulationOverlay (ReverseSamplerProcessor& p, Component& r) : processor (p), root (r) {
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

void ModulationOverlay::collect (Component& parent, std::vector<Ring>& out) {
    auto& matrix = processor.getModMatrix();
    for (auto* child : parent.getChildren()) {
        if (child == this || ! child->isVisible()) continue;
        if (auto* s = dynamic_cast<Slider*> (child)) {
            const auto id = taggedParameter (*s);
            const int d = id.isNotEmpty() && s->isRotary() ? ModMatrix::destinationIndex (id) : 0;
            if (d > 0 && matrix.isDestinationRouted (d)) {
                Ring ring;
                ring.bounds = getLocalArea (s, s->getLookAndFeel().getSliderLayout (*s).sliderBounds).toFloat();
                const auto rotary = s->getRotaryParameters();
                ring.startAngle = rotary.startAngleRadians;
                ring.endAngle = rotary.endAngleRadians;
                ring.base = matrix.baseNormalised (d);
                float low = 0.0f, high = 0.0f;
                for (int r = 0; r < rs::mod::numRows; ++r) {
                    const auto row = matrix.readRow (r);
                    if (row.destination != d || row.source == rs::mod::none) continue;
                    if (row.bipolar) { low -= std::abs (row.amount); high += std::abs (row.amount); }
                    else { low += jmin (0.0f, row.amount); high += jmax (0.0f, row.amount); }
                }
                ring.low = jlimit (0.0f, 1.0f, ring.base + low);
                ring.high = jlimit (0.0f, 1.0f, ring.base + high);
                ring.current = matrix.displayNormalised (d);
                out.push_back (ring);
            }
        }
        collect (*child, out);
    }
}

void ModulationOverlay::timerCallback() {
    if (getBounds() != root.getLocalBounds()) setBounds (root.getLocalBounds());
    if (root.getNumChildComponents() > 0 && root.getChildComponent (root.getNumChildComponents() - 1) != this) toFront (false);
    std::vector<Ring> fresh;
    collect (root, fresh);
    if (fresh != rings) { rings = std::move (fresh); repaint(); }
}

void ModulationOverlay::paint (Graphics& g) {
    for (const auto& ring : rings) {
        const auto bounds = ring.bounds.reduced (10.0f);
        const auto radius = jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
        if (radius < 4.0f) continue;
        const auto lineW = jmin (8.0f, radius * 0.5f);
        const auto r = radius + 3.0f - lineW * 0.25f;
        const auto c = bounds.getCentre();
        const auto angle = [&ring] (float v) { return ring.startAngle + v * (ring.endAngle - ring.startAngle); };
        Path range;
        range.addCentredArc (c.x, c.y, r, r, 0.0f, angle (ring.low), angle (ring.high), true);
        g.setColour (Theme::modulation.withAlpha (0.35f));
        g.strokePath (range, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));
        Path moved;
        moved.addCentredArc (c.x, c.y, r, r, 0.0f, angle (ring.base), angle (ring.current), true);
        g.setColour (Theme::modulation);
        g.strokePath (moved, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));
        const auto dot = c.getPointOnCircumference (r, angle (ring.current));
        g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre (dot));
    }
}

class ModMatrixEditor::BarLookAndFeel final : public LookAndFeel_V4 {
public:
    BarLookAndFeel() {
        setColour (ComboBox::backgroundColourId, Theme::panelLight);
        setColour (ComboBox::outlineColourId, Theme::border);
        setColour (ComboBox::textColourId, Theme::text);
        setColour (ComboBox::arrowColourId, Theme::dimText);
        setColour (PopupMenu::backgroundColourId, Theme::panel);
        setColour (PopupMenu::textColourId, Theme::text);
        setColour (PopupMenu::headerTextColourId, Theme::modulation);
        setColour (PopupMenu::highlightedBackgroundColourId, Theme::panelLight);
        setColour (PopupMenu::highlightedTextColourId, Theme::text);
        setColour (TooltipWindow::backgroundColourId, Theme::panelLight);
        setColour (TooltipWindow::textColourId, Theme::text);
        setColour (TooltipWindow::outlineColourId, Theme::border);
    }
    Font getComboBoxFont (ComboBox& box) override { return FontOptions (jmin (13.5f, (float) box.getHeight() * 0.6f)); }
    void positionComboBoxText (ComboBox& box, Label& label) override {
        label.setBounds (4, 1, box.getWidth() - 22, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }
    void drawComboBox (Graphics& g, int width, int height, bool down, int, int, int, int, ComboBox& box) override {
        const auto area = Rectangle<int> (width, height).toFloat();
        g.setColour (box.findColour (ComboBox::backgroundColourId));
        g.fillRoundedRectangle (area, 3.0f);
        g.setColour (box.findColour (ComboBox::outlineColourId));
        g.drawRoundedRectangle (area.reduced (0.5f), 3.0f, 1.0f);
        Path arrow;
        const auto x = (float) width - 12.0f, y = (float) height * 0.5f;
        arrow.startNewSubPath (x - 4.0f, y - 2.0f);
        arrow.lineTo (x, y + 2.0f);
        arrow.lineTo (x + 4.0f, y - 2.0f);
        g.setColour (box.findColour (ComboBox::arrowColourId).withAlpha (down || box.isMouseOver() ? 1.0f : 0.7f));
        g.strokePath (arrow, PathStrokeType (1.5f));
    }
    void drawLinearSlider (Graphics& g, int x, int y, int width, int height, float sliderPos, float minPos, float maxPos,
                           Slider::SliderStyle style, Slider& slider) override {
        if (style != Slider::LinearBar) {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minPos, maxPos, style, slider);
            return;
        }
        const auto area = Rectangle<int> (x, y, width, height).toFloat();
        g.setColour (Theme::panelLight);
        g.fillRoundedRectangle (area, 3.0f);
        const auto zero = area.getX() + area.getWidth() * (float) slider.valueToProportionOfLength (0.0);
        const auto left = jmin (zero, sliderPos), right = jmax (zero, sliderPos);
        g.setColour (Theme::modulation.withAlpha (slider.isEnabled() ? 0.75f : 0.3f));
        g.fillRect (Rectangle<float> (left, area.getY() + 1.0f, right - left, area.getHeight() - 2.0f));
        g.setColour (Theme::border.brighter (0.4f));
        g.fillRect (Rectangle<float> (zero - 0.5f, area.getY(), 1.0f, area.getHeight()));
    }
};

class ModMatrixEditor::LfoPanel final : public Component {
public:
    LfoPanel (ReverseSamplerProcessor& p, int index, LookAndFeel& bars) : processor (p), lfo (index) {
        for (auto* c : { &shape, &trigger, &division }) c->setLookAndFeel (&bars);
        auto& st = p.getValueTreeState();
        const auto id = [index] (const char* n) { return ModMatrix::lfoID (index, n); };
        fillChoices (shape, st.getParameter (id ("Shape")));
        fillChoices (trigger, st.getParameter (id ("Trig")));
        fillChoices (division, st.getParameter (id ("Div")));
        for (auto* c : { &shape, &trigger }) addAndMakeVisible (*c);
        shape.setTooltip ("Wave shape. S&H holds a new random value each cycle, Random glides between them.");
        trigger.setTooltip ("Free: runs all the time (synced: locked to the host bar). Retrig: restarts with every note. One Shot: one cycle per note, then holds.");
        division.setTooltip ("LFO cycle length in note values of the host tempo");
        styleToggle (sync);
        sync.setTooltip ("Sync the rate to the host tempo");
        addAndMakeVisible (sync);
        shapeAttachment = std::make_unique<APVTS::ComboBoxAttachment> (st, id ("Shape"), shape);
        triggerAttachment = std::make_unique<APVTS::ComboBoxAttachment> (st, id ("Trig"), trigger);
        divisionAttachment = std::make_unique<APVTS::ComboBoxAttachment> (st, id ("Div"), division);
        syncAttachment = std::make_unique<APVTS::ButtonAttachment> (st, id ("Sync"), sync);
        rate = makeKnob (st, id ("Rate"), "Rate");
        phase = makeKnob (st, id ("Phase"), "Phase");
        smooth = makeKnob (st, id ("Smooth"), "Smooth");
        for (auto* k : { rate.get(), phase.get(), smooth.get() }) addAndMakeVisible (*k);
        addChildComponent (division);
    }

    ~LfoPanel() override {
        for (auto* c : { &shape, &trigger, &division }) c->setLookAndFeel (nullptr);
    }

    void refresh() {
        const bool synced = sync.getToggleState();
        rate->setVisible (! synced);
        division.setVisible (synced);
        const auto settings = processor.getModMatrix().readLfo (lfo);
        if (settings.shape != lastShape) { lastShape = settings.shape; rebuildShape(); }
        repaint (view.expanded (4).getUnion (meter));
    }

    void paint (Graphics& g) override {
        drawPanel (g, getLocalBounds(), "LFO " + String (lfo + 1));
        auto& matrix = processor.getModMatrix();
        const float value = matrix.sourceValue (rs::mod::lfo1 + lfo);
        drawSourceMeter (g, meter, value);
        g.setColour (Theme::background);
        g.fillRoundedRectangle (view.toFloat(), 4.0f);
        g.setColour (Theme::border);
        g.drawHorizontalLine (view.getCentreY(), (float) view.getX(), (float) view.getRight());
        auto curve = shapePath;
        curve.applyTransform (AffineTransform::scale ((float) view.getWidth() - 8.0f, (float) view.getHeight() - 8.0f).translated ((float) view.getX() + 4.0f, (float) view.getY() + 4.0f));
        g.setColour (Theme::modulation.withAlpha (0.85f));
        g.strokePath (curve, PathStrokeType (1.8f));
        const auto x = (float) view.getX() + 4.0f + ((float) view.getWidth() - 8.0f) * matrix.lfoPhase (lfo);
        const auto y = (float) view.getBottom() - 4.0f - ((float) view.getHeight() - 8.0f) * value;
        g.setColour (Theme::text);
        g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y }));
    }

    void resized() override {
        auto area = getLocalBounds().reduced (10, 6);
        auto top = area.removeFromTop (20);
        top.removeFromLeft (52);
        meter = top.removeFromLeft (60).withSizeKeepingCentre (60, 6);
        top.removeFromLeft (8);
        sync.setBounds (top.removeFromRight (50));
        top.removeFromRight (6);
        trigger.setBounds (top.removeFromRight (84));
        top.removeFromRight (6);
        shape.setBounds (top);
        area.removeFromTop (6);
        const int knobWidth = jmin (62, area.getWidth() / 6);
        auto knobs = area.removeFromRight (knobWidth * 3);
        area.removeFromRight (8);
        view = area;
        auto first = knobs.removeFromLeft (knobWidth);
        rate->setBounds (first);
        division.setBounds (first.withSizeKeepingCentre (first.getWidth(), 24));
        phase->setBounds (knobs.removeFromLeft (knobWidth));
        smooth->setBounds (knobs.removeFromLeft (knobWidth));
        rebuildShape();
    }

private:
    void rebuildShape() {
        shapePath.clear();
        constexpr int points = 160;
        for (int i = 0; i <= points; ++i) {
            const double position = (double) i / points;
            const float y = 1.0f - rs::mod::Lfo::shapePreview (lastShape, position);
            if (i == 0) shapePath.startNewSubPath ((float) position, y); else shapePath.lineTo ((float) position, y);
        }
    }

    ReverseSamplerProcessor& processor;
    const int lfo;
    ComboBox shape, trigger, division;
    TextButton sync { "SYNC" };
    std::unique_ptr<LabelledKnob> rate, phase, smooth;
    std::unique_ptr<APVTS::ComboBoxAttachment> shapeAttachment, triggerAttachment, divisionAttachment;
    std::unique_ptr<APVTS::ButtonAttachment> syncAttachment;
    Rectangle<int> view, meter;
    Path shapePath;
    int lastShape = -1;
};

class ModMatrixEditor::EnvelopePanel final : public Component {
public:
    explicit EnvelopePanel (ReverseSamplerProcessor& p) : processor (p) {
        auto& st = p.getValueTreeState();
        attack = makeKnob (st, ModMatrix::envAttack, "Attack");
        decay = makeKnob (st, ModMatrix::envDecay, "Decay");
        sustain = makeKnob (st, ModMatrix::envSustain, "Sustain");
        release = makeKnob (st, ModMatrix::envRelease, "Release");
        for (auto* k : { attack.get(), decay.get(), sustain.get(), release.get() }) addAndMakeVisible (*k);
    }

    void refresh() { repaint(); }

    void paint (Graphics& g) override {
        drawPanel (g, getLocalBounds(), "ENVELOPE");
        auto& matrix = processor.getModMatrix();
        const float level = matrix.sourceValue (rs::mod::envelope);
        drawSourceMeter (g, meter, level);
        g.setColour (Theme::background);
        g.fillRoundedRectangle (view.toFloat(), 4.0f);
        const auto s = matrix.readEnvelope();
        const auto width = [] (float ms) { return std::sqrt (jmax (0.0f, ms) / 10000.0f); };
        const float a = width (s.attackMs), d = width (s.decayMs), r = width (s.releaseMs), hold = 0.35f;
        const float total = jmax (0.001f, a + d + hold + r);
        const auto plot = view.toFloat().reduced (5.0f);
        const auto px = [&] (float t) { return plot.getX() + plot.getWidth() * t / total; };
        const auto py = [&] (float v) { return plot.getBottom() - plot.getHeight() * v; };
        const float sus = jlimit (0.0f, 1.0f, s.sustain);
        const std::array<Point<float>, 5> points { Point<float> { px (0.0f), py (0.0f) }, { px (a), py (1.0f) }, { px (a + d), py (sus) },
                                                    { px (a + d + hold), py (sus) }, { px (total), py (0.0f) } };
        const int stage = matrix.envelopeStage();
        for (int k = 0; k < 4; ++k) {
            const bool current = stage == k + 1;
            g.setColour (Theme::modulation.withAlpha (current ? 1.0f : 0.6f));
            g.drawLine ({ points[(size_t) k], points[(size_t) k + 1] }, current ? 2.6f : 1.6f);
        }
        if (stage != 0) {
            g.setColour (Theme::text.withAlpha (0.5f));
            g.drawHorizontalLine (roundToInt (py (level)), plot.getX(), plot.getRight());
        }
    }

    void resized() override {
        auto area = getLocalBounds().reduced (10, 6);
        auto top = area.removeFromTop (20);
        top.removeFromLeft (80);
        meter = top.removeFromLeft (60).withSizeKeepingCentre (60, 6);
        area.removeFromTop (6);
        const int knobWidth = jmin (62, area.getWidth() / 7);
        auto knobs = area.removeFromRight (knobWidth * 4);
        area.removeFromRight (8);
        view = area;
        for (auto* k : { attack.get(), decay.get(), sustain.get(), release.get() }) k->setBounds (knobs.removeFromLeft (knobWidth));
    }

private:
    ReverseSamplerProcessor& processor;
    std::unique_ptr<LabelledKnob> attack, decay, sustain, release;
    Rectangle<int> view, meter;
};

class ModMatrixEditor::MacroPanel final : public Component {
public:
    MacroPanel (ReverseSamplerProcessor& p, std::function<void()> namesChanged) : processor (p), onNamesChanged (std::move (namesChanged)) {
        auto& st = p.getValueTreeState();
        for (int m = 0; m < rs::mod::numMacros; ++m) {
            auto& knob = knobs[(size_t) m];
            knob = makeKnob (st, ModMatrix::macroID (m), p.getMacroName (m));
            knob->label.setEditable (false, true, false);
            knob->label.setInterceptsMouseClicks (true, true);
            knob->label.setTooltip ("Double-click to rename");
            knob->label.onTextChange = [this, m] {
                processor.setMacroName (m, knobs[(size_t) m]->label.getText());
                knobs[(size_t) m]->label.setText (processor.getMacroName (m), dontSendNotification);
                if (onNamesChanged) onNamesChanged();
            };
            addAndMakeVisible (*knob);
        }
    }

    void refresh() {
        for (int m = 0; m < rs::mod::numMacros; ++m)
            if (! knobs[(size_t) m]->label.isBeingEdited())
                knobs[(size_t) m]->label.setText (processor.getMacroName (m), dontSendNotification);
    }

    void paint (Graphics& g) override { drawPanel (g, getLocalBounds(), "MACROS"); }

    void resized() override {
        auto area = getLocalBounds().reduced (10, 6);
        area.removeFromTop (20);
        const int width = area.getWidth() / rs::mod::numMacros;
        for (auto& k : knobs) k->setBounds (area.removeFromLeft (width).reduced (4, 0));
    }

private:
    ReverseSamplerProcessor& processor;
    std::function<void()> onNamesChanged;
    std::array<std::unique_ptr<LabelledKnob>, rs::mod::numMacros> knobs;
};

class ModMatrixEditor::Row final : public Component {
public:
    Row (ReverseSamplerProcessor& p, int index, LookAndFeel& bars) : processor (p), row (index), destination (p) {
        auto& st = p.getValueTreeState();
        for (int s = 0; s < rs::mod::numSources; ++s) {
            source.addItem (sourceName (p, s), s + 1);
            aux.addItem (s == 0 ? String ("-") : sourceName (p, s), s + 1);
        }
        const auto& list = ModMatrix::destinations();
        for (int d = 0; d < (int) list.size(); ++d) destination.addItem (modui::destinationLabel (p, d), d + 1);
        source.setTooltip ("What moves the destination");
        aux.setTooltip ("Optional second source that scales this row (e.g. Mod Wheel to fade an LFO in)");
        destination.setTooltip ("The parameter this row moves, grouped by Global / Layer / loaded FX / LFO. Right-click a knob to add a row for it.");
        for (auto* c : std::initializer_list<ComboBox*> { &source, &aux, &destination }) { c->setLookAndFeel (&bars); addAndMakeVisible (*c); }

        for (auto* s : { &amount, &curve }) {
            s->setSliderStyle (Slider::LinearBar);
            s->setLookAndFeel (&bars);
            s->setColour (Slider::textBoxTextColourId, Theme::text);
            s->setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
            addAndMakeVisible (*s);
        }
        amount.setTooltip ("How far the source moves the destination, as a share of its whole range. Double-click: 0.");
        curve.setTooltip ("Bends the source: positive keeps it low longer, negative rises quickly. Double-click: linear.");
        amount.setDoubleClickReturnValue (true, 0.0);
        curve.setDoubleClickReturnValue (true, 0.0);
        styleToggle (bipolar);
        bipolar.setTooltip ("Bipolar: the source swings the destination both ways around its knob position");
        addAndMakeVisible (bipolar);
        clear.setTooltip ("Clear this row");
        {
            Path cross;
            cross.addLineSegment ({ 0.0f, 0.0f, 1.0f, 1.0f }, 0.16f);
            cross.addLineSegment ({ 1.0f, 0.0f, 0.0f, 1.0f }, 0.16f);
            clear.setShape (cross, false, true, false);
            clear.setBorderSize (BorderSize<int> (7));
        }
        clear.setWantsKeyboardFocus (false);
        clear.onClick = [this] { processor.clearModulationRow (row); };
        addAndMakeVisible (clear);

        sourceAttachment = std::make_unique<APVTS::ComboBoxAttachment> (st, ModMatrix::rowID (row, "Src"), source);
        auxAttachment = std::make_unique<APVTS::ComboBoxAttachment> (st, ModMatrix::rowID (row, "Aux"), aux);
        destinationAttachment = std::make_unique<APVTS::ComboBoxAttachment> (st, ModMatrix::rowID (row, "Dst"), destination);
        amountAttachment = std::make_unique<APVTS::SliderAttachment> (st, ModMatrix::rowID (row, "Amt"), amount);
        curveAttachment = std::make_unique<APVTS::SliderAttachment> (st, ModMatrix::rowID (row, "Curve"), curve);
        bipolarAttachment = std::make_unique<APVTS::ButtonAttachment> (st, ModMatrix::rowID (row, "Bip"), bipolar);
    }

    ~Row() override {
        for (auto* c : std::initializer_list<Component*> { &source, &aux, &destination, &amount, &curve }) c->setLookAndFeel (nullptr);
    }

    void layout (const std::array<int, 9>& x) {
        const auto cell = [this, &x] (int column) { return Rectangle<int> (x[(size_t) column], 0, x[(size_t) column + 1] - x[(size_t) column], getHeight()).reduced (3, 3); };
        source.setBounds (cell (1));
        aux.setBounds (cell (2));
        destination.setBounds (cell (3));
        amount.setBounds (cell (4));
        curve.setBounds (cell (5));
        bipolar.setBounds (cell (6));
        meter = cell (7).withSizeKeepingCentre (cell (7).getWidth(), 8);
        clear.setBounds (Rectangle<int> (x[8], 0, getWidth() - x[8], getHeight()).reduced (3, 3));
        number = Rectangle<int> (0, 0, x[1], getHeight());
    }

    void refresh() {
        const auto settings = processor.getModMatrix().readRow (row);
        const bool nowActive = settings.source != rs::mod::none && settings.destination > 0 && std::abs (settings.amount) >= 1.0e-6f;
        const float nowValue = processor.getModMatrix().rowValue (row);
        const bool nowUsed = settings.source != rs::mod::none || settings.destination > 0;
        if (nowActive != active || nowUsed != used || ! exactlyEqual (nowValue, value)) {
            active = nowActive; used = nowUsed; value = nowValue;
            for (auto* c : std::initializer_list<Component*> { &aux, &amount, &curve, &bipolar }) c->setAlpha (used ? 1.0f : 0.45f);
            clear.setEnabled (used || std::abs (settings.amount) >= 1.0e-6f);
            repaint();
        }
    }

    void setDestinationNames() {
        for (int d = 1; d < ModMatrix::numDestinations(); ++d) destination.changeItemText (d + 1, modui::destinationLabel (processor, d));
    }

    void setSourceNames() {
        for (int s = rs::mod::macro1; s <= rs::mod::macro4; ++s) {
            source.changeItemText (s + 1, sourceName (processor, s));
            aux.changeItemText (s + 1, sourceName (processor, s));
        }
    }

    void paint (Graphics& g) override {
        if (row % 2 == 1) { g.setColour (Theme::panelLight.withAlpha (0.25f)); g.fillRect (getLocalBounds()); }
        g.setColour (active ? Theme::modulation : Theme::dimText);
        g.setFont (FontOptions (12.0f, active ? Font::bold : Font::plain));
        g.drawText (String (row + 1), number, Justification::centred);
        const auto bar = meter.toFloat();
        g.setColour (Theme::background);
        g.fillRoundedRectangle (bar, 2.0f);
        const float centre = bar.getCentreX();
        const float end = centre + jlimit (-1.0f, 1.0f, value) * bar.getWidth() * 0.5f;
        g.setColour (Theme::modulation);
        g.fillRect (Rectangle<float> (jmin (centre, end), bar.getY(), std::abs (end - centre), bar.getHeight()));
        g.setColour (Theme::border.brighter (0.4f));
        g.fillRect (Rectangle<float> (centre - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f));
    }

private:
    ReverseSamplerProcessor& processor;
    const int row;
    ComboBox source, aux;
    DestinationBox destination;
    Slider amount { Slider::LinearBar, Slider::TextBoxBelow }, curve { Slider::LinearBar, Slider::TextBoxBelow };
    TextButton bipolar { CharPointer_UTF8 ("\xc2\xb1") };
    ShapeButton clear { "clear", Theme::dimText, Theme::text, Theme::modulation };
    std::unique_ptr<APVTS::ComboBoxAttachment> sourceAttachment, auxAttachment, destinationAttachment;
    std::unique_ptr<APVTS::SliderAttachment> amountAttachment, curveAttachment;
    std::unique_ptr<APVTS::ButtonAttachment> bipolarAttachment;
    Rectangle<int> meter, number;
    bool active = false, used = false;
    float value = 0.0f;
};

ModMatrixEditor::ModMatrixEditor (ReverseSamplerProcessor& p) : processor (p), barLookAndFeel (std::make_unique<BarLookAndFeel>()) {
    close.onClick = [this] { if (onClose) onClose(); };
    close.setWantsKeyboardFocus (false);
    addAndMakeVisible (close);
    for (int l = 0; l < rs::mod::numLfos; ++l) addAndMakeVisible (*(lfoPanels[(size_t) l] = std::make_unique<LfoPanel> (p, l, *barLookAndFeel)));
    envelopePanel = std::make_unique<EnvelopePanel> (p);
    addAndMakeVisible (*envelopePanel);
    macroPanel = std::make_unique<MacroPanel> (p, [this] {
        for (auto& r : rows) r->setSourceNames();
        if (onMacroNamesChanged) onMacroNamesChanged();
    });
    addAndMakeVisible (*macroPanel);
    for (int r = 0; r < rs::mod::numRows; ++r) addAndMakeVisible (*(rows[(size_t) r] = std::make_unique<Row> (p, r, *barLookAndFeel)));
    timerCallback();
    startTimerHz (30);
}

ModMatrixEditor::~ModMatrixEditor() {
    stopTimer();
    for (auto& r : rows) r = nullptr;
}

void ModMatrixEditor::timerCallback() {
    for (auto& l : lfoPanels) l->refresh();
    envelopePanel->refresh();
    macroPanel->refresh();
    String signature;
    for (int s = 0; s < rs::fxSlots; ++s) signature << fxTypeOf (processor, s) << ",";
    for (int m = 0; m < rs::mod::numMacros; ++m) signature << processor.getMacroName (m) << ",";
    const bool renamed = signature != fxSignature;
    fxSignature = signature;
    for (auto& r : rows) {
        if (renamed) { r->setDestinationNames(); r->setSourceNames(); }
        r->refresh();
    }
}

void ModMatrixEditor::paint (Graphics& g) {
    g.fillAll (Theme::background);
    g.setColour (Theme::text);
    g.setFont (FontOptions (20.0f, Font::bold));
    g.drawText ("MOD MATRIX", getLocalBounds().reduced (8, 4).removeFromTop (30), Justification::centred);
    g.setColour (Theme::dimText);
    g.setFont (FontOptions (12.0f));
    g.drawText ("Right-click any knob to modulate it", getLocalBounds().reduced (12, 4).removeFromTop (30), Justification::centredRight);

    drawPanel (g, matrixArea, "ROUTING");
    g.setColour (Theme::dimText);
    g.setFont (FontOptions (11.5f, Font::bold));
    const char* titles[] { "#", "SOURCE", "AUX", "DESTINATION", "AMOUNT", "CURVE", "BIP", "OUTPUT" };
    for (int c = 0; c < 8; ++c)
        g.drawText (titles[c], Rectangle<int> (headerArea.getX() + columnX[(size_t) c], headerArea.getY(), columnX[(size_t) c + 1] - columnX[(size_t) c], headerArea.getHeight()),
                    c == 0 ? Justification::centred : Justification::centredLeft);
}

void ModMatrixEditor::resized() {
    auto area = getLocalBounds().reduced (4);
    auto top = area.removeFromTop (30);
    close.setBounds (top.removeFromLeft (70).reduced (0, 3));
    area.removeFromTop (6);

    auto left = area.removeFromLeft (jlimit (360, 460, area.getWidth() * 9 / 25));
    area.removeFromLeft (8);
    const int macroHeight = 104, envelopeHeight = jmin (130, (left.getHeight() - macroHeight - 24) / 3);
    const int lfoHeight = (left.getHeight() - macroHeight - envelopeHeight - 24) / 2;
    for (auto& l : lfoPanels) { l->setBounds (left.removeFromTop (lfoHeight)); left.removeFromTop (8); }
    envelopePanel->setBounds (left.removeFromTop (envelopeHeight));
    left.removeFromTop (8);
    macroPanel->setBounds (left.removeFromTop (macroHeight));

    matrixArea = area;
    auto inner = area.reduced (8, 6);
    inner.removeFromTop (20);
    headerArea = inner.removeFromTop (18);
    const int w = inner.getWidth();
    const int amountWidth = jlimit (76, 120, w / 7), curveWidth = jlimit (58, 90, w / 10);
    const int flexible = w - (28 + amountWidth + curveWidth + 40 + 70 + 26);
    const int sourceWidth = flexible * 31 / 100, auxWidth = flexible * 22 / 100;
    const int destinationWidth = flexible - sourceWidth - auxWidth;
    const int widths[] { 28, sourceWidth, auxWidth, destinationWidth, amountWidth, curveWidth, 40, 70 };
    columnX[0] = 0;
    for (int c = 0; c < 8; ++c) columnX[(size_t) c + 1] = columnX[(size_t) c] + widths[c];
    const int rowHeight = jlimit (20, 30, inner.getHeight() / rs::mod::numRows);
    for (auto& r : rows) {
        r->setBounds (inner.removeFromTop (rowHeight));
        r->layout (columnX);
    }
}
