#pragma once

#include <array>
#include <cstdint>

// The Hydrasynth's MIDI CC assignments (its MIDI implementation chart), exposed
// to the host as automatable parameters.
namespace ccmap {

struct Entry {
    std::uint8_t cc;
    const char* name;
};

inline constexpr std::array<Entry, 111> kEntries{{
    {3, "Noise Vol"}, {5, "Glide Time"}, {7, "Master Volume"}, {8, "Noise Pan"}, {9, "Ring Mod Vol"},
    {10, "Ring Mod Pan"}, {12, "Pre-FX Param 1"}, {13, "Pre-FX Param 2"}, {14, "Delay Feedback"}, {15, "Delay Time"},
    {16, "Macro 1"}, {17, "Macro 2"}, {18, "Macro 3"}, {19, "Macro 4"}, {20, "Macro 5"},
    {21, "Macro 6"}, {22, "Macro 7"}, {23, "Macro 8"}, {24, "Osc 1 WaveScan"}, {25, "Env 4 Attack"},
    {26, "Osc 2 WaveScan"}, {27, "Env 4 Decay"}, {28, "LFO 2 Gain"}, {29, "Mutator 1 Ratio"}, {30, "Mutator 1 Depth"},
    {31, "Mutator 1 Dry/Wet"}, {33, "Mutator 2 Ratio"}, {34, "Mutator 2 Depth"}, {35, "Mutator 2 Dry/Wet"},
    {36, "Mutator 3 Ratio"}, {37, "Mutator 3 Depth"}, {39, "Mutator 3 Dry/Wet"}, {40, "Mutator 4 Ratio"},
    {41, "Mutator 4 Depth"}, {42, "Mutator 4 Dry/Wet"}, {43, "RM12 Depth"}, {44, "Osc 1 Vol"}, {45, "Osc 1 Pan"},
    {46, "Osc 2 Vol"}, {47, "Osc 2 Pan"}, {48, "Osc 3 Vol"}, {49, "Osc 3 Pan"}, {50, "Filter 1 Drive"},
    {51, "Filter 1 Keytrack"}, {52, "Filter 1 LFO1 Amt"}, {53, "Filter 1 Vel Env"}, {54, "Filter 1 Env1 Amt"},
    {55, "Filter 2 Cutoff"}, {56, "Filter 2 Res"}, {57, "Filter 2 Type"}, {58, "Filter 2 Keytrack"},
    {59, "Filter 2 LFO1 Amt"}, {60, "Filter 2 Vel Env"}, {61, "Filter 2 Env1 Amt"}, {62, "Amp LFO2 Amt"},
    {63, "Delay Wet Tone"}, {65, "Reverb Time"}, {66, "Glide"}, {67, "Reverb Tone"}, {68, "Post-FX Param 1"},
    {69, "Post-FX Param 2"}, {70, "LFO 1 Gain"}, {71, "Filter 1 Res"}, {72, "LFO 1 Rate"}, {73, "LFO 2 Rate"},
    {74, "Filter 1 Cutoff"}, {75, "LFO 3 Gain"}, {76, "LFO 3 Rate"}, {77, "LFO 4 Gain"}, {78, "LFO 4 Rate"},
    {79, "LFO 5 Gain"}, {80, "LFO 5 Rate"}, {81, "Env 1 Attack"}, {82, "Env 1 Decay"}, {83, "Env 1 Sustain"},
    {84, "Env 1 Release"}, {85, "Env 2 Attack"}, {86, "Env 2 Decay"}, {87, "Env 2 Sustain"}, {88, "Env 2 Release"},
    {89, "Env 3 Attack"}, {90, "Env 3 Decay"}, {91, "Reverb Dry/Wet"}, {92, "Delay Dry/Wet"}, {93, "Pre-FX Mix"},
    {94, "Post-FX Mix"}, {95, "Detune"}, {96, "Env 3 Sustain"}, {97, "Env 3 Release"}, {102, "Env 5 Attack"},
    {103, "Env 5 Decay"}, {104, "Env 5 Sustain"}, {105, "Env 5 Release"}, {106, "Arp Division"}, {107, "Arp Gate"},
    {108, "Arp Mode"}, {109, "Arp Ratchet"}, {110, "Arp Chance"}, {111, "Osc 1 Cent"}, {112, "Osc 2 Cent"},
    {113, "Osc 3 Cent"}, {114, "Osc 3 FRate"}, {115, "Noise FRate"}, {116, "RM12 FRate"}, {117, "Stereo Width"},
    {118, "Osc 1 FRate"}, {119, "Osc 2 FRate"}, {120, "Arp Octave"}, {122, "Arp Length"}, {124, "Env 4 Release"},
    {125, "Env 4 Sustain"},
}};

}  // namespace ccmap
