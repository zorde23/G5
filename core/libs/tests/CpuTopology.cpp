#include "prx/libc/include/CpuTopology.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "cpu topology test failed: %s\n", what);
    std::exit(1);
}

#ifdef __linux__
static std::uint64_t Parse(const char* text) {
    const auto path = std::filesystem::temp_directory_path() / ("anyps5_cpu_topology_test_cpus-" + std::to_string(std::random_device{}()));
    {
        std::ofstream file(path, std::ios::trunc);
        file << text;
    }
    const auto mask = CpuTopology::MaskFromCpuList(path.c_str());
    std::filesystem::remove(path);
    return mask;
}
#endif

int main() {
#ifdef __linux__
    Require(Parse("0-15\n") == 0xffffull, "range");
    Require(Parse("16-31\n") == 0xffff0000ull, "high range");
    Require(Parse("0,2,4-5\n") == 0x35ull, "list of cpus and ranges");
    Require(Parse("3") == 0x8ull, "single cpu without a newline");
    Require(Parse("62-70\n") == 0xc000000000000000ull, "cpus past 63 are left out");
    Require(Parse("") == 0, "empty list");
    Require(Parse("4-2\n") == 0, "reversed range");
    Require(Parse("0-x\n") == 0, "malformed range");
    Require(Parse("0;1\n") == 0, "malformed separator");
    Require(CpuTopology::MaskFromCpuList("/nonexistent/anyps5/cpus") == 0, "missing file");
#endif

    {
        std::uint64_t byClass[4] = {};
        bool seen[4] = {};
        const auto empty = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(!empty.hybrid && empty.efficient == 0 && empty.performant == 0, "empty classes");

        byClass[0] = 0xff;
        seen[0] = true;
        const auto single = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(!single.hybrid && single.efficient == 0 && single.performant == 0, "single class");

        byClass[1] = 0xff00;
        seen[1] = true;
        const auto twoTier = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(twoTier.hybrid && twoTier.efficient == 0xff && twoTier.performant == 0xff00, "two-tier classes");

        byClass[2] = 0xff0000;
        seen[2] = true;
        const auto threeTier = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(threeTier.hybrid && threeTier.efficient == 0xffff && threeTier.performant == 0xff0000, "three-tier classes aggregate efficient");
    }

    {
        CpuTopology::Layout smt{};
        smt.process = 0xffff;
        for (unsigned core = 0; core < 8; ++core) {
            smt.cores.push_back(std::uint64_t{3} << (2 * core));
            smt.coreClasses.push_back(0);
        }
        Require(CpuTopology::ReservedCoreFrom(smt) == 0xc000, "SMT: the last physical core with both siblings");
        smt.process = 0x7fff;
        Require(CpuTopology::ReservedCoreFrom(smt) == 0x3000, "a core partly outside the process is skipped");
        CpuTopology::Layout hybrid{};
        hybrid.process = 0xfffff;
        for (unsigned core = 0; core < 6; ++core) {
            hybrid.cores.push_back(std::uint64_t{3} << (2 * core));
            hybrid.coreClasses.push_back(1);
        }
        for (unsigned core = 12; core < 20; ++core) {
            hybrid.cores.push_back(std::uint64_t{1} << core);
            hybrid.coreClasses.push_back(0);
        }
        Require(CpuTopology::ReservedCoreFrom(hybrid) == 0xc00, "hybrid: a performance core");
        CpuTopology::Layout plain{};
        plain.process = 0x3f;
        for (unsigned core = 0; core < 6; ++core) {
            plain.cores.push_back(std::uint64_t{1} << core);
            plain.coreClasses.push_back(0);
        }
        Require(CpuTopology::ReservedCoreFrom(plain) == 0x20, "no SMT: one logical cpu");
        plain.cores.pop_back();
        plain.coreClasses.pop_back();
        Require(CpuTopology::ReservedCoreFrom(plain) == 0, "fewer than 6 physical cores: off");
    }

    const auto& layout = CpuTopology::Get();
    Require(layout.process != 0, "process affinity read");
#ifdef _WIN32
    Require(!layout.cores.empty() && layout.cores.size() == layout.coreClasses.size(), "physical cores read");
#endif
    Require(!layout.hybrid || (layout.performant & layout.efficient) == 0, "hybrid classes overlap");
    const std::uint64_t first = layout.process & (~layout.process + 1);
    Require(CpuTopology::Pin(nullptr, first) == first, "pin to one cpu of the process");
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    Require(pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0, "read thread affinity");
    Require(CPU_COUNT(&set) == 1 && CPU_ISSET(static_cast<unsigned>(__builtin_ctzll(first)), &set), "thread runs on the pinned cpu only");
    Require(CpuTopology::Pin(nullptr, ~layout.process) == 0, "mask outside the process is not applied");
    int dummy = 0;
    Require(CpuTopology::Pin(&dummy, first) == 0, "other threads are not pinned by handle");
#endif
    return 0;
}
