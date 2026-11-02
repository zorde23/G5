#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDEDDRAWSTATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDEDDRAWSTATE_HPP

#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace AgcDriver::Graphics {

enum RecordedSlot : unsigned {
    KeptViewport, KeptScissor, KeptDepthBias, KeptDepthBounds, KeptCull, KeptFrontFace, KeptDepthTest, KeptDepthWrite, KeptDepthCompare, KeptDepthBoundsTest, KeptStencilTest,
    KeptStencilOpFront, KeptStencilOpBack, KeptCompareMaskFront, KeptCompareMaskBack, KeptWriteMaskFront, KeptWriteMaskBack, KeptReferenceFront, KeptReferenceBack, RecordedSlotCount
};
static_assert(RecordedSlotCount <= 32);

struct RecordedDrawState {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    std::uint64_t generation = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::uint32_t known = 0;
    std::array<std::array<std::byte, 24>, RecordedSlotCount> values{};

    bool Bind(VkPipeline bound, std::uint32_t dynamicSlots) {
        if (pipeline == bound) return false;
        pipeline = bound;
        known &= dynamicSlots;
        return true;
    }
};

template <typename... T>
bool Unchanged(RecordedDrawState* recorded, unsigned slot, const T&... values) {
    if (recorded == nullptr) return false;
    static_assert((sizeof(T) + ... + 0) <= 24);
    std::array<std::byte, 24> packed{};
    std::size_t at = 0;
    ((std::memcpy(packed.data() + at, &values, sizeof(T)), at += sizeof(T)), ...);
    const bool same = (recorded->known >> slot & 1u) != 0 && recorded->values[slot] == packed;
    recorded->values[slot] = packed;
    recorded->known |= 1u << slot;
    return same;
}

inline std::uint32_t RecordedSlotsOf(std::span<const VkDynamicState> states) {
    std::uint32_t slots = 0;
    const auto add = [&](RecordedSlot first, RecordedSlot second) { slots |= (1u << first) | (1u << second); };
    for (const auto state : states) {
        switch (state) {
            case VK_DYNAMIC_STATE_VIEWPORT: add(KeptViewport, KeptViewport); break;
            case VK_DYNAMIC_STATE_SCISSOR: add(KeptScissor, KeptScissor); break;
            case VK_DYNAMIC_STATE_DEPTH_BIAS: add(KeptDepthBias, KeptDepthBias); break;
            case VK_DYNAMIC_STATE_DEPTH_BOUNDS: add(KeptDepthBounds, KeptDepthBounds); break;
            case VK_DYNAMIC_STATE_CULL_MODE_EXT: add(KeptCull, KeptCull); break;
            case VK_DYNAMIC_STATE_FRONT_FACE_EXT: add(KeptFrontFace, KeptFrontFace); break;
            case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT: add(KeptDepthTest, KeptDepthTest); break;
            case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT: add(KeptDepthWrite, KeptDepthWrite); break;
            case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT: add(KeptDepthCompare, KeptDepthCompare); break;
            case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT: add(KeptDepthBoundsTest, KeptDepthBoundsTest); break;
            case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT: add(KeptStencilTest, KeptStencilTest); break;
            case VK_DYNAMIC_STATE_STENCIL_OP_EXT: add(KeptStencilOpFront, KeptStencilOpBack); break;
            case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: add(KeptCompareMaskFront, KeptCompareMaskBack); break;
            case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: add(KeptWriteMaskFront, KeptWriteMaskBack); break;
            case VK_DYNAMIC_STATE_STENCIL_REFERENCE: add(KeptReferenceFront, KeptReferenceBack); break;
            default: break;
        }
    }
    return slots;
}

bool RecordStateFilter();
void ForgetRecordedDrawState();
RecordedDrawState* RecordedFor(VkCommandBuffer commands, bool enabled = RecordStateFilter());

}

#endif
