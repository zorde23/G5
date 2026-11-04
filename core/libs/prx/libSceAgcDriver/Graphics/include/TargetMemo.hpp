#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TARGETMEMO_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_TARGETMEMO_HPP

#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace AgcDriver::Graphics {

struct TargetMemoKey {
    std::array<std::uint64_t, 22> words{};
    bool operator==(const TargetMemoKey&) const = default;
};

template <typename Resident>
class TargetMemo {
public:
    struct Entry {
        TargetMemoKey key;
        std::shared_ptr<Resident> resident;
        VkImageView view = VK_NULL_HANDLE;
    };

    enum class Lookup : std::uint8_t { Miss, Hit, Verify };

    explicit TargetMemo(std::uint64_t verifyEvery = 0) : verifyEvery(verifyEvery) {}

    Lookup Find(std::uint64_t address, const TargetMemoKey& key, Entry*& found) {
        found = nullptr;
        for (auto& entry : entries) {
            if (entry.resident != nullptr && entry.key.words[0] == address) found = &entry;
        }
        if (found == nullptr || !(found->key == key)) return Lookup::Miss;
        return verifyEvery != 0 && ++hits % verifyEvery == 0 ? Lookup::Verify : Lookup::Hit;
    }

    void Store(const TargetMemoKey& key, std::shared_ptr<Resident> resident, VkImageView view) {
        Entry* slot = nullptr;
        for (auto& entry : entries) {
            if (entry.resident != nullptr && entry.key.words[0] == key.words[0]) slot = &entry;
        }
        if (slot == nullptr) slot = &entries[next++ % entries.size()];
        *slot = {key, std::move(resident), view};
    }

    void Forget(std::uint64_t address) {
        for (auto& entry : entries) {
            if (entry.key.words[0] == address) entry = {};
        }
    }

    void Clear() { entries = {}; }

private:
    std::array<Entry, 16> entries{};
    std::size_t next = 0;
    std::uint64_t hits = 0;
    std::uint64_t verifyEvery = 0;
};

}

#endif
