// Starts the DSP block's worker threads and immediately asks for a sample, many
// times: a worker that starts late must still take part in that first sample
// (else advanceToSample waits for it forever). Run it on one core to make late
// starts likely: taskset -c 0 dsp_start_race
#include <cstdio>
#include <memory>

#include "dsp_block.h"

int main() {
    for (int round = 0; round < 3000; ++round) {
        auto block = std::make_unique<hydra::DspBlock>();
        block->setThreads(4);
        block->advanceToSample(3);
        if (round % 500 == 0) {
            std::printf("round %d ok\n", round);
            std::fflush(stdout);
        }
    }
    std::printf("PASS\n");
}
