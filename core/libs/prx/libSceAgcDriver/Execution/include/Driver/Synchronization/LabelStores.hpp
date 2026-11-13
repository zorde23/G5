#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATION_LABELSTORES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SYNCHRONIZATION_LABELSTORES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace AgcDriver::DriverDetail {

inline constexpr std::size_t LabelStoreHistory = 4;
inline constexpr std::size_t LabelStoreBytes = 64;

inline constexpr std::size_t LabelStoreSweepAt = 65536;
inline constexpr std::uint64_t LabelStoreRetireWindow = 1ull << 22;

template <class History>
std::size_t EvictRetiredLabelStores(std::unordered_map<std::uint64_t, History>& stores, std::uint64_t cutoff) {
    return std::erase_if(stores, [&](const auto& entry) { return std::ranges::all_of(entry.second, [&](const auto& store) { return store.stamp <= cutoff; }); });
}

inline bool KeepsLabelStore(std::uint64_t address, std::size_t bytes) {
    return bytes != 0 && bytes <= LabelStoreBytes && bytes % 4 == 0 && address % 4 == 0;
}

}

#endif
