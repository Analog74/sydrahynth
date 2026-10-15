// Headless test of the built plugin, hosted the way a DAW would: loads the
// VST3, runs it at 48 kHz in real time, uploads the first patch of a bank to
// A001 over SysEx, plays middle C, then saves the state, loads it into a fresh
// instance and plays again. Writes both takes as WAV files and prints their
// level and pitch. The firmware comes from HYDRA_FIRMWARE.
//
//   plugintest <plugin.vst3> <bank.hydra> <out-prefix>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "patch_bank.h"
#include "sysex.h"

namespace {
constexpr double kRate = 48000.0;
// Host block size: HYDRA_HOST_BLOCK, default 512.
const int kBlock = std::getenv("HYDRA_HOST_BLOCK") ? std::atoi(std::getenv("HYDRA_HOST_BLOCK")) : 512;
using Clock = std::chrono::steady_clock;

struct Take {
    juce::AudioBuffer<float> audio{2, 0};
};

class Host {
public:
    explicit Host(juce::AudioPluginInstance& plugin) : plugin_(plugin) {
        plugin_.setPlayConfigDetails(0, 2, kRate, kBlock);
        plugin_.prepareToPlay(kRate, kBlock);
        next_ = Clock::now();
    }
    ~Host() { plugin_.releaseResources(); }

    // Runs `seconds` of real time; queued MIDI goes into the first block.
    void run(double seconds, juce::AudioBuffer<float>* record = nullptr) {
        const int blocks = static_cast<int>(seconds * kRate / kBlock);
        juce::AudioBuffer<float> buffer(2, kBlock);
        for (int b = 0; b < blocks; ++b) {
            next_ += std::chrono::microseconds(static_cast<long>(1e6 * kBlock / kRate));
            std::this_thread::sleep_until(next_);
            buffer.clear();
            plugin_.processBlock(buffer, midi_);
            midi_.clear();
            if (record) {
                const int at = record->getNumSamples();
                record->setSize(2, at + kBlock, true);
                for (int ch = 0; ch < 2; ++ch)
                    record->copyFrom(ch, at, buffer, ch, 0, kBlock);
            }
        }
    }
    void send(const juce::MidiMessage& message) { midi_.addEvent(message, 0); }

private:
    juce::AudioPluginInstance& plugin_;
    juce::MidiBuffer midi_;
    Clock::time_point next_;
};

void report(const char* label, const juce::AudioBuffer<float>& take, const juce::File& file) {
    const float* left = take.getReadPointer(0);
    const int n = take.getNumSamples();
    float peak = 0;
    for (int i = 0; i < n; ++i)
        peak = std::max(peak, std::abs(left[i]));
    // Fundamental from the autocorrelation over 0.2 s in the middle of the note:
    // the shortest lag that correlates nearly as well as the best one (longer
    // lags at multiples of the period correlate as well, or better).
    const int start = static_cast<int>(0.3 * kRate), length = static_cast<int>(0.2 * kRate);
    const int minLag = static_cast<int>(kRate / 1000), maxLag = static_cast<int>(kRate / 60);
    std::vector<double> correlation(static_cast<std::size_t>(maxLag), 0.0);
    double best = 0;
    for (int lag = minLag; lag < maxLag && start + length + lag < n; ++lag) {
        double sum = 0;
        for (int i = 0; i < length; ++i)
            sum += left[start + i] * left[start + i + lag];
        correlation[static_cast<std::size_t>(lag)] = sum;
        best = std::max(best, sum);
    }
    int bestLag = 0;
    for (int lag = minLag + 1; lag + 1 < maxLag && !bestLag; ++lag) {
        const double c = correlation[static_cast<std::size_t>(lag)];
        if (c >= 0.9 * best && c >= correlation[static_cast<std::size_t>(lag - 1)] && c >= correlation[static_cast<std::size_t>(lag + 1)])
            bestLag = lag;
    }
    std::printf("%s: peak %.3f, fundamental %.1f Hz\n", label, peak, bestLag ? kRate / bestLag : 0.0);
    juce::WavAudioFormat wav;
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(2).withBitsPerSample(24);
    if (stream)
        if (auto writer = wav.createWriterFor(stream, options))
            writer->writeFromAudioSampleBuffer(take, 0, n);
}

std::unique_ptr<juce::AudioPluginInstance> load(juce::AudioPluginFormatManager& formats, const juce::String& path) {
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* format : formats.getFormats())
        format->findAllTypesForFile(found, path);
    if (found.isEmpty())
        return nullptr;
    juce::String error;
    auto instance = formats.createPluginInstance(*found[0], kRate, kBlock, error);
    if (!instance)
        std::fprintf(stderr, "cannot load: %s\n", error.toRawUTF8());
    return instance;
}

void playNote(Host& host, juce::AudioBuffer<float>& take) {
    host.send(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)));
    host.run(1.0, &take);
    host.send(juce::MidiMessage::noteOff(1, 60));
    host.run(1.0, &take);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <plugin.vst3> <bank.hydra> <out-prefix>\n", argv[0]);
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI juce;
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);
    const juce::String prefix(argv[3]);

    juce::MemoryBlock state;
    {
        auto plugin = load(formats, argv[1]);
        if (!plugin)
            return 1;
        Host host(*plugin);
        std::printf("loaded %s with %d parameters, latency %d samples\n", plugin->getName().toRawUTF8(),
                    plugin->getParameters().size(), plugin->getLatencySamples());
        host.run(10.0);  // boot (with its warm-up), and let the firmware settle
        const std::vector<hydra::Patch> patches = hydra::loadPatches(argv[2]);
        for (const auto& message : hydra::sysex::patchUpload(patches.at(0).body, 0, 0, true)) {
            host.send(juce::MidiMessage(message.data(), static_cast<int>(message.size())));
            host.run(0.1);
        }
        host.run(0.5);
        juce::AudioBuffer<float> take(2, 0);
        playNote(host, take);
        report("first instance", take, juce::File(prefix + "-first.wav"));
        plugin->getStateInformation(state);
        std::printf("state: %zu bytes\n", state.getSize());
    }
    {
        auto plugin = load(formats, argv[1]);
        if (!plugin)
            return 1;
        plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        Host host(*plugin);
        host.run(10.0);
        juce::AudioBuffer<float> take(2, 0);
        playNote(host, take);
        report("restored instance", take, juce::File(prefix + "-restored.wav"));
    }
    return 0;
}
