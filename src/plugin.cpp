#include "bridge.h"
#include "controls.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/main/pluginfactory_constexpr.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace isolator {
using namespace Steinberg;
using namespace Steinberg::Vst;
#ifdef VOICE_ISOLATOR_RTX
static DECLARE_UID(ProcessorId, 0x9A744A42, 0x489A4A82, 0xB8966F36, 0x5BB2A651);
static DECLARE_UID(ControllerId, 0x43AEFAA2, 0x4A024733, 0xAE509402, 0xC94C669C);
#define ISOLATOR_NAME "Voice Isolator RTX"
#define ISOLATOR_CONTROLLER "Voice Isolator RTX Controller"
#define ISOLATOR_VERSION VOICE_ISOLATOR_VERSION_STRING

constexpr uint32 modelLookahead = 1568;     // Samples at the model's 48 kHz rate.
constexpr uint32 lowGpuBudget = 432;        // 9 ms at 48 kHz.
constexpr uint32 latencyNoticeSteps = 1u << 20; // Every step is exact even in float32.

// Fade the last audible mix to silence when its GPU result is missing.
// Keep each channel's own anchor, including on a second miss during recovery.
class WetGapFader {
public:
    void prepare(double rate) {
        length = std::max(1, static_cast<int>(std::lround(rate * .005)));
        reset(0);
    }
    void reset(int64_t firstSample) {
        origin = firstSample;
        started = false; available = false; position = length;
        anchor = last = 0.f;
    }
    double process(int64_t sample, bool ready, double current) {
        if (sample < origin) return 0.f;
        if (!started) {
            started = true;
            available = ready;
        } else if (ready != available) {
            available = ready;
            anchor = last;
            position = 0;
        }
        if (position < length - 1) {
            ++position;
            const float t = static_cast<float>(position) / length;
            const float gain = t * t * (3.f - 2.f * t);
            last = anchor + (current - anchor) * gain;
        } else {
            position = length;
            last = current;
        }
        return last;
    }
private:
    int64_t origin = 0;
    int length = 1, position = 1;
    bool started = false, available = false;
    double anchor = 0., last = 0.;
};

class StrengthRamp {
public:
    void prepare(double rate) { length = std::max(1, static_cast<int>(std::lround(rate * .005))); }
    void reset(int index) { target = current = wetFractions[index]; remaining = 0; }
    double next(int index) {
        const double requested = wetFractions[index];
        if (requested != target) {
            target = requested;
            remaining = length;
            step = (target - current) / length;
        }
        if (remaining > 0)
            current = --remaining == 0 ? target : current + step;
        return current;
    }
private:
    int length = 1, remaining = 0;
    double target = 1., current = 1., step = 0.;
};
#else
static DECLARE_UID(ProcessorId, 0xCD5C46B4, 0x61E44654, 0xA21B7EB6, 0xDB610438);
static DECLARE_UID(ControllerId, 0x394BE9BB, 0xFA49459E, 0x8E7C32C8, 0x121D1310);
#define ISOLATOR_NAME "Inworld Isolator 38"
#define ISOLATOR_CONTROLLER "Inworld Isolator Controller"
#define ISOLATOR_VERSION "0.1.0"
#endif
struct Settings {
    bool bypass = false;
    uint8 strength = defaultStrength;
    uint8 latency = 0;
};
static bool loadSettings(IBStream* file, Settings& settings) {
    uint8 data[2]{}; int32 n = 0;
    if (!file || file->read(data, 2, &n) != kResultOk || n != 2) return false;
    if (data[0] != 1) {
#ifdef VOICE_ISOLATOR_RTX
        if ((data[0] != 2 && data[0] != 3) || file->read(&settings.strength, 1, &n) != kResultOk ||
            n != 1 || settings.strength >= wetFractions.size()) return false;
        if (data[0] == 3 && (file->read(&settings.latency, 1, &n) != kResultOk ||
                             n != 1 || settings.latency > 1)) return false;
#else
        return false;
#endif
    }
    settings.bypass = data[1] != 0;
    return true;
}

// Walk a host's sorted queue without allocating in process(). Multiple changes
// at the same sample resolve to the last value, including parameter-only flushes.
class Points {
public:
    void attach(IParamValueQueue* q) { queue = q; advance(); }
    template<class Setter> void applyThrough(int32 sample, Setter set) {
        while (available && offset <= sample) { set(value); advance(); }
    }
private:
    void advance() {
        available = queue && point < queue->getPointCount() &&
                    queue->getPoint(point++, offset, value) == kResultOk;
    }
    IParamValueQueue* queue = nullptr;
    int32 point = 0, offset = 0;
    ParamValue value = 0.;
    bool available = false;
};

class Controller final : public EditController {
public:
    static FUnknown* make(void*) { return static_cast<IEditController*>(new Controller); }
    tresult PLUGIN_API initialize(FUnknown* context) override {
        auto r = EditController::initialize(context);
        if (r != kResultOk) return r;
        parameters.addParameter(STR16("Bypass"), nullptr, 1, 0,
                                ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, Bypass);
        auto state = new StringListParameter(STR16("GPU engine"), Engine, nullptr,
                                            ParameterInfo::kIsReadOnly | ParameterInfo::kIsList);
        state->appendString(STR16("Loading"));
        state->appendString(STR16("Ready"));
        state->appendString(STR16("Catching up"));
        state->appendString(STR16("Unavailable"));
        parameters.addParameter(state);
        parameters.addParameter(new RangeParameter(STR16("Processed audio"), Coverage, STR16("%"),
                                                    0, 100, 0, 0, ParameterInfo::kIsReadOnly));
#ifdef VOICE_ISOLATOR_RTX
        auto strength = new StringListParameter(STR16("Noise reduction"), Strength, nullptr,
                                                ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
        strength->appendString(STR16("50%"));
        strength->appendString(STR16("80%"));
        strength->appendString(STR16("100%"));
        strength->getInfo().defaultNormalizedValue = normalizedStrength(defaultStrength);
        strength->setNormalized(strength->getInfo().defaultNormalizedValue);
        parameters.addParameter(strength);
        auto latency = new StringListParameter(STR16("Latency"), Latency, nullptr, ParameterInfo::kIsList);
        latency->appendString(STR16("Standard"));
        latency->appendString(STR16("Low"));
        parameters.addParameter(latency);
        parameters.addParameter(STR16("Latency changed"), nullptr, 0, 0,
                                ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden, LatencyNotice);
#endif
        return r;
    }
    tresult PLUGIN_API setComponentState(IBStream* file) override {
        Settings settings;
        if (!loadSettings(file, settings)) return kResultFalse;
#ifdef VOICE_ISOLATOR_RTX
        setParamNormalized(Strength, normalizedStrength(settings.strength));
        setParamNormalized(Latency, settings.latency ? 1. : 0.);
#endif
        return setParamNormalized(Bypass, settings.bypass ? 1. : 0.);
    }
#ifdef VOICE_ISOLATOR_RTX
    // Hosts may forward only the latest output parameter. Each applied change has
    // a distinct notice, including a return to Standard before the next UI update.
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        const bool pending = id == LatencyNotice && value > 0.;
        if (pending && !componentHandler) return kResultOk; // Retry when a handler is connected.
        const bool changed = pending && value != getParamNormalized(LatencyNotice);
        const auto result = EditController::setParamNormalized(id, value);
        if (changed && result == kResultOk) componentHandler->restartComponent(kLatencyChanged);
        return result;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override {
        return name && std::strcmp(name, ViewType::kEditor) == 0 ? makeEditor(this) : nullptr;
    }
#endif
};

class Processor final : public AudioEffect {
public:
    Processor() { setControllerClass(FUID(ControllerId)); }
    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Processor); }
    tresult PLUGIN_API initialize(FUnknown* context) override {
        auto r = AudioEffect::initialize(context);
        if (r == kResultOk) {
            addAudioInput(STR16("Mic"), SpeakerArr::kStereo);
            addAudioOutput(STR16("Isolated voice"), SpeakerArr::kStereo);
        }
        return r;
    }
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* in, int32 ni, SpeakerArrangement* out, int32 no) override {
        if (!in || !out || ni != 1 || no != 1 || in[0] != out[0] ||
            (in[0] != SpeakerArr::kMono && in[0] != SpeakerArr::kStereo)) return kResultFalse;
        return AudioEffect::setBusArrangements(in, ni, out, no);
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        return size == kSample32 || size == kSample64 ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup& s) override {
        if (!std::isfinite(s.sampleRate) || s.sampleRate < 8000 || s.sampleRate > 384000 ||
            s.sampleRate != std::floor(s.sampleRate) || s.maxSamplesPerBlock < 1 || s.maxSamplesPerBlock > 65536)
            return kInvalidArgument;
        const double ratio = s.sampleRate / 48000.;
        const auto block = static_cast<uint32>(s.maxSamplesPerBlock);
        const uint32 resampling = s.sampleRate != 48000. ? 32 + static_cast<uint32>(std::ceil(32 * ratio)) : 0;
        const uint32 standard = static_cast<uint32>(std::ceil(2048 * ratio)) + block + resampling;
#ifdef VOICE_ISOLATOR_RTX
        // The callback consumes its whole output at entry. A long callback can need
        // the result up to one maximum block early, even after a very short input
        // callback. Reserve that block separately from inference time in both modes.
        const uint32 low = static_cast<uint32>(std::ceil((modelLookahead + lowGpuBudget) * ratio)) + block + resampling;
        delays = {standard, low};
        delay = delays[latencyMode()];
#else
        delay = standard;
#endif
        for (auto& channel : dry) channel.assign(standard, 0.);
        mono.resize(s.maxSamplesPerBlock);
#ifdef VOICE_ISOLATOR_RTX
        for (auto& fader : gapFaders) fader.prepare(s.sampleRate);
        strengthRamp.prepare(s.sampleRate);
#endif
        return AudioEffect::setupProcessing(s);
    }
#ifdef VOICE_ISOLATOR_RTX
    uint32 PLUGIN_API getLatencySamples() override {
        const auto samples = delays[latencyMode()];
        reported = samples;
        return samples;
    }
    uint32 PLUGIN_API getTailSamples() override { return delays[latencyMode()]; }
#else
    uint32 PLUGIN_API getLatencySamples() override { return delay; }
    uint32 PLUGIN_API getTailSamples() override { return delay; }
#endif
    tresult PLUGIN_API setActive(TBool on) override {
        if (on && !active) {
            if (mono.empty()) return kResultFalse;
            cursor = 0;
            bridge.start(processSetup.sampleRate);
            reset();
        } else if (!on && active) {
            bridge.close();
        }
        active = on != 0;
        return AudioEffect::setActive(on);
    }
    tresult PLUGIN_API setProcessing(TBool on) override {
        if (on) reset();
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override {
        bridge.close();
        return AudioEffect::terminate();
    }
    tresult PLUGIN_API getState(IBStream* file) override {
#ifdef VOICE_ISOLATOR_RTX
        uint8 data[] = {3, static_cast<uint8>(bypassed.load()), strength.load(), latencyMode()};
#else
        uint8 data[] = {1, static_cast<uint8>(bypassed.load())};
#endif
        int32 n = 0;
        return file && file->write(data, sizeof(data), &n) == kResultOk && n == sizeof(data) ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API setState(IBStream* file) override {
        Settings settings;
        if (!loadSettings(file, settings)) return kResultFalse;
        bypassed = settings.bypass;
#ifdef VOICE_ISOLATOR_RTX
        strength = settings.strength;
        setLatencyMode(settings.latency);
#endif
        return kResultOk;
    }
    tresult PLUGIN_API process(ProcessData& d) override {
        Points bypassPoints, strengthPoints;
        if (d.inputParameterChanges) {
            for (int32 i = 0; i < d.inputParameterChanges->getParameterCount(); ++i) {
                auto q = d.inputParameterChanges->getParameterData(i);
                if (!q) continue;
                if (q->getParameterId() == Bypass) bypassPoints.attach(q);
#ifdef VOICE_ISOLATOR_RTX
                if (q->getParameterId() == Strength) strengthPoints.attach(q);
                if (q->getParameterId() == Latency) {
                    // The delay changes between blocks, so only the block's last value counts.
                    int32 offset = 0; ParamValue value = 0.;
                    if (q->getPointCount() > 0 && q->getPoint(q->getPointCount() - 1, offset, value) == kResultOk)
                        setLatencyMode(value >= .5 ? 1 : 0);
                }
#endif
            }
        }
        if (!d.numSamples || !d.numInputs || !d.numOutputs) {
            applyChanges(bypassPoints, strengthPoints, INT32_MAX);
            const int s = bridge.state();
            report(d.outputParameterChanges, Engine, s < 0 ? 1. : s == 1 ? 1./3. : 0.);
            reportLatency(d.outputParameterChanges);
            return kResultOk;
        }
        if (!active || d.numSamples < 0 || d.numSamples > static_cast<int32>(mono.size()) ||
            d.inputs[0].numChannels != d.outputs[0].numChannels ||
            d.inputs[0].numChannels < 1 || d.inputs[0].numChannels > 2) return kInvalidArgument;
        if (d.processContext && (d.processContext->state & ProcessContext::kPlaying)) {
            auto position = d.processContext->projectTimeSamples;
            if (tracking && position != nextProject) reset();
            tracking = true;
            nextProject = position + d.numSamples;
        } else tracking = false;
        return d.symbolicSampleSize == kSample64
            ? audio(d, d.inputs[0].channelBuffers64, d.outputs[0].channelBuffers64, bypassPoints, strengthPoints)
            : audio(d, d.inputs[0].channelBuffers32, d.outputs[0].channelBuffers32, bypassPoints, strengthPoints);
    }
private:
    void applyChanges(Points& bypassPoints, Points& strengthPoints, int32 sample) {
        bypassPoints.applyThrough(sample, [this](double v) { bypassed = v >= .5; });
#ifdef VOICE_ISOLATOR_RTX
        strengthPoints.applyThrough(sample, [this](double v) { strength = static_cast<uint8>(strengthIndex(v)); });
#endif
    }
    void reset() {
        for (auto& lane : dry) std::fill(lane.begin(), lane.end(), 0.);
#ifdef VOICE_ISOLATOR_RTX
        for (auto& fader : gapFaders) fader.reset(cursor);
        strengthRamp.reset(strength.load());
#endif
        readySeen = false; tracking = false;
    }
    static void report(IParameterChanges* changes, ParamID id, double val) {
        if (!changes) return;
        int32 index = 0;
        if (auto q = changes->addParameterData(id, index)) q->addPoint(0, val, index);
    }
#ifdef VOICE_ISOLATOR_RTX
    uint8 latencyMode() const { return static_cast<uint8>(latencyRevision.load() & 1u); }
    void setLatencyMode(uint8 mode) {
        // Each toggle flips the low bit. Preset recalls and audio-thread edits
        // publish the mode and its notice number in one atomic update.
        auto revision = latencyRevision.load();
        while ((revision & 1u) != mode) {
            if (latencyRevision.compare_exchange_weak(revision, revision + 1u)) return;
        }
    }
#endif
    void reportLatency(IParameterChanges* changes) {
#ifdef VOICE_ISOLATOR_RTX
        const auto revision = latencyRevision.load();
        ParamValue notice = 0.; // Supersede any queued request if the host already has this delay.
        if (delays[revision & 1u] != reported.load()) {
            // An odd stride visits every step, while placing adjacent mode
            // changes half a scale apart for hosts that round parameter values.
            const auto stride = latencyNoticeSteps / 2u + 1u;
            const auto step = (revision * stride) % latencyNoticeSteps + 1u;
            notice = static_cast<ParamValue>(step) / latencyNoticeSteps;
        }
        report(changes, LatencyNotice, notice);
#endif
    }
    template<class Sample>
    tresult audio(ProcessData& d, Sample** in, Sample** out, Points& bypassPoints, Points& strengthPoints) {
        const int channels = d.inputs[0].numChannels, count = d.numSamples;
#ifdef VOICE_ISOLATOR_RTX
        if (const uint32 selected = delays[latencyMode()]; selected != delay) {
            delay = selected;
            reset();
        }
#endif
        const bool silent0 = (d.inputs[0].silenceFlags & 1) || !in || !in[0];
        const bool silent1 = channels < 2 || (d.inputs[0].silenceFlags & 2) || !in || !in[1];
        for (int i=0; i<count; ++i) {
            const float a = silent0 ? 0.f : static_cast<float>(in[0][i]);
            mono[i] = channels == 1 ? a : (a + (silent1 ? 0.f : static_cast<float>(in[1][i]))) * .5f;
        }
        int state = bridge.state();
        if (state == 0 && d.processMode == kOffline) state = bridge.waitReady() ? 1 : -1;
        if (state == 1) {
            if (!readySeen) { bridge.reset(cursor); readySeen = true; }
            bridge.publish(cursor, count, mono.data(), nullptr);
            if (d.processMode == kOffline) bridge.waitWet(cursor + count - delay);
        }
        const int64_t end = state == 1 ? bridge.wetEnd() : 0;
        int matched = 0, eligible = 0;
        uint64 silence = (1u << channels) - 1;
        for (int i=0; i<count; ++i) {
            applyChanges(bypassPoints, strengthPoints, i);
            const bool bypass = bypassed.load(std::memory_order_relaxed);
            const auto target = cursor + i - static_cast<int64_t>(delay);
            const bool expected = readySeen && target >= bridge.origin;
            const bool wet = expected && target < end && state == 1;
            eligible += expected; matched += wet;
            const float processed = wet ? bridge.wet(target) : 0.f;
#ifdef VOICE_ISOLATOR_RTX
            const double amount = strengthRamp.next(strength.load(std::memory_order_relaxed));
#endif
            const size_t pos = static_cast<size_t>((cursor+i) % delay);
            for (int c=0; c<channels; ++c) {
                const double sample = (c == 0 ? silent0 : silent1) ? 0. : in[c][i];
                const double original = dry[c][pos];
                dry[c][pos] = sample;
#ifdef VOICE_ISOLATOR_RTX
                const double mix = amount == 1. ? processed : amount * processed + (1. - amount) * original;
                const double isolated = gapFaders[c].process(target, wet, wet ? mix : 0.);
                const Sample y = static_cast<Sample>(bypass ? original : isolated);
#else
                const Sample y = static_cast<Sample>(wet && !bypass ? processed : original);
#endif
                if (out && out[c]) out[c][i] = y;
                if (y != 0) silence &= ~(uint64(1) << c);
            }
        }
        d.outputs[0].silenceFlags = silence;
        cursor += count;
        const int display = state < 0 ? 3 : state == 0 ? 0 : matched < eligible ? 2 : 1;
        report(d.outputParameterChanges, Engine, display / 3.);
        report(d.outputParameterChanges, Coverage, static_cast<double>(matched)/count);
        reportLatency(d.outputParameterChanges);
        return kResultOk;
    }
    Bridge bridge;
#ifdef VOICE_ISOLATOR_RTX
    std::array<WetGapFader, 2> gapFaders;
    StrengthRamp strengthRamp;
    std::atomic<uint8> strength{defaultStrength};
    std::atomic<uint32> latencyRevision{0};
    std::atomic<uint32> reported{0};
    std::array<uint32, 2> delays{2176, modelLookahead + lowGpuBudget + 128};
#endif
    std::array<std::vector<double>,2> dry;
    std::vector<float> mono;
    uint32 delay = 2176;
    int64_t cursor = 0, nextProject = 0;
    std::atomic<bool> bypassed{false};
    bool readySeen = false, tracking = false, active = false;
};
}
BEGIN_FACTORY_DEF("Local Audio Lab", "", "", 2)
DEF_CLASS(isolator::ProcessorId, Steinberg::PClassInfo::kManyInstances, kVstAudioEffectClass,
          ISOLATOR_NAME, 0, "Fx|Restoration", ISOLATOR_VERSION, kVstVersionString,
          isolator::Processor::make, nullptr)
DEF_CLASS(isolator::ControllerId, Steinberg::PClassInfo::kManyInstances, kVstComponentControllerClass,
          ISOLATOR_CONTROLLER, 0, "", ISOLATOR_VERSION, kVstVersionString,
          isolator::Controller::make, nullptr)
END_FACTORY
