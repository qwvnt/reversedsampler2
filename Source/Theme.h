#pragma once

#include <juce_graphics/juce_graphics.h>

// Colours shared by all parts of the editor.
namespace Theme
{
    inline const juce::Colour background { 0xff15171c };
    inline const juce::Colour panel      { 0xff1d2027 };
    inline const juce::Colour panelLight { 0xff262a33 };
    inline const juce::Colour border     { 0xff2f343e };
    inline const juce::Colour text       { 0xffe8eaed };
    inline const juce::Colour dimText    { 0xff8d939e };
    inline const juce::Colour layer1     { 0xff5fb3ff };   // blue
    inline const juce::Colour layer2     { 0xff4fd1b5 };   // teal
    inline const juce::Colour reversed   { 0xffffa94d };   // a reversed layer
    inline const juce::Colour disabled   { 0xff4a4f59 };   // a layer that is switched off
    inline const juce::Colour warning    { 0xffffd166 };
    inline const juce::Colour error      { 0xffff6b6b };
    inline const juce::Colour modulation { 0xffc792ea };

    inline juce::Colour layerColour (int layer)   { return layer == 0 ? layer1 : layer2; }
}
