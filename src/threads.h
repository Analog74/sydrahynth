#pragma once

namespace hydra {

// Sets up the calling thread as one of the emulator's: names it (shown by
// top -H and ps -L) and keeps it off cores with less than half the capacity
// of the fastest core it may use, such as the low-power E-cores of Intel's
// Core Ultra chips. The DSP side syncs every sample, so one thread on such a
// core holds up all the others: on a Core Ultra 5 235H (P-cores 1024, E-cores
// 793, LP E-cores 320) the emulator ran below real time at times and the
// output had underruns, without the two LP E-cores none. Cores of equal
// capacity, the usual case, are all kept. HYDRA_ALL_CORES=1 keeps every core.
void initEmulatorThread(const char* name);

}  // namespace hydra
