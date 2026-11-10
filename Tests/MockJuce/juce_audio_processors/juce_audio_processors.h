#pragma once
// Stand-ins for the few JUCE classes that Source/FxRack.* uses, so the FX rack can be built and
// tested with any C++17 compiler, without JUCE or CMake. Only used by Tests/FxRackTests.cpp:
//   g++ -std=c++17 -O2 -ISource -ITests/MockJuce Tests/FxRackTests.cpp Source/FxRack.cpp -o FxRackTests
// Never add this folder to the plugin's include path.
// New buffers are filled with NaN (JUCE leaves them uninitialised), so a read of memory that was
// never written shows up as a non-finite output instead of passing by luck.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace juce
{
template <typename T> constexpr T jmin (T a, T b) noexcept { return b < a ? b : a; }
template <typename T> constexpr T jmax (T a, T b) noexcept { return a < b ? b : a; }
template <typename T> constexpr T jlimit (T lowest, T highest, T v) noexcept { return v < lowest ? lowest : (highest < v ? highest : v); }

template <typename T>
struct MathConstants
{
    static constexpr T pi = static_cast<T> (3.141592653589793238L);
    static constexpr T twoPi = static_cast<T> (2.0L * 3.141592653589793238L);
};

struct ScopedNoDenormals { ScopedNoDenormals() noexcept {} };

class String
{
public:
    String() = default;
    String (const char* s) : text (s != nullptr ? s : "") {}
    String (std::string s) : text (std::move (s)) {}
    explicit String (int value) : text (std::to_string (value)) {}
    const std::string& toStdString() const noexcept { return text; }
    friend String operator+ (const String& a, const String& b) { return String (a.text + b.text); }
    friend String operator+ (const char* a, const String& b) { return String (std::string (a) + b.text); }
    friend String operator+ (const String& a, const char* b) { return String (a.text + b); }
private:
    std::string text;
};

class StringArray
{
public:
    StringArray() = default;
    StringArray (std::initializer_list<const char*> items) { for (auto* s : items) strings.emplace_back (s); }
    int size() const noexcept { return (int) strings.size(); }
private:
    std::vector<String> strings;
};

template <typename T>
class AudioBuffer
{
public:
    AudioBuffer() = default;
    AudioBuffer (int channels, int samples) { setSize (channels, samples); }
    AudioBuffer (T* const* data, int channels, int samples) : AudioBuffer (data, channels, 0, samples) {}
    AudioBuffer (T* const* data, int channels, int start, int samples) : numChannels (channels), numSamples (samples)
    {
        for (int c = 0; c < channels; ++c) pointers.push_back (data[c] + start);
    }
    AudioBuffer (const AudioBuffer&) = delete;
    AudioBuffer& operator= (const AudioBuffer&) = delete;

    void setSize (int channels, int samples, bool = false, bool = false, bool = false)
    {
        numChannels = channels; numSamples = samples;
        storage.assign ((size_t) channels * (size_t) samples, std::numeric_limits<T>::quiet_NaN());
        pointers.clear();
        for (int c = 0; c < channels; ++c) pointers.push_back (storage.data() + (size_t) c * (size_t) samples);
    }
    int getNumChannels() const noexcept { return numChannels; }
    int getNumSamples() const noexcept { return numSamples; }
    T* getWritePointer (int c) noexcept { assert (c >= 0 && c < numChannels); return pointers[(size_t) c]; }
    T* getWritePointer (int c, int start) noexcept { return getWritePointer (c) + start; }
    const T* getReadPointer (int c) const noexcept { assert (c >= 0 && c < numChannels); return pointers[(size_t) c]; }
    const T* getReadPointer (int c, int start) const noexcept { return getReadPointer (c) + start; }
    T* const* getArrayOfWritePointers() noexcept { return pointers.data(); }
    void clear() noexcept { for (auto* p : pointers) std::fill (p, p + numSamples, T (0)); }
    T getSample (int c, int i) const noexcept { assert (i >= 0 && i < numSamples); return getReadPointer (c)[i]; }
    void setSample (int c, int i, T v) noexcept { assert (i >= 0 && i < numSamples); getWritePointer (c)[i] = v; }
private:
    int numChannels = 0, numSamples = 0;
    std::vector<T> storage;
    std::vector<T*> pointers;
};

namespace ValueSmoothingTypes { struct Linear {}; struct Multiplicative {}; }

// Linear smoothing with JUCE's semantics (reset = steps, setTargetValue restarts the countdown).
template <typename T, typename SmoothingType = ValueSmoothingTypes::Linear>
class SmoothedValue
{
public:
    SmoothedValue() = default;
    explicit SmoothedValue (T initial) : current (initial), target (initial) {}
    void reset (double sampleRate, double rampSeconds) noexcept { reset ((int) std::floor (rampSeconds * sampleRate)); }
    void reset (int steps) noexcept { stepsToTarget = steps; setCurrentAndTargetValue (target); }
    void setCurrentAndTargetValue (T v) noexcept { target = current = v; countdown = 0; }
    void setTargetValue (T v) noexcept
    {
        if (v == target) return;
        if (stepsToTarget <= 0) { setCurrentAndTargetValue (v); return; }
        target = v; countdown = stepsToTarget; step = (target - current) / (T) countdown;
    }
    T getNextValue() noexcept
    {
        if (! isSmoothing()) return target;
        --countdown;
        if (isSmoothing()) current += step; else current = target;
        return current;
    }
    bool isSmoothing() const noexcept { return countdown > 0; }
    T getCurrentValue() const noexcept { return current; }
    T getTargetValue() const noexcept { return target; }
private:
    T current {}, target {}, step {};
    int countdown = 0, stepsToTarget = 0;
};

// Raw parameter values only; unknown IDs are created at 0 (the tests set every default).
class AudioProcessorValueTreeState
{
public:
    std::atomic<float>* getRawParameterValue (const String& id)
    {
        auto& value = values[id.toStdString()];
        if (value == nullptr) value = std::make_unique<std::atomic<float>> (0.0f);
        return value.get();
    }
private:
    std::map<std::string, std::unique_ptr<std::atomic<float>>> values;
};
} // namespace juce
