#include "threads.h"

#include <algorithm>
#include <cstdlib>

#if defined(__linux__)
#include <fstream>
#include <string>

#include <pthread.h>
#include <sched.h>
#endif

namespace hydra {

#if defined(__linux__)
namespace {
// The core's relative capacity (1024 = the fastest), else its maximum clock,
// else 0 (unknown).
long coreCapacity(int cpu) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
    for (const char* file : {"/cpu_capacity", "/cpufreq/cpuinfo_max_freq"}) {
        std::ifstream in(base + file);
        long value = 0;
        if (in >> value && value > 0)
            return value;
    }
    return 0;
}
}  // namespace
#endif

void initEmulatorThread(const char* name) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
    if (std::getenv("HYDRA_ALL_CORES"))
        return;
    cpu_set_t allowed;
    if (pthread_getaffinity_np(pthread_self(), sizeof allowed, &allowed) != 0)
        return;
    long capacity[CPU_SETSIZE] = {}, fastest = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &allowed)) {
            capacity[cpu] = coreCapacity(cpu);
            if (capacity[cpu] == 0)
                return;  // not known for every core: leave the affinity alone
            fastest = std::max(fastest, capacity[cpu]);
        }
    cpu_set_t fast;
    CPU_ZERO(&fast);
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &allowed) && 2 * capacity[cpu] >= fastest)
            CPU_SET(cpu, &fast);
    if (CPU_COUNT(&fast) < CPU_COUNT(&allowed))
        pthread_setaffinity_np(pthread_self(), sizeof fast, &fast);
#else
    (void)name;
#endif
}

}  // namespace hydra
