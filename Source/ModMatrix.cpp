#include "ModMatrix.h"
#include "FxRack.h"

namespace rs {
namespace {
using namespace juce;

String layerParamID (int layer, const char* name) { return "l" + String (layer + 1) + "_" + name; }

NormalisableRange<float> logRange (float min, float max) {
    return { min, max,
             [] (float start, float end, float normalised) { return start * std::pow (end / start, normalised); },
             [] (float start, float end, float value) { return std::log (value / start) / std::log (end / start); },
             [] (float start, float end, float value) { return jlimit (start, end, value); } };
}
NormalisableRange<float> centred (float min, float max, float centre) {
    NormalisableRange<float> r (min, max);
    r.setSkewForCentre (centre);
    return r;
}
String milliseconds (float ms) { return ms < 1000.0f ? String (ms, ms < 10.0f ? 1 : 0) + " ms" : String (ms * 0.001f, 2) + " s"; }
float parseMilliseconds (const String& t) {
    const auto v = t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue();
    return t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s") ? v : v * 1000.0f;
}
StringArray onOff() { return { "Off", "On" }; }
}

const std::vector<ModMatrix::Destination>& ModMatrix::destinations() {
    static const std::vector<Destination> list = [] {
        std::vector<Destination> d;
        const auto make = [] (const String& id, const String& name) { Destination e; e.paramID = id; e.name = name; return e; };
        d.push_back (make ("", "None"));
        d.push_back (make ("gain", "Volume"));
        jassert ((int) d.size() - 1 == gainDestination);
        const char* layerParams[][2] { { "level", "Level" }, { "pan", "Pan" }, { "width", "Width" }, { "lowCut", "Low Cut" }, { "highCut", "High Cut" } };
        for (int layer = 0; layer < 2; ++layer)
            for (const auto& p : layerParams) {
                auto entry = make (layerParamID (layer, p[0]), "Layer " + String (layer + 1) + " " + p[1]);
                entry.layer = layer;
                d.push_back (entry);
            }
        const char* fxParams[][2] { { "p1", "Knob 1" }, { "p2", "Knob 2" }, { "p3", "Knob 3" }, { "mix", "Mix" },
                                    { "diffusion", "Diffusion" }, { "bias", "Bias" },
                                    { "eqAir", "EQ Air" }, { "eqLowCut", "EQ Low Cut" }, { "eqHighCut", "EQ High Cut" },
                                    { "rvPreDelay", "Reverb Pre-Delay" }, { "rvLowCut", "Reverb Low Cut" }, { "rvChar", "Reverb Character" },
                                    { "chDelay", "Chorus Delay" }, { "chFeedback", "Chorus Feedback" }, { "chOutput", "Chorus Output" },
                                    { "chVibRate", "Chorus Vibrato Rate" }, { "chVibDepth", "Chorus Vibrato Depth" },
                                    { "cpKnee", "Compressor Knee" }, { "cpAttack", "Compressor Attack" }, { "cpRelease", "Compressor Release" } };
        for (int slot = 0; slot < fxSlots; ++slot)
            for (const auto& p : fxParams) {
                auto entry = make (fxID (slot, p[0]), "FX " + String (slot + 1) + " " + p[1]);
                entry.fxSlot = slot;
                entry.fxParameter = p[0];
                d.push_back (entry);
            }
        for (int lfo = 0; lfo < mod::numLfos; ++lfo)
            d.push_back (make (lfoID (lfo, "Rate"), "LFO " + String (lfo + 1) + " Rate"));
        return d;
    }();
    return list;
}

int ModMatrix::destinationIndex (const String& paramID) {
    const auto& list = destinations();
    for (size_t i = 1; i < list.size(); ++i)
        if (list[i].paramID == paramID) return (int) i;
    return 0;
}

StringArray ModMatrix::allParameterIDs() {
    StringArray ids;
    for (int m = 0; m < mod::numMacros; ++m) ids.add (macroID (m));
    for (int l = 0; l < mod::numLfos; ++l)
        for (const auto* n : { "Shape", "Rate", "Sync", "Div", "Trig", "Phase", "Smooth" }) ids.add (lfoID (l, n));
    for (const auto* n : { envAttack, envDecay, envSustain, envRelease }) ids.add (n);
    for (int r = 0; r < mod::numRows; ++r)
        for (const auto* n : rowFields) ids.add (rowID (r, n));
    return ids;
}

void ModMatrix::addParameters (AudioProcessorValueTreeState::ParameterLayout& layout) {
    using Float = AudioParameterFloat;
    using Choice = AudioParameterChoice;
    using Attributes = AudioParameterFloatAttributes;
    const auto id = [] (const String& name) { return ParameterID { name, 15 }; };
    const auto percent = Attributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " %"; });
    const auto signedPercent = Attributes().withStringFromValueFunction ([] (float v, int) { return (v > 0.05f ? "+" : "") + String (v, 1) + " %"; });
    const auto ms = Attributes().withStringFromValueFunction ([] (float v, int) { return milliseconds (v); })
                                .withValueFromStringFunction ([] (const String& t) { return parseMilliseconds (t); });

    for (int m = 0; m < mod::numMacros; ++m)
        layout.add (std::make_unique<Float> (id (macroID (m)), "Macro " + String (m + 1), NormalisableRange<float> (0.0f, 100.0f, 0.01f), 0.0f, percent));

    StringArray shapes, divisions, triggers, sourcesList, destinationNames;
    for (const auto* s : mod::shapeNames) shapes.add (s);
    for (const auto* s : mod::divisionNames) divisions.add (s);
    for (const auto* s : mod::triggerNames) triggers.add (s);
    for (const auto* s : mod::sourceNames) sourcesList.add (s);
    for (const auto& d : destinations()) destinationNames.add (d.name);

    for (int l = 0; l < mod::numLfos; ++l) {
        const String name = "LFO " + String (l + 1) + " ";
        layout.add (std::make_unique<Choice> (id (lfoID (l, "Shape")), name + "Shape", shapes, 0));
        layout.add (std::make_unique<Float> (id (lfoID (l, "Rate")), name + "Rate", logRange (mod::minRateHz, mod::maxRateHz), 1.0f,
                                             Attributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 1.0f ? 2 : 1) + " Hz"; })
                                                         .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue(); })));
        layout.add (std::make_unique<Choice> (id (lfoID (l, "Sync")), name + "Sync", onOff(), 0));
        layout.add (std::make_unique<Choice> (id (lfoID (l, "Div")), name + "Division", divisions, 7));
        layout.add (std::make_unique<Choice> (id (lfoID (l, "Trig")), name + "Trigger", triggers, 0));
        layout.add (std::make_unique<Float> (id (lfoID (l, "Phase")), name + "Phase", NormalisableRange<float> (0.0f, 360.0f, 1.0f), 0.0f,
                                             Attributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + String (CharPointer_UTF8 ("\xc2\xb0")); })));
        layout.add (std::make_unique<Float> (id (lfoID (l, "Smooth")), name + "Smooth", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, percent));
    }

    layout.add (std::make_unique<Float> (id (envAttack), "Envelope Attack", centred (0.0f, 10000.0f, 400.0f), 10.0f, ms));
    layout.add (std::make_unique<Float> (id (envDecay), "Envelope Decay", centred (1.0f, 10000.0f, 600.0f), 300.0f, ms));
    layout.add (std::make_unique<Float> (id (envSustain), "Envelope Sustain", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 70.0f, percent));
    layout.add (std::make_unique<Float> (id (envRelease), "Envelope Release", centred (1.0f, 10000.0f, 800.0f), 500.0f, ms));

    for (int r = 0; r < mod::numRows; ++r) {
        const String name = "Mod " + String (r + 1) + " ";
        layout.add (std::make_unique<Choice> (id (rowID (r, "Src")), name + "Source", sourcesList, 0));
        layout.add (std::make_unique<Choice> (id (rowID (r, "Aux")), name + "Aux Source", sourcesList, 0));
        layout.add (std::make_unique<Choice> (id (rowID (r, "Dst")), name + "Destination", destinationNames, 0));
        layout.add (std::make_unique<Float> (id (rowID (r, "Amt")), name + "Amount", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, signedPercent));
        layout.add (std::make_unique<Float> (id (rowID (r, "Curve")), name + "Curve", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f,
                                             Attributes().withStringFromValueFunction ([] (float v, int) { return std::abs (v) < 0.05f ? String ("Linear") : (v > 0.0f ? "+" : "") + String (v, 0); })));
        layout.add (std::make_unique<Choice> (id (rowID (r, "Bip")), name + "Bipolar", onOff(), 0));
    }
}

void ModMatrix::connect (AudioProcessorValueTreeState& state) {
    targets.clear();
    for (const auto& d : destinations()) {
        auto target = std::make_unique<Target>();
        if (d.paramID.isNotEmpty()) {
            target->parameter = state.getParameter (d.paramID);
            target->base = state.getRawParameterValue (d.paramID);
            if (target->base != nullptr) target->effective.store (target->base->load());
        }
        targets.push_back (std::move (target));
    }
    for (int r = 0; r < mod::numRows; ++r)
        for (int f = 0; f < 6; ++f) rows[(size_t) r].field[f] = state.getRawParameterValue (rowID (r, rowFields[f]));
    for (int m = 0; m < mod::numMacros; ++m) macros[(size_t) m] = state.getRawParameterValue (macroID (m));
    for (int l = 0; l < mod::numLfos; ++l) {
        auto& p = lfoParams[(size_t) l];
        p.shape = state.getRawParameterValue (lfoID (l, "Shape"));
        p.rate = resolve (lfoID (l, "Rate"));
        p.sync = state.getRawParameterValue (lfoID (l, "Sync"));
        p.division = state.getRawParameterValue (lfoID (l, "Div"));
        p.trigger = state.getRawParameterValue (lfoID (l, "Trig"));
        p.phase = state.getRawParameterValue (lfoID (l, "Phase"));
        p.smooth = state.getRawParameterValue (lfoID (l, "Smooth"));
    }
    envParams = { state.getRawParameterValue (envAttack), state.getRawParameterValue (envDecay),
                  state.getRawParameterValue (envSustain), state.getRawParameterValue (envRelease) };
}

std::atomic<float>* ModMatrix::resolve (const String& paramID) noexcept {
    const int index = destinationIndex (paramID);
    if (index <= 0 || index >= (int) targets.size() || targets[(size_t) index]->base == nullptr) return nullptr;
    return &targets[(size_t) index]->effective;
}

void ModMatrix::prepare (double sampleRate) {
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (auto& l : lfos) l.prepare (fs);
    envelope.prepare (fs);
    resetSources();
    passThrough();
}

void ModMatrix::resetSources() noexcept {
    for (auto& l : lfos) l.reset();
    envelope.reset();
    held.fill (false); sustained.fill (false); heldCount = 0; pedal = false;
    sources.fill (0.0f);
    sources[(size_t) mod::pitchBend] = 0.5f;
    lastBuild = 0.0f;
}

mod::RowSettings ModMatrix::readRow (int row) const noexcept {
    mod::RowSettings s;
    const auto& f = rows[(size_t) jlimit (0, mod::numRows - 1, row)].field;
    if (f[0] == nullptr) return s;
    s.source = jlimit (0, (int) mod::numSources - 1, roundToInt (f[0]->load()));
    s.aux = jlimit (0, (int) mod::numSources - 1, roundToInt (f[1]->load()));
    s.destination = jlimit (0, numDestinations() - 1, roundToInt (f[2]->load()));
    s.amount = f[3]->load() * 0.01f;
    s.curve = f[4]->load() * 0.01f;
    s.bipolar = f[5]->load() >= 0.5f;
    return s;
}

mod::LfoSettings ModMatrix::readLfo (int lfo) const noexcept {
    mod::LfoSettings s;
    const auto& p = lfoParams[(size_t) jlimit (0, mod::numLfos - 1, lfo)];
    if (p.shape == nullptr) return s;
    s.shape = jlimit (0, (int) mod::numShapes - 1, roundToInt (p.shape->load()));
    s.rateHz = p.rate != nullptr ? p.rate->load() : 1.0f;
    s.sync = p.sync->load() >= 0.5f;
    s.division = jlimit (0, mod::numDivisions - 1, roundToInt (p.division->load()));
    s.trigger = jlimit (0, (int) mod::numTriggers - 1, roundToInt (p.trigger->load()));
    s.phase = p.phase->load() / 360.0f;
    s.smooth = p.smooth->load() * 0.01f;
    return s;
}

mod::EnvelopeSettings ModMatrix::readEnvelope() const noexcept {
    mod::EnvelopeSettings s;
    if (envParams[0] == nullptr) return s;
    s.attackMs = envParams[0]->load(); s.decayMs = envParams[1]->load();
    s.sustain = envParams[2]->load() * 0.01f; s.releaseMs = envParams[3]->load();
    return s;
}

bool ModMatrix::refreshActive() noexcept {
    bool any = false;
    layerModulated = {};
    fxModulated = gainModulated = false;
    numRouted = 0;
    const auto& list = destinations();
    for (int r = 0; r < mod::numRows; ++r) {
        rowCache[(size_t) r] = readRow (r);
        const auto& row = rowCache[(size_t) r];
        if (row.source == mod::none || row.destination <= 0 || std::abs (row.amount) < 1.0e-6f) continue;
        any = true;
        const auto& d = list[(size_t) row.destination];
        if (d.layer >= 0) layerModulated[(size_t) d.layer] = true;
        if (d.fxSlot >= 0) fxModulated = true;
        if (row.destination == gainDestination) gainModulated = true;
        bool known = false;
        for (int k = 0; k < numRouted && ! known; ++k) known = routedTargets[(size_t) k] == row.destination;
        if (! known) routedTargets[(size_t) numRouted++] = row.destination;
    }
    active = any;
    return any;
}

void ModMatrix::passThrough() noexcept {
    for (auto& t : targets) {
        if (t->base == nullptr) continue;
        t->effective.store (t->base->load(), std::memory_order_relaxed);
        t->offset.store (0.0f, std::memory_order_relaxed);
    }
    for (auto& r : rowDisplay) r.store (0.0f, std::memory_order_relaxed);
}

void ModMatrix::beginBlock (const MidiBuffer& buffer, const BlockInfo& info) noexcept {
    midi = &buffer;
    block = info;
    for (int l = 0; l < mod::numLfos; ++l) lfoCache[(size_t) l] = readLfo (l);
    envelopeCache = readEnvelope();
    for (size_t i = 0; i < targets.size(); ++i) {
        auto& t = *targets[i];
        if (t.base == nullptr) continue;
        bool routed = false;
        for (int k = 0; k < numRouted && ! routed; ++k) routed = routedTargets[(size_t) k] == (int) i;
        if (routed) continue;
        t.effective.store (t.base->load (std::memory_order_relaxed), std::memory_order_relaxed);
        t.offset.store (0.0f, std::memory_order_relaxed);
    }
}

void ModMatrix::noteOn (int note, float velocity) noexcept {
    if (! held[(size_t) note]) { held[(size_t) note] = true; ++heldCount; }
    sustained[(size_t) note] = false;
    sources[(size_t) mod::velocity] = jlimit (0.0f, 1.0f, velocity);
    sources[(size_t) mod::note] = (float) note / 127.0f;
    sources[(size_t) mod::random] = random.next();
    envelope.noteOn();
    for (int l = 0; l < mod::numLfos; ++l) lfos[(size_t) l].noteOn (lfoCache[(size_t) l]);
}

void ModMatrix::noteOff (int note) noexcept {
    if (! held[(size_t) note]) return;
    if (pedal) { sustained[(size_t) note] = true; return; }
    held[(size_t) note] = false;
    if (--heldCount <= 0) { heldCount = 0; envelope.noteOff(); }
}

void ModMatrix::releaseAll() noexcept {
    held.fill (false); sustained.fill (false); heldCount = 0;
    envelope.noteOff();
}

void ModMatrix::handleMidi (const MidiMessage& m) noexcept {
    if (m.isNoteOn()) noteOn (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff()) noteOff (m.getNoteNumber());
    else if (m.isAllNotesOff() || m.isAllSoundOff()) releaseAll();
    else if (m.isSustainPedalOn()) pedal = true;
    else if (m.isSustainPedalOff()) {
        pedal = false;
        for (int n = 0; n < 128; ++n)
            if (sustained[(size_t) n]) { sustained[(size_t) n] = false; noteOff (n); }
    }
    else if (m.isControllerOfType (1)) sources[(size_t) mod::modWheel] = (float) m.getControllerValue() / 127.0f;
    else if (m.isChannelPressure()) sources[(size_t) mod::aftertouch] = (float) m.getChannelPressureValue() / 127.0f;
    else if (m.isAftertouch()) sources[(size_t) mod::aftertouch] = (float) m.getAfterTouchValue() / 127.0f;
    else if (m.isPitchWheel()) sources[(size_t) mod::pitchBend] = (float) m.getPitchWheelValue() / 16383.0f;
}

void ModMatrix::processChunk (int start, int length) noexcept {
    if (midi != nullptr)
        for (auto it = midi->findNextSamplePosition (start); it != midi->cend(); ++it) {
            const auto event = *it;
            if (event.samplePosition >= start + length) break;
            handleMidi (event.getMessage());
        }

    const double ppqAtEnd = block.ppqStart + (double) (start + length) * block.bpm / 60.0 / fs;
    for (int l = 0; l < mod::numLfos; ++l) {
        sources[(size_t) (mod::lfo1 + l)] = lfos[(size_t) l].advance (lfoCache[(size_t) l], length, block.bpm, block.playing, ppqAtEnd);
        lfoDisplayPhase[(size_t) l].store ((float) lfos[(size_t) l].displayPhase(), std::memory_order_relaxed);
    }
    sources[(size_t) mod::envelope] = envelope.advance (envelopeCache, length);
    envelopeDisplayStage.store ((int) envelope.getStage(), std::memory_order_relaxed);
    for (int m = 0; m < mod::numMacros; ++m)
        sources[(size_t) (mod::macro1 + m)] = macros[(size_t) m] != nullptr ? macros[(size_t) m]->load() * 0.01f : 0.0f;

    if (block.buildEnd < 0.0) lastBuild = 0.0f;
    else {
        const double position = block.buildEnd - (double) (block.numSamples - (start + length));
        if (position >= 0.0) lastBuild = block.buildPeak > 0.5 ? (float) jlimit (0.0, 1.0, position / block.buildPeak) : 1.0f;
    }
    sources[(size_t) mod::build] = lastBuild;
    sources[(size_t) mod::none] = 0.0f;
    for (int s = 0; s < mod::numSources; ++s) sourceDisplay[(size_t) s].store (sources[(size_t) s], std::memory_order_relaxed);

    for (int k = 0; k < numRouted; ++k) targets[(size_t) routedTargets[(size_t) k]]->sum = 0.0f;
    for (int r = 0; r < mod::numRows; ++r) {
        const auto& row = rowCache[(size_t) r];
        const float out = mod::rowOutput (row, sources.data());
        rowDisplay[(size_t) r].store (out, std::memory_order_relaxed);
        if (row.source == mod::none || row.destination <= 0 || std::abs (row.amount) < 1.0e-6f) continue;
        targets[(size_t) row.destination]->sum += out;
    }
    for (int k = 0; k < numRouted; ++k) {
        auto& t = *targets[(size_t) routedTargets[(size_t) k]];
        if (t.base == nullptr) continue;
        const float base = t.base->load (std::memory_order_relaxed);
        if (t.parameter == nullptr) {
            t.effective.store (base, std::memory_order_relaxed);
            t.offset.store (0.0f, std::memory_order_relaxed);
            continue;
        }
        const float normalised = jlimit (0.0f, 1.0f, t.parameter->convertTo0to1 (base) + t.sum);
        t.effective.store (t.parameter->convertFrom0to1 (normalised), std::memory_order_relaxed);
        t.offset.store (t.sum, std::memory_order_relaxed);
    }
}

float ModMatrix::destinationOffset (int destination) const noexcept {
    return isPositiveAndBelow (destination, (int) targets.size()) ? targets[(size_t) destination]->offset.load (std::memory_order_relaxed) : 0.0f;
}

bool ModMatrix::isDestinationRouted (int destination) const noexcept {
    if (destination <= 0) return false;
    for (int r = 0; r < mod::numRows; ++r) {
        const auto row = readRow (r);
        if (row.destination == destination && row.source != mod::none && std::abs (row.amount) >= 1.0e-6f) return true;
    }
    return false;
}

RangedAudioParameter* ModMatrix::destinationParameter (int destination) const noexcept {
    return isPositiveAndBelow (destination, (int) targets.size()) ? targets[(size_t) destination]->parameter : nullptr;
}

float ModMatrix::baseNormalised (int destination) const noexcept {
    auto* p = destinationParameter (destination);
    return p != nullptr ? p->getValue() : 0.0f;
}

float ModMatrix::displayNormalised (int destination) const noexcept {
    return jlimit (0.0f, 1.0f, baseNormalised (destination) + (isDestinationRouted (destination) ? destinationOffset (destination) : 0.0f));
}

float ModMatrix::displayValue (const String& paramID, float base) const noexcept {
    const int destination = destinationIndex (paramID);
    auto* p = destinationParameter (destination);
    if (p == nullptr || ! isDestinationRouted (destination)) return base;
    return p->convertFrom0to1 (jlimit (0.0f, 1.0f, p->convertTo0to1 (base) + destinationOffset (destination)));
}

}
