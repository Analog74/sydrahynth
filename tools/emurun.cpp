// Drives hydra::Emulator the way a plugin host would: boots the firmware,
// measures its unpaced throughput, optionally uploads the first patch of a bank
// to A001 (after `seconds`, default 0.5), then pulls audio in 512-frame blocks at
// real-time pace while playing middle C, reports underruns, restarts with the
// captured flash and plays again (state restore), and writes the first take as
// a 32-bit float WAV. HYDRA_DSP_THREADS sets the DSP threads (default 4), HYDRA_TARGET_FILL
// the frames the emulator keeps buffered, HYDRA_BLOCK the pull size (512);
// HYDRA_SOAK runs a long panel soak test instead (see below).
//
//   emurun <firmware.dat> <out.wav> [bank.hydra [seconds]]

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <format>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

#include "emulator.h"
#include "panel_mcu.h"
#include "patch_bank.h"
#include "sysex.h"

using namespace hydra;
using Clock = std::chrono::steady_clock;

namespace {
bool writeFloatWav(const char* path, const std::vector<float>& interleaved) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f)
        return false;
    const auto u32 = [f](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    const auto u16 = [f](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const auto bytes = static_cast<std::uint32_t>(interleaved.size() * 4);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(3);  // IEEE float
    u16(2);
    u32(44100);
    u32(44100 * 8);
    u16(8);
    u16(32);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    std::fwrite(interleaved.data(), 4, interleaved.size(), f);
    return std::fclose(f) == 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <firmware.dat> <out.wav> [bank.hydra]\n", argv[0]);
        return 2;
    }
    Emulator emulator;
    Emulator::Config config;
    config.firmwarePath = argv[1];
    config.profile = true;
    config.systemSettings = {{{5, 7}, 1}};  // PgmChgRX on, as the plugin does
    if (const char* t = std::getenv("HYDRA_TARGET_FILL"))
        config.targetFill = static_cast<std::size_t>(std::atoi(t));
    const std::size_t kBlock = std::getenv("HYDRA_BLOCK") ? static_cast<std::size_t>(std::atoi(std::getenv("HYDRA_BLOCK"))) : 512;
    const auto start = Clock::now();
    emulator.start(config);
    while (emulator.state() == Emulator::State::Booting)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const double bootSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("state after %.2f s: %s\n", bootSeconds, emulator.status().c_str());
    if (emulator.state() != Emulator::State::Running)
        return 1;

    // HYDRA_BOOT_TEST=<seconds>: right after going live, pulls 128-frame blocks at
    // real-time pace for that long (as a host at 2.9 ms blocks would) and reports
    // underruns and the slowest run chunk: whatever the firmware still does for
    // the first time after the boot (e.g. the end of the panel's start screen)
    // happens live then.
    if (const char* bootTest = std::getenv("HYDRA_BOOT_TEST")) {
        emulator.resetChunkStats();
        const std::uint64_t before = emulator.underruns();
        std::vector<float> l(128), r(128);
        const auto t0 = Clock::now();
        auto next = t0;
        std::uint64_t lastUnderruns = before;
        const double seconds = std::atof(bootTest);
        while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
            next += std::chrono::nanoseconds(static_cast<long long>(128 / Emulator::kSampleRate * 1e9));
            std::this_thread::sleep_until(next);
            emulator.readAudio(l.data(), r.data(), 128);
            if (emulator.underruns() != lastUnderruns) {
                std::printf("  underrun at %.2f s after going live\n", std::chrono::duration<double>(Clock::now() - t0).count());
                lastUnderruns = emulator.underruns();
            }
        }
        std::printf("boot test: %llu underruns in %.0f s, slowest chunk %llu us, %llu chunks over 2 ms\n",
                    static_cast<unsigned long long>(emulator.underruns() - before), seconds,
                    static_cast<unsigned long long>(emulator.slowestChunkMicros()), static_cast<unsigned long long>(emulator.slowChunks()));
        return 0;
    }

    // HYDRA_ARP_TEST: opens the arp edit page (SHIFT + ON) and steps each arp
    // switch through its positions, saving the small displays per position to
    // /tmp/hydra-pb/arp_<switch>_<position>.pgm (4 displays, 128 x 256).
    if (std::getenv("HYDRA_ARP_TEST")) {
        using K = Emulator::PanelInput::Kind;
        std::vector<float> sink(8192);
        const auto idle = [&](int ms) {
            for (int i = 0; i < ms / 5; ++i) {
                emulator.readAudio(sink.data(), sink.data() + 4096, 4096);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        if (argc > 3) {
            std::vector<Patch> patches = loadPatches(argv[3]);
            patches.resize(1);
            patches[0].slot = 0;
            emulator.uploadPatches(std::move(patches), 0);
            while (emulator.uploading())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        idle(1000);
        emulator.panelInput({K::Button, 62, 1, 0, 0});
        emulator.panelInput({K::Button, 26, 1, 0, 20});
        idle(150);
        emulator.panelInput({K::Button, 26, 0, 0, 20});
        emulator.panelInput({K::Button, 62, 0, 0, 100});
        idle(800);
        FrontPanel::Lights lights;
        std::uint64_t generation = 0;
        for (int sw = 0; sw < FrontPanel::kArpSwitches; ++sw) {
            const int positions = FrontPanel::kArpPositions[static_cast<std::size_t>(sw)];
            for (int pos = 0; pos < positions; ++pos) {
                emulator.panelInput({K::ArpSwitch, sw, pos, 0});
                idle(150 * positions + 600);
                emulator.readPanel(lights, generation);
                if (std::FILE* f = std::fopen(std::format("/tmp/hydra-pb/arp_{}_{}.pgm", sw, pos).c_str(), "wb")) {
                    std::fprintf(f, "P5\n128 256\n255\n");
                    for (int d = 1; d < 5; ++d)
                        std::fwrite(lights.displays[static_cast<std::size_t>(d)].data(), 1, lights.displays[static_cast<std::size_t>(d)].size(), f);
                    std::fclose(f);
                }
            }
            emulator.panelInput({K::ArpSwitch, sw, 0, 0});
            idle(150 * positions + 300);
        }
        return 0;
    }

    // HYDRA_CHAIN_TEST: uploads the bank's first three patches to A001-A003,
    // selects each and prints the WS2812 chains' bytes (GRB per LED).
    if (std::getenv("HYDRA_CHAIN_TEST") && argc > 3) {
        std::vector<float> sink(8192);
        const auto idle = [&](int ms) {
            for (int i = 0; i < ms / 5; ++i) {
                emulator.readAudio(sink.data(), sink.data() + 4096, 4096);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        std::vector<Patch> patches = loadPatches(argv[3]);
        patches.resize(3);
        emulator.uploadPatches(patches, 0);
        while (emulator.uploading())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int k = 0; k < 3; ++k) {
            emulator.selectPatch(0, k);
            idle(1500);
            std::printf("A%03d %s:\n", k + 1, patches[static_cast<std::size_t>(k)].name.c_str());
            for (int chain = 0; chain < 4; ++chain) {
                const auto& bytes = emulator.machine()->frontPanel().ledChain(chain);
                std::printf("  chain %d (%zu LEDs):", chain, bytes.size() / 3);
                for (std::size_t i = 0; i + 2 < bytes.size(); i += 3)
                    std::printf(" %02x%02x%02x", bytes[i + 1], bytes[i], bytes[i + 2]);  // as RGB
                std::printf("\n");
            }
        }
        return 0;
    }

    // HYDRA_SHIFT_TEST: presses buttons alone and with SHIFT (the panel view's
    // timing: SHIFT, the button 20 ms later, SHIFT released 80 ms after it) and
    // reports whether the displays differ.
    if (std::getenv("HYDRA_SHIFT_TEST")) {
        std::vector<float> sink(8192);
        const auto idle = [&](int ms) {
            for (int i = 0; i < ms / 5; ++i) {
                emulator.readAudio(sink.data(), sink.data() + 4096, 4096);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        FrontPanel::Lights lights;
        std::uint64_t generation = 0;
        const auto screens = [&] {
            idle(600);
            emulator.readPanel(lights, generation);
            return lights.displays;
        };
        const auto click = [&](int id, bool shift) {
            using K = Emulator::PanelInput::Kind;
            if (shift)
                emulator.panelInput({K::Button, 62, 1, 0, 0});
            emulator.panelInput({K::Button, id, 1, 0, shift ? 20 : 0});
            idle(120);
            emulator.panelInput({K::Button, id, 0, 0, shift ? 20 : 0});
            if (shift)
                emulator.panelInput({K::Button, 62, 0, 0, 100});
        };
        if (argc > 3) {  // the bank's first patch to A001
            std::vector<Patch> patches = loadPatches(argv[3]);
            patches.resize(1);
            patches[0].slot = 0;
            emulator.uploadPatches(std::move(patches), 0);
            while (emulator.uploading())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        idle(1500);
        int index = 0;
        std::vector<int> ids = {11, 14, 24, 1, 2, 4, 5, 70, 66, 45, 27, 44, 59, 56, 57, 58, 26, 33};
        if (const char* list = std::getenv("HYDRA_SHIFT_IDS")) {
            ids.clear();
            for (const char* p = list; *p;) {
                char* end;
                ids.push_back(static_cast<int>(std::strtol(p, &end, 10)));
                p = *end ? end + 1 : end;
            }
        }
        const int maxSaved = std::getenv("HYDRA_SHIFT_SAVE_ALL") ? 1000 : 4;
        for (const int id : ids) {
            click(id, false);
            const auto plain = screens();
            click(7, false);
            screens();
            click(id, true);
            const auto shifted = screens();
            click(7, false);
            screens();
            const bool differs = plain != shifted;
            std::printf("button %2d: %s\n", id, differs ? "SHIFT changes the result" : "same");
            if ((differs || maxSaved > 4) && index < maxSaved)
                if (std::FILE* f = std::fopen(std::format("/tmp/hydra-pb/shift_{}.pgm", id).c_str(), "wb")) {
                    ++index;
                    std::fprintf(f, "P5\n128 640\n255\n");
                    for (const auto& set : {plain, shifted})
                        for (const auto& d : set)
                            std::fwrite(d.data(), 1, d.size(), f);
                    std::fclose(f);
                }
        }
        return 0;
    }

    // HYDRA_LED_TEST: presses ARP ON, LATCH and ARP ON again through the panel
    // input and prints the button lights the panel view gets (LEDs 30 and 31).
    if (std::getenv("HYDRA_LED_TEST")) {
        std::vector<float> sink(8192);
        const auto idle = [&](int ms) {
            for (int i = 0; i < ms / 5; ++i) {
                emulator.readAudio(sink.data(), sink.data() + 4096, 4096);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        FrontPanel::Lights lights;
        std::uint64_t generation = 0;
        const auto show = [&](const char* what) {
            emulator.readPanel(lights, generation);
            std::printf("%-16s ARP ON %.2f  LATCH %.2f  TRIPLET %.2f\n", what, lights.buttons[30], lights.buttons[31], lights.buttons[32]);
        };
        idle(500);
        show("start");
        for (const auto& [id, name] : {std::pair{26, "after ON"}, std::pair{28, "after LATCH"}, std::pair{26, "after ON again"}}) {
            emulator.panelInput({Emulator::PanelInput::Kind::Button, id, 1, 0});
            idle(150);
            emulator.panelInput({Emulator::PanelInput::Kind::Button, id, 0, 0});
            idle(500);
            show(name);
        }
        return 0;
    }

    // HYDRA_SOAK=<seconds>: runs that long (emulated, unpaced) and watches the
    // panel: every emulated second counts the panel captures (any display or LED
    // change) and the link bytes; every 5 s turns encoder 1 by a detent (display
    // 1 must change within a second) and plays a short note. One line per 10 s.
    if (const char* soak = std::getenv("HYDRA_SOAK")) {
        const double seconds = std::atof(soak);
        const bool paced = std::getenv("HYDRA_SOAK_PACED") != nullptr;  // drain at real-time pace, as a host would
        const bool hold = std::getenv("HYDRA_SOAK_HOLD") != nullptr;    // hold one note throughout instead of short notes
        const bool reselect = std::getenv("HYDRA_SOAK_RESELECT") != nullptr;  // select patch A001 again every 30 s
        const bool still = std::getenv("HYDRA_SOAK_STILL") != nullptr;  // no encoder turns: count spontaneous changes of the small displays
        std::vector<float> sink(8192);
        if (argc > 3) {  // the bank's first patch to A001, so that the displays animate
            std::vector<Patch> patches = loadPatches(argv[3]);
            const std::size_t pick = std::getenv("HYDRA_SOAK_PATCH") ? std::strtoul(std::getenv("HYDRA_SOAK_PATCH"), nullptr, 10) : 0;
            patches.erase(patches.begin(), patches.begin() + static_cast<std::ptrdiff_t>(std::min(pick, patches.size() - 1)));
            patches.resize(1);
            patches[0].slot = 0;
            emulator.uploadPatches(std::move(patches), 0);
            while (emulator.uploading())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::printf("patch uploaded\n");
        }
        if (hold) {
            const std::uint8_t on[] = {0x90, 48, 100};
            emulator.sendMidi(on, 3);
        }
        static std::map<std::uint64_t, std::uint64_t> idleSites;
        if (std::getenv("HYDRA_SOAK_IDLE"))
            emulator.machine()->panel().core().idleSites = &idleSites;
        const bool profileTask = std::getenv("HYDRA_SOAK_TASK") != nullptr;  // count the display task's loop (firmware 2.2.0: 0x2fd4c)
        if (profileTask)
            emulator.machine()->cpu().setProfileBlocks(true);
        FrontPanel::Lights lights;
        std::uint64_t generation = 0;
        const auto now = [&] { return static_cast<double>(emulator.framesProduced()) / Emulator::kSampleRate; };
        const double start = now();
        double nextSecond = start + 1, nextTurn = start + 5, turnAt = 0, noteOffAt = 0;
        std::uint64_t updates = 0, ledUpdates = 0, linkBytes = emulator.profile().linkBytes, oledFrames = 0;
        FrontPanel::Lights previous;
        std::array<std::uint64_t, 5> smallChanges{};
        std::map<std::uint8_t, std::uint64_t> lastFrames;
        std::array<Machine::FrameGaps, 16> lastGaps{};
        std::uint64_t lastExceptions = 0, lastReplies = 0, lastIdle = 0, lastCycles = 0, lastReads0 = 0;
        std::array<std::uint64_t, 61> lastHist{};
        std::map<std::uint32_t, std::uint64_t> lastBlocks;
        std::uint64_t lastAdc = 0;
        int failures = 0, second = 0, turnsSent = 0;
        bool turnPending = false, noteOn = false;
        std::remove_reference_t<decltype(lights.displays[1])> before{};
        const auto dumpPanel = [&] {
            Machine& m = *emulator.machine();
            hydra::PanelMcu& panel = m.panel();
            std::printf("    panel TIM2: CR1 %08x DIER %08x SR %08x CNT %u PSC %u ARR %u CCR2 %u; DMA1 ch1 CCR %08x CNDTR %u ISR %08x; ADC CR2 %08x SR %08x; "
                        "SPI2 overruns %llu; core %llu cycles, %llu idle, %llu exceptions; replies %llu\n",
                        panel.bus().read(0x40000000, 4), panel.bus().read(0x4000000C, 4), panel.bus().read(0x40000010, 4),
                        panel.bus().read(0x40000024, 4), panel.bus().read(0x40000028, 4), panel.bus().read(0x4000002C, 4),
                        panel.bus().read(0x40000038, 4), panel.bus().read(0x40020008, 4), panel.bus().read(0x4002000C, 4),
                        panel.bus().read(0x40020000, 4), panel.bus().read(0x40012408, 4), panel.bus().read(0x40012400, 4),
                        static_cast<unsigned long long>(panel.spi(1).overruns), static_cast<unsigned long long>(panel.core().now()),
                        static_cast<unsigned long long>(panel.core().idleTicks()), static_cast<unsigned long long>(panel.core().exceptionsTaken()),
                        static_cast<unsigned long long>(m.panelLinkReplies()));
            std::printf("    panel RAM: [200069b5]=%02x [20000002]=%02x [200000f5]=%02x\n", panel.core().read(0x200069B5, 1),
                        panel.core().read(0x20000002, 1), panel.core().read(0x200000F5, 1));
            std::vector<std::pair<std::uint64_t, std::uint32_t>> sites;
            for (const auto& [key, count] : m.linkOverrunSites())
                sites.emplace_back(count, key);
            std::sort(sites.rbegin(), sites.rend());
            std::printf("    link overruns by panel state (exception, masked, pc):");
            for (std::size_t i = 0; i < std::min<std::size_t>(sites.size(), 12); ++i)
                std::printf(" %llu@%u%s/%06x", static_cast<unsigned long long>(sites[i].first), sites[i].second >> 24,
                            sites[i].second & 1u << 23 ? "m" : "", sites[i].second & 0x7FFFFF);
            std::printf("\n");
        };
        while (now() - start < seconds) {
            if (paced) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                emulator.readAudio(sink.data(), sink.data() + 4096, 221);
            } else if (emulator.readAudio(sink.data(), sink.data() + 4096, 4096) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (emulator.readPanel(lights, generation)) {
                ++updates;
                for (std::size_t d = 1; d < 5; ++d)
                    if (lights.displays[d] != previous.displays[d] && now() - start > 3)
                        ++smallChanges[d];
                if (lights.buttons != previous.buttons || lights.rings != previous.rings || lights.pads != previous.pads) {
                    ++ledUpdates;
                    if (ledUpdates <= 3 && second < 12) {
                        std::string diff = "    LED changes:";
                        for (std::size_t i = 0; i < lights.buttons.size(); ++i)
                            if (lights.buttons[i] != previous.buttons[i])
                                diff += std::format(" b{}:{:.3f}>{:.3f}", i, previous.buttons[i], lights.buttons[i]);
                        for (std::size_t i = 0; i < lights.pads.size(); ++i)
                            if (lights.pads[i] != previous.pads[i])
                                diff += std::format(" pad{}:{:06x}>{:06x}", i, previous.pads[i], lights.pads[i]);
                        if (lights.rings != previous.rings)
                            diff += " rings";
                        std::printf("%s\n", diff.c_str());
                    }
                }
                previous = lights;
            }
            if (turnPending && lights.displays[1] != before)
                turnPending = false;
            if (turnPending && now() > turnAt + 1.0) {
                ++failures;
                turnPending = false;
                std::printf("  t=%.0f s: display 1 did not react to the encoder turn\n", now() - start);
                if (failures <= 3)
                    dumpPanel();
            }
            if (noteOn && now() >= noteOffAt) {
                const std::uint8_t off[] = {0x80, 60, 0};
                emulator.sendMidi(off, 3);
                noteOn = false;
            }
            if (still && now() >= nextTurn) {
                nextTurn += 5;
            } else if (now() >= nextTurn) {
                before = lights.displays[1];
                turnsSent += 1;
                emulator.panelInput({Emulator::PanelInput::Kind::Encoder, 0, turnsSent % 2 ? 1 : -1, 0});  // back and forth, so the parameter never hits its end
                turnPending = true;
                turnAt = now();
                nextTurn += 5;
                if (!hold) {
                    const std::uint8_t on[] = {0x90, 60, 100};
                    emulator.sendMidi(on, 3);
                    noteOn = true;
                    noteOffAt = now() + 0.5;
                }
            }
            if (now() >= nextSecond) {
                nextSecond += 1;
                if (reselect && second % 30 == 25) {
                    emulator.selectPatch(0, 0);
                    std::printf("  (patch A001 selected again)\n");
                }
                if (++second % 10 == 0) {
                    const std::uint64_t bytes = emulator.profile().linkBytes;
                    const std::uint64_t frames = emulator.machine()->panel().displays().display(0).frames();
                    std::map<std::uint8_t, std::uint64_t> linkFrames = emulator.machine()->linkFrames();
                    std::string perType;
                    for (const auto& [type, count] : linkFrames) {
                        perType += (perType.empty() ? " frames/s:" : ",") + std::format(" {:02x}={:.1f}", type, (count - lastFrames[type]) / 10.0);
                        lastFrames[type] = count;
                    }
                    const auto gaps = emulator.machine()->frameChangeGaps();
                    std::string gapText = "\n    changed frames/s by gap <15/<30/<60/<120/more ms (unchanged):";
                    for (std::size_t t = 0; t < gaps.size(); ++t) {
                        if (gaps[t] == lastGaps[t])
                            continue;
                        gapText += std::format("  {:02x}:", t);
                        for (int i = 0; i < 5; ++i)
                            gapText += std::format(" {:.1f}", (gaps[t][static_cast<std::size_t>(i)] - lastGaps[t][static_cast<std::size_t>(i)]) / 10.0);
                        gapText += std::format(" ({:.1f})", (gaps[t][5] - lastGaps[t][5]) / 10.0);
                    }
                    lastGaps = gaps;
                    {
                        Machine& m = *emulator.machine();
                        const std::uint64_t exceptions = m.panel().core().exceptionsTaken(), replies = m.panelLinkReplies(),
                                            idle = m.panel().core().idleTicks(), cycles = m.panel().core().now();
                        std::printf("    panel: %.0f exceptions/s, %.0f reply bytes/s, %.1f%% idle, clock %+.3f s vs main CPU\n",
                                    (exceptions - lastExceptions) / 10.0, (replies - lastReplies) / 10.0,
                                    100.0 * static_cast<double>(idle - lastIdle) / static_cast<double>(std::max<std::uint64_t>(1, cycles - lastCycles)),
                                    static_cast<double>(cycles) / 72e6 - static_cast<double>(m.cpu().ticksExecuted()) / static_cast<double>(Machine::kCpuHz));
                        lastExceptions = exceptions; lastReplies = replies; lastIdle = idle; lastCycles = cycles;
                    }
                    {
                        const auto hist = emulator.machine()->frameGapHistogram();
                        std::string line = "    frame-to-frame gaps (0.5 ms buckets, per s):";
                        for (std::size_t i = 0; i < hist.size(); ++i)
                            line += std::format(" {:.0f}", (hist[i] - lastHist[i]) / 10.0);
                        lastHist = hist;
                        {
                            Machine& mm = *emulator.machine();
                            std::string h = "    send after ready (100 us):";
                            for (std::size_t i = 0; i < mm.sendAfterReady_.size(); ++i)
                                h += std::format(" {}", mm.sendAfterReady_[i]);
                            h += "\n    ready after last byte (100 us):";
                            for (std::size_t i = 0; i < mm.readyAfterFrame_.size(); ++i)
                                h += std::format(" {}", mm.readyAfterFrame_[i]);
                            mm.sendAfterReady_.fill(0);
                            mm.readyAfterFrame_.fill(0);
                            std::printf("%s\n", h.c_str());
                        }
                        const auto reads = emulator.machine()->gpioInputReads();
                        line += std::format("; handshake reads/s {:.0f}", (reads[0] - lastReads0) / 10.0);
                        lastReads0 = reads[0];
                        std::printf("%s\n", line.c_str());
                    }
                    if (profileTask) {
                        const auto& counts = emulator.machine()->cpu().blockCounts();
                        std::string line = "    display task blocks (pc:count/s):";
                        for (const auto& [pc, entry] : counts)
                            if (pc >= 0x2fd4c && pc < 0x2fe60) {
                                line += std::format(" {:x}:{:.0f}", pc, (entry.first - lastBlocks[pc]) / 10.0);
                                lastBlocks[pc] = entry.first;
                            }
                        for (const std::uint32_t pc : {0x18ad8u, 0x1aac8u, 0x334f8u, 0x4f7d8u, 0x49c84u})
                            if (auto it = counts.find(pc); it != counts.end()) {
                                line += std::format(" | {:x}:{:.0f}", pc, (it->second.first - lastBlocks[pc]) / 10.0);
                                lastBlocks[pc] = it->second.first;
                            }
                        line += std::format(" | ADC conversions/s {:.0f}", (emulator.machine()->adcConversions() - lastAdc) / 10.0);
                        lastAdc = emulator.machine()->adcConversions();
                        std::printf("%s\n", line.c_str());
                    }
                    std::printf("t=%4d s: %5.1f panel updates/s (%.1f with LEDs), %5.1f main display redraws/s, %7.0f link bytes/s,%s%s %s\n", second, updates / 10.0, ledUpdates / 10.0,
                                (frames - oledFrames) / 10.0, (bytes - linkBytes) / 10.0, perType.c_str(), gapText.c_str(), emulator.status().c_str());
                    oledFrames = frames;
                    std::fflush(stdout);
                    updates = ledUpdates = 0;
                    linkBytes = bytes;
                    if (second % 60 == 0)
                        dumpPanel();
                }
            }
        }
        dumpPanel();
        std::printf("DMA1 ch1 completions: %llu in its handler, %llu unacknowledged\n",
                    static_cast<unsigned long long>(emulator.machine()->panel().dma1().completionsInHandler),
                    static_cast<unsigned long long>(emulator.machine()->panel().dma1().completionsUnacknowledged));
        std::printf("small display changes: %llu %llu %llu %llu\n", static_cast<unsigned long long>(smallChanges[1]),
                    static_cast<unsigned long long>(smallChanges[2]), static_cast<unsigned long long>(smallChanges[3]),
                    static_cast<unsigned long long>(smallChanges[4]));
        if (std::getenv("HYDRA_SOAK_SCREEN"))
            if (std::FILE* f = std::fopen(std::getenv("HYDRA_SOAK_SCREEN"), "wb")) {
                std::fprintf(f, "P5\n128 320\n255\n");
                for (std::size_t d = 0; d < 5; ++d)
                    std::fwrite(lights.displays[d].data(), 1, lights.displays[d].size(), f);
                std::fclose(f);
            }
        if (!idleSites.empty()) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> order;
            for (const auto& [k, c] : idleSites)
                order.emplace_back(c, k);
            std::sort(order.rbegin(), order.rend());
            std::printf("panel idle polls (count lr pc):");
            for (std::size_t i = 0; i < std::min<std::size_t>(order.size(), 10); ++i)
                std::printf(" %llu@%08llx/%08llx", static_cast<unsigned long long>(order[i].first),
                            static_cast<unsigned long long>(order[i].second >> 32), static_cast<unsigned long long>(order[i].second & 0xFFFFFFFF));
            std::printf("\n");
        }
        std::printf("soak: %d encoder turns without display reaction\n", failures);
        return failures ? 1 : 0;
    }

    // HYDRA_UPLOAD=<n>: uploads the first n patches of the bank to bank HYDRA_UPLOAD_BANK
    // (default 0), reports the time, then selects each in turn by program change and
    // writes the main display (PGM) to <out.wav>.<k>.pgm.
    if (const char* count = std::getenv("HYDRA_UPLOAD"); count && argc > 3) {
        std::vector<Patch> patches = loadPatches(argv[3]);
        patches.resize(std::min<std::size_t>(patches.size(), static_cast<std::size_t>(std::atoi(count))));
        const int bank = std::getenv("HYDRA_UPLOAD_BANK") ? std::atoi(std::getenv("HYDRA_UPLOAD_BANK")) : 0;
        const std::size_t total = patches.size();
        const auto t0 = Clock::now();
        if (!emulator.uploadPatches(std::move(patches), bank)) {
            std::printf("upload refused\n");
            return 1;
        }
        while (emulator.uploading()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::printf("\r  uploading %zu/%zu", emulator.uploadProgress().first, total);
            std::fflush(stdout);
        }
        std::printf("\nuploaded %zu patches in %.1f s\n", total, std::chrono::duration<double>(Clock::now() - t0).count());
        // The emulator only runs while the audio is drained.
        std::vector<float> sink(8192);
        const auto idle = [&](int milliseconds) {
            for (int i = 0; i < milliseconds / 5; ++i) {
                emulator.readAudio(sink.data(), sink.data() + 4096, 4096);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        idle(2000);
        Emulator::Patches stored;
        std::uint64_t storedGeneration = 0;
        if (emulator.readPatches(stored, storedGeneration)) {
            int count = 0;
            for (const std::string& name : stored.names)
                count += !name.empty();
            std::printf("patch list: %d stored, current %d '%s'\n", count, stored.current, stored.currentName.c_str());
        }
        int wrong = 0;
        for (std::size_t k = 0; k < total; ++k) {
            emulator.selectPatch(bank, static_cast<int>(k));
            idle(std::getenv("HYDRA_SELECT_MS") ? std::atoi(std::getenv("HYDRA_SELECT_MS")) : 1500);
            emulator.readPatches(stored, storedGeneration);  // `stored` keeps the last copy if unchanged
            const int expected = bank * Emulator::Patches::kSlots + static_cast<int>(k);
            const bool ok = stored.current == expected && stored.currentName == stored.names[static_cast<std::size_t>(expected)];
            wrong += !ok;
            std::printf("  selected %c%03zu: current %d '%s'%s\n", 'A' + bank, k + 1, stored.current, stored.currentName.c_str(),
                        ok ? "" : "  <-- MISMATCH");
            FrontPanel::Lights lights;
            std::uint64_t generation = 0;
            if (emulator.readPanel(lights, generation)) {
                const std::string path = std::string(argv[2]) + "." + std::to_string(k) + ".pgm";
                if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fprintf(f, "P5\n128 64\n255\n");
                    std::fwrite(lights.displays[0].data(), 1, lights.displays[0].size(), f);
                    std::fclose(f);
                }
            }
        }
        std::printf("program changes with a wrong result: %d\n", wrong);
        if (std::getenv("HYDRA_BROWSE")) {
            // Selects through the panel: BROWSE, then the arrow right (bank), dumping the display each time.
            const auto press = [&](int id, const char* name) {
                emulator.panelInput({Emulator::PanelInput::Kind::Button, id, 1, 0});
                idle(200);
                emulator.panelInput({Emulator::PanelInput::Kind::Button, id, 0, 0});
                idle(1500);
                FrontPanel::Lights lights;
                std::uint64_t generation = 0;
                if (emulator.readPanel(lights, generation))
                    if (std::FILE* f = std::fopen((std::string(argv[2]) + "." + name + ".pgm").c_str(), "wb")) {
                        std::fprintf(f, "P5\n128 64\n255\n");
                        std::fwrite(lights.displays[0].data(), 1, lights.displays[0].size(), f);
                        std::fclose(f);
                    }
            };
            press(45, "browse");
            press(55, "right1");
            press(55, "right2");
        }
        for (const auto& block : emulator.captureFlash())
            std::printf("flash sector %08x\n", 0x40000000 + block.offset);
        emulator.stop();
        return 0;
    }

    // Pulls audio like a host's callback would, every 512 frames of wall time.
    std::vector<float> out, left(kBlock), right(kBlock);
    std::size_t minBuffered = SIZE_MAX;
    auto next = Clock::now();
    const auto pull = [&](double seconds, bool record) {
        const auto blocks = static_cast<int>(seconds * Emulator::kSampleRate / kBlock);
        for (int b = 0; b < blocks; ++b) {
            next += std::chrono::microseconds(static_cast<long>(1e6 * kBlock / Emulator::kSampleRate));
            std::this_thread::sleep_until(next);
            minBuffered = std::min(minBuffered, emulator.bufferedFrames());
            if (b % static_cast<int>(44100 / kBlock) == 0) {
                static std::uint64_t lastProduced = 0;
                static auto lastTime = Clock::now();
                const double dt = std::chrono::duration<double>(Clock::now() - lastTime).count();
                std::printf("  buffered %4zu, produced %.2fx in the last %.2f s\n", emulator.bufferedFrames(),
                            double(emulator.framesProduced() - lastProduced) / Emulator::kSampleRate / dt, dt);
                lastProduced = emulator.framesProduced();
                lastTime = Clock::now();
            }
            emulator.readAudio(left.data(), right.data(), kBlock);
            if (record)
                for (std::size_t i = 0; i < kBlock; ++i) {
                    out.push_back(left[i]);
                    out.push_back(right[i]);
                }
        }
    };

    // Production rate with the buffer drained continuously (no pacing limit).
    const auto measureUnpaced = [&](const char* label) {
        const std::uint64_t before = emulator.framesProduced();
        // The emulator only runs ahead up to its target fill, so raise it for the
        // measurement: with the default 512 frames, draining every 5 ms caps it
        // at ~2.3 x real time regardless of the emulator's speed.
        const std::size_t fill = emulator.targetFill();
        emulator.setTargetFill(1 << 14);
        const Machine::Profile p0 = emulator.profile();
        const auto t0 = Clock::now();
        std::vector<float> sink(65536);
        while (Clock::now() - t0 < std::chrono::seconds(2)) {
            emulator.readAudio(sink.data(), sink.data() + 32768, 32768);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        emulator.setTargetFill(fill);
        std::printf("unpaced, %s: %.2fx real time\n", label, double(emulator.framesProduced() - before) / Emulator::kSampleRate / seconds);
        const Machine::Profile p1 = emulator.profile();
        std::printf("  per wall second: main CPU %.2f, DSP cores %.2f, effects %.2f, tone generator %.2f, panel %.2f, DSP wait %.2f\n",
                    (p1.cpu - p0.cpu) / 1e9 / seconds, (p1.dspCores - p0.dspCores) / 1e9 / seconds, (p1.effects - p0.effects) / 1e9 / seconds,
                    (p1.toneGenerator - p0.toneGenerator) / 1e9 / seconds, (p1.panel - p0.panel) / 1e9 / seconds, (p1.dspWait - p0.dspWait) / 1e9 / seconds);
        std::printf("  DSP thread waiting for its workers: %.2f per wall second\n", (p1.dspSync - p0.dspSync) / 1e9 / seconds);
        const double emulated = double(emulator.framesProduced() - before) / Emulator::kSampleRate;
        std::printf("  per emulated second: %.0f slices (%.0f idle)\n", (p1.slices - p0.slices) / emulated,
                    (p1.idleSlices - p0.idleSlices) / emulated);
        next = Clock::now();
    };
    measureUnpaced("no patch");
    // HYDRA_ARP_PLAY=<seconds>: turns the arpeggiator on (ARP ON), holds middle C
    // and pulls audio at real-time pace like a host for that long; writes the
    // take to <out.wav> and lists when underruns happened (wall seconds), for
    // checking the arpeggio's timing.
    if (const char* play = std::getenv("HYDRA_ARP_PLAY")) {
        pull(1.0, false);
        if (argc > 3) {  // patch HYDRA_ARP_PATCH (default 0) of the bank to A001
            const std::vector<Patch> patches = loadPatches(argv[3]);
            const std::size_t index = std::getenv("HYDRA_ARP_PATCH") ? std::strtoul(std::getenv("HYDRA_ARP_PATCH"), nullptr, 10) : 0;
            for (const auto& message : sysex::patchUpload(patches.at(index).body, 0, 0, true)) {
                emulator.sendMidi(message.data(), message.size());
                pull(0.1, false);
            }
            std::printf("uploaded '%s' to A001\n", patches[index].name.c_str());
            pull(0.5, false);
        }
        emulator.panelInput({Emulator::PanelInput::Kind::Button, 26, 1, 0});
        pull(0.15, false);
        emulator.panelInput({Emulator::PanelInput::Kind::Button, 26, 0, 0});
        pull(0.5, false);
        const std::uint8_t on[] = {0x90, 60, 100};
        emulator.sendMidi(on, 3);
        const auto blocks = static_cast<int>(std::atof(play) * Emulator::kSampleRate / kBlock);
        const auto start = Clock::now();
        std::uint64_t underruns = emulator.underruns();
        int late = 0;
        for (int b = 0; b < blocks; ++b) {
            next += std::chrono::microseconds(static_cast<long>(1e6 * kBlock / Emulator::kSampleRate));
            std::this_thread::sleep_until(next);
            if (Clock::now() - next > std::chrono::milliseconds(2))
                ++late;  // the test itself woke late
            const std::size_t got = emulator.readAudio(left.data(), right.data(), kBlock);
            if (emulator.underruns() != underruns) {
                underruns = emulator.underruns();
                std::printf("underrun at %.3f s: %zu of %zu frames\n", std::chrono::duration<double>(Clock::now() - start).count(), got, kBlock);
            }
            for (std::size_t i = 0; i < kBlock; ++i) {
                out.push_back(left[i]);
                out.push_back(right[i]);
            }
        }
        std::printf("arp play: %d blocks, %d woken late, slowest run chunk %llu us\n", blocks, late,
                    static_cast<unsigned long long>(emulator.slowestChunkMicros()));
        writeFloatWav(argv[2], out);
        emulator.stop();
        return 0;
    }
    // HYDRA_LOAD_TEST=i,j,...: the cost of patches i, j, ... of the bank (each
    // uploaded to A001) silent, with one note and with eight, as core time per
    // emulated second of the DSP cores, the main CPU and the panel MCU, and
    // whether the arpeggiator is on (HYDRA_LOAD_ARP toggles it after loading;
    // HYDRA_LOAD_PAGE=<button> presses a panel button then). Writes the
    // displays to <out.wav>.load<i>.pgm.
    if (const char* list = std::getenv("HYDRA_LOAD_TEST"); list && argc > 3) {
        const std::vector<Patch> patches = loadPatches(argv[3]);
        const auto cost = [&] {
            const std::uint64_t before = emulator.framesProduced();
            emulator.setTargetFill(1 << 14);
            const Machine::Profile p0 = emulator.profile();
            const auto t0 = Clock::now();
            std::vector<float> sink(65536);
            while (Clock::now() - t0 < std::chrono::milliseconds(1500)) {
                emulator.readAudio(sink.data(), sink.data() + 32768, 32768);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const Machine::Profile p1 = emulator.profile();
            emulator.setTargetFill(config.targetFill);
            const double emulated = double(emulator.framesProduced() - before) / Emulator::kSampleRate;
            std::printf("  DSP %.2f CPU %.2f panel %.2f", (p1.dspCores - p0.dspCores) / 1e9 / emulated,
                        (p1.cpu - p0.cpu) / 1e9 / emulated, (p1.panel - p0.panel) / 1e9 / emulated);
        };
        pull(0.5, false);
        for (const char* at = list; *at;) {
            const std::size_t index = std::strtoul(at, nullptr, 10);
            for (const auto& message : sysex::patchUpload(patches.at(index).body, 0, 0, true)) {
                emulator.sendMidi(message.data(), message.size());
                pull(0.1, false);
            }
            pull(0.5, false);
            if (std::getenv("HYDRA_LOAD_ARP")) {  // toggle the arpeggiator (ARP ON)
                emulator.panelInput({Emulator::PanelInput::Kind::Button, 26, 1, 0});
                pull(0.15, false);
                emulator.panelInput({Emulator::PanelInput::Kind::Button, 26, 0, 0});
                pull(0.5, false);
            }
            if (const char* page = std::getenv("HYDRA_LOAD_PAGE")) {  // press this panel button (a module page)
                emulator.panelInput({Emulator::PanelInput::Kind::Button, std::atoi(page), 1, 0});
                pull(0.15, false);
                emulator.panelInput({Emulator::PanelInput::Kind::Button, std::atoi(page), 0, 0});
                pull(0.5, false);
            }
            FrontPanel::Lights lights;
            std::uint64_t generation = 0;
            emulator.readPanel(lights, generation);
            if (std::FILE* f = std::fopen(std::format("{}.load{}.pgm", argv[2], index).c_str(), "wb")) {
                std::fprintf(f, "P5\n128 %zu\n255\n", lights.displays.size() * 64);
                for (const auto& display : lights.displays)
                    std::fwrite(display.data(), 1, display.size(), f);
                std::fclose(f);
            }
            std::printf("%3zu %-16s arp %s silent:", index, patches[index].name.c_str(), lights.buttons[30] > 0.5f ? "on " : "off");
            cost();
            const std::uint8_t on[] = {0x90, 60, 100};
            emulator.sendMidi(on, 3);
            std::printf(" | 1 note:");
            cost();
            for (const int note : {48, 52, 55, 64, 67, 71, 72})
                emulator.sendMidi(std::array<std::uint8_t, 3>{0x90, static_cast<std::uint8_t>(note), 100}.data(), 3);
            std::printf(" | 8 notes:");
            cost();
            std::printf("\n");
            std::fflush(stdout);
            for (const int note : {48, 52, 55, 60, 64, 67, 71, 72})
                emulator.sendMidi(std::array<std::uint8_t, 3>{0x80, static_cast<std::uint8_t>(note), 0}.data(), 3);
            pull(1.0, false);
            while (*at && *at != ',')
                ++at;
            if (*at == ',')
                ++at;
        }
        emulator.stop();
        return 0;
    }
    pull(argc > 4 ? std::atof(argv[4]) : 0.5, false);  // let the buffer settle (and the firmware finish starting)
    if (argc > 3) {
        std::vector<Patch> patches = loadPatches(argv[3]);
        for (const auto& message : sysex::patchUpload(patches.at(0).body, 0, 0, true)) {
            emulator.sendMidi(message.data(), message.size());
            pull(0.1, false);  // about the time the bytes take at MIDI speed, plus the reply
        }
        std::printf("uploaded '%s' to A001\n", patches[0].name.c_str());
        pull(0.5, false);
        measureUnpaced("patch loaded");
        const std::uint8_t on[] = {0x90, 60, 100}, off[] = {0x80, 60, 0};
        emulator.sendMidi(on, 3);
        measureUnpaced("note held");
        emulator.sendMidi(off, 3);
        pull(1.0, false);
    }

    const std::uint64_t underrunsBefore = emulator.underruns();
    emulator.resetChunkStats();
    minBuffered = SIZE_MAX;
    pull(0.5, true);
    const std::uint8_t noteOn[] = {0x90, 60, 100}, noteOff[] = {0x80, 60, 0};
    emulator.sendMidi(noteOn, 3);
    pull(1.0, true);
    emulator.sendMidi(noteOff, 3);
    pull(1.5, true);
    // A load test: four-note chords, changing every half second.
    for (int chord = 0; chord < 8; ++chord) {
        for (const int note : {48, 55, 60, 64}) {
            const std::uint8_t on[] = {0x90, static_cast<std::uint8_t>(note + chord % 4), 100};
            emulator.sendMidi(on, 3);
        }
        pull(0.5, false);
        for (const int note : {48, 55, 60, 64}) {
            const std::uint8_t off[] = {0x80, static_cast<std::uint8_t>(note + chord % 4), 0};
            emulator.sendMidi(off, 3);
        }
    }
    pull(1.0, false);

    float peak = 0;
    for (const float v : out)
        peak = std::max(peak, std::abs(v));
    std::printf("target fill %zu, block %zu: recorded %zu frames, peak %.3f, underruns %llu, overruns %llu, min buffered %zu frames\n", config.targetFill, kBlock, out.size() / 2,
                peak, static_cast<unsigned long long>(emulator.underruns() - underrunsBefore),
                static_cast<unsigned long long>(emulator.overruns()), minBuffered);
    std::printf("slowest run chunk %llu us, %llu chunks over 2 ms\n", static_cast<unsigned long long>(emulator.slowestChunkMicros()),
                static_cast<unsigned long long>(emulator.slowChunks()));
    // Restore test: restart with the captured flash and play again.
    const auto flash = emulator.captureFlash();
    std::printf("captured %zu flash sectors:", flash.size());
    for (const auto& block : flash)
        std::printf(" %08x", 0x40000000 + block.offset);
    std::printf("\n");
    emulator.stop();
    config.flash = flash;
    emulator.start(config);
    while (emulator.state() == Emulator::State::Booting)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    next = Clock::now();
    pull(argc > 4 ? std::atof(argv[4]) : 0.5, false);
    std::vector<float> restored;
    out.swap(restored);
    emulator.sendMidi(noteOn, 3);
    pull(1.0, true);
    emulator.sendMidi(noteOff, 3);
    pull(0.5, true);
    float restoredPeak = 0;
    for (const float v : out)
        restoredPeak = std::max(restoredPeak, std::abs(v));
    std::printf("restored: peak %.3f\n", restoredPeak);
    out.swap(restored);
    emulator.stop();
    return writeFloatWav(argv[2], out) ? 0 : 1;
}
