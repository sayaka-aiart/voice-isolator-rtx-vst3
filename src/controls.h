#pragma once
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/base/futils.h"
#include <array>

namespace isolator {
enum : Steinberg::Vst::ParamID {
    Bypass = 100, Engine = 101, Coverage = 102, Strength = 103, Latency = 104, LatencyNotice = 105
};
inline constexpr std::array<double, 3> wetFractions{.5, .8, 1.};
inline constexpr int defaultStrength = 2;
inline int strengthIndex(double normalized) {
    return normalized > 0. ? Steinberg::FromNormalized(normalized < 1. ? normalized : 1., 2) : 0;
}
inline double normalizedStrength(int index) { return index / 2.; }
Steinberg::IPlugView* makeEditor(Steinberg::Vst::EditController* controller);
}
