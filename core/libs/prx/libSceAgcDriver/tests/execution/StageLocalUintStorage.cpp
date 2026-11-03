#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "RuntimeAbi.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::ShaderResources;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 8;
constexpr std::uint32_t WaveSize = 64;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t TwoChannelSwizzle = 0x22cu;
constexpr std::uint32_t Format16_16Sint = 28u;
constexpr std::uint32_t Format16_16Float = 29u;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t TexelBytes = 8192;
constexpr std::uint8_t Sentinel = 0xabu;

alignas(256) constexpr std::array<std::uint32_t, 8> PixelFloatCode{
    0x7e040f00u, 0x7e060f01u, 0xf0201308u, 0x00000002u, 0x7e0e02f2u, 0xf800180fu, 0x07070707u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelSintCode{
    0x7e040f00u, 0x7e060f01u, 0xf0201308u, 0x00000202u, 0x7e0e02f2u, 0xf800180fu, 0x07070707u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 7> ComputeFloatCode{
    0x7e080d00u, 0x7e0a0d01u, 0x060808f0u, 0x060a0af0u, 0xf0201308u, 0x00000400u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 3> ComputeSintCode{0xf0201308u, 0x00000000u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u,
};
alignas(256) std::array<std::array<float, 4>, 3> Triangle{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "stage local uint storage: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

struct Case {
    const char* name;
    std::uint32_t format;
    bool sint;
};

constexpr Case Float16{"R16G16_SFLOAT", Format16_16Float, false};
constexpr Case Sint16{"R16G16_SINT", Format16_16Sint, true};

bool SwitchOn() {
    return std::getenv("APS5_TMP_STAGE_LOCAL_UINT_STORAGE") != nullptr;
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::uint16_t Half(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    if ((bits & 0x7fffffffu) == 0u) return static_cast<std::uint16_t>(bits >> 16u);
    const auto exponent = static_cast<std::int32_t>((bits >> 23u) & 0xffu) - 127 + 15;
    Require(exponent > 0 && exponent < 31 && (bits & 0x1fffu) == 0u, "stage local uint storage: expected value is not an exact half");
    return static_cast<std::uint16_t>(((bits >> 16u) & 0x8000u) | (static_cast<std::uint32_t>(exponent) << 10u) | ((bits >> 13u) & 0x3ffu));
}

std::array<std::uint16_t, 2> Expected(const Case& which, std::uint32_t x, std::uint32_t y) {
    if (which.sint) return {static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)};
    return {Half(static_cast<float>(x) + 0.5f), Half(static_cast<float>(y) + 0.5f)};
}

AgcDriver::Graphics::TileMipLayout Mip(const Case& which) {
    return AgcDriver::Graphics::ComputeMipLayout(AgcDriver::Graphics::TextureTileMode::kLinear, which.format, Width, Height, 1u).at(0);
}

std::array<std::uint32_t, 8> TextureDescriptor(const void* data, std::uint32_t format) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        TwoChannelSwizzle | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::array<std::uint32_t, 4> VertexBuffer(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

ShaderRecompiler::RecompileResult CompileCompute(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Case& which) {
    const std::span<const std::uint32_t> code = which.sint ? std::span<const std::uint32_t>(ComputeSintCode) : std::span<const std::uint32_t>(ComputeFloatCode);
    const auto texture = TextureDescriptor(texels, which.format);
    static std::vector<std::uint32_t> userData;
    userData.assign(texture.begin(), texture.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Width, Height, 1}, 0u, {false, false, false}, false, 2};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {WaveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

struct Programs {
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult pixel;
    std::uint32_t vertexPush;
};

Programs CompileGraphics(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Case& which) {
    const auto target = device.Target();
    static std::vector<std::uint32_t> vertexUserData;
    vertexUserData.assign(4, 0u);
    const auto vertices = VertexBuffer(Triangle.data(), 16u, static_cast<std::uint32_t>(Triangle.size()));
    std::copy(vertices.begin(), vertices.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {WaveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    const std::span<const std::uint32_t> code = which.sint ? std::span<const std::uint32_t>(PixelSintCode) : std::span<const std::uint32_t>(PixelFloatCode);
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = false;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const auto texture = TextureDescriptor(texels, which.format);
    static std::vector<std::uint32_t> pixelUserData;
    pixelUserData.assign(texture.begin(), texture.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {WaveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    return {std::move(vertexResult), ShaderRecompiler::Recompile(fragment), vertexPush};
}

void Reset(std::uint8_t* texels) {
    std::fill_n(texels, TexelBytes, Sentinel);
}

void Finish(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    device.WaitIdle();
    AgcDriver::Graphics::StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(texels), TexelBytes, nullptr, "test");
    device.WaitIdle();
}

void Dispatch(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Case& which) {
    const auto result = CompileCompute(device, texels, which);
    Reset(texels);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(which.sint ? ComputeSintCode.data() : ComputeFloatCode.data()));
    Finish(device, texels);
}

void Draw(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Case& which) {
    const auto target = device.Target();
    const auto compiled = CompileGraphics(device, texels, which);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &compiled.vertex, 0},
        {ShaderStage::Fragment, &compiled.pixel, PixelPushOffset(compiled.vertexPush, target)}
    }};
    Pixels.fill(std::byte{0});
    Reset(texels);
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, WaveSize, WaveSize, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    Finish(device, texels);
}

void CheckTexels(const std::uint8_t* texels, const Case& which, std::string_view stage) {
    const auto mip = Mip(which);
    std::uint32_t wrong = 0;
    std::string first;
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            std::array<std::uint16_t, 2> actual{};
            std::memcpy(actual.data(), texels + mip.tiledOffset + static_cast<std::uint64_t>(y) * mip.pitchBytes + x * 4u, 4u);
            const auto wanted = Expected(which, x, y);
            if (actual == wanted) continue;
            if (wrong++ == 0) first = "texel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + Hex(actual[0]) + "/" + Hex(actual[1]) + ", expected " + Hex(wanted[0]) + "/" + Hex(wanted[1]);
        }
    }
    Require(wrong == 0, std::string(stage) + " image_store of " + which.name + ": " + std::to_string(wrong) + " of " + std::to_string(Width * Height) + " texels wrong, first " + first);
}

std::uint32_t StorageBinding(const ShaderRecompiler::RecompileResult& result) {
    for (const auto& binding : result.bindings) {
        if (binding.kind == ShaderRecompiler::DescriptorKind::StorageImage && ShaderResources::UintStorageView(binding.binding, true)) return binding.binding;
    }
    Require(false, "stage local uint storage: the shader has no uint storage image binding");
    return 0;
}

std::string Layout(const ShaderRecompiler::RecompileResult& result) {
    std::string text;
    for (const auto& binding : result.bindings) text += " " + std::to_string(binding.descriptorSet) + ":" + std::to_string(binding.binding) + ":" + std::to_string(static_cast<int>(binding.kind)) + "x" + std::to_string(binding.count) + "/" + std::to_string(binding.guestDescriptor.size()) + "w" + std::to_string(std::count(binding.imageWritten.begin(), binding.imageWritten.end(), true));
    return text;
}

std::string IdentityText(const ShaderRecompiler::RecompileResult& result) {
    const auto identity = result.Identity();
    char text[40];
    std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(identity.high), static_cast<unsigned long long>(identity.low));
    return text;
}

void Decision() {
    constexpr auto count = static_cast<std::uint32_t>(ShaderRecompiler::RuntimeAbi::Binding::Count);
    constexpr auto first = ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding + 7u;
    constexpr auto last = ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding + 14u;
    for (std::uint32_t local = 0; local < count; ++local) {
        const bool uintLocal = local >= first && local < last;
        Require(ShaderResources::UintStorageView(local, false) == uintLocal && ShaderResources::UintStorageView(local, true) == uintLocal, "decision: binding group 0 (compute, vertex) local " + std::to_string(local) + " changed with the switch");
        for (std::uint32_t group = 1; group < ShaderRecompiler::RuntimeAbi::StageCount; ++group) {
            const auto global = group * count + local;
            Require(!ShaderResources::UintStorageView(global, false), "decision: the global test picked a uint view for group " + std::to_string(group) + " binding " + std::to_string(global));
            Require(ShaderResources::UintStorageView(global, true) == uintLocal, "decision: the stage-local test differs from group 0 for group " + std::to_string(group) + " binding " + std::to_string(global));
        }
    }
    std::puts("decision: group 0 identical both ways; groups 1-3 uint only with the stage-local test");
}

void Compute(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Case& which) {
    Dispatch(device, texels, which);
    CheckTexels(texels, which, "compute");
    std::printf("compute %s: passed, %u texels\n", which.name, Width * Height);
}

void Pixel(AgcDriver::VulkanDevice& device, std::uint8_t* texels, const Case& which) {
    Draw(device, texels, which);
    std::printf("pixel %s: switch %s\n", which.name, SwitchOn() ? "on" : "off");
    CheckTexels(texels, which, "pixel shader");
    std::printf("pixel %s: passed, %u texels\n", which.name, Width * Height);
}

void ForceSwitch(const char* value) {
#ifdef _WIN32
    _putenv_s("APS5_TMP_STAGE_LOCAL_UINT_STORAGE", value != nullptr ? value : "");
#else
    if (value != nullptr) setenv("APS5_TMP_STAGE_LOCAL_UINT_STORAGE", value, 1);
    else unsetenv("APS5_TMP_STAGE_LOCAL_UINT_STORAGE");
#endif
}

struct Shape {
    std::string identity;
    std::string layout;
    std::uint32_t binding;
};

Shape ShapeOf(const ShaderRecompiler::RecompileResult& result) {
    return {IdentityText(result), Layout(result), StorageBinding(result)};
}

void Identity(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    const char* original = std::getenv("APS5_TMP_STAGE_LOCAL_UINT_STORAGE");
    const std::string kept = original != nullptr ? original : "";
    for (const auto* which : {&Float16, &Sint16}) {
        ForceSwitch(nullptr);
        const auto computeOff = ShapeOf(CompileCompute(device, texels, *which));
        const auto pixelOff = ShapeOf(CompileGraphics(device, texels, *which).pixel);
        ForceSwitch("1");
        const auto computeOn = ShapeOf(CompileCompute(device, texels, *which));
        const auto pixelOn = ShapeOf(CompileGraphics(device, texels, *which).pixel);
        Require(computeOff.identity == computeOn.identity && computeOff.layout == computeOn.layout && computeOff.binding == computeOn.binding, std::string("identity: compute ") + which->name + " SPIR-V or layout changed with the switch");
        Require(pixelOff.identity == pixelOn.identity && pixelOff.layout == pixelOn.layout && pixelOff.binding == pixelOn.binding, std::string("identity: pixel ") + which->name + " SPIR-V or layout changed with the switch");
        Require(ShaderResources::UintStorageView(computeOff.binding, false) && ShaderResources::UintStorageView(computeOff.binding, true), std::string("identity: compute ") + which->name + " does not get a uint view both ways");
        Require(!ShaderResources::UintStorageView(pixelOff.binding, false) && ShaderResources::UintStorageView(pixelOff.binding, true), std::string("identity: pixel ") + which->name + " does not get its uint view only from the stage-local test");
        std::printf("identity compute %s: spirv+layout %s layout%s binding %u, uint view off 1 on 1\n", which->name, computeOff.identity.c_str(), computeOff.layout.c_str(), computeOff.binding);
        std::printf("identity pixel %s: spirv+layout %s layout%s binding %u, uint view off 0 on 1\n", which->name, pixelOff.identity.c_str(), pixelOff.layout.c_str(), pixelOff.binding);
    }
    ForceSwitch(original != nullptr ? kept.c_str() : nullptr);
}

}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "all";
        std::printf("APS5_TMP_STAGE_LOCAL_UINT_STORAGE %s\n", SwitchOn() ? "on" : "off");
        if (mode == "decision") {
            Decision();
            return 0;
        }
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock block;
        auto* texels = block.Data();
        if (mode == "all") Decision();
        if (mode == "all" || mode == "identity") Identity(*device, texels);
        if (mode == "all" || mode == "compute") {
            Compute(*device, texels, Float16);
            Compute(*device, texels, Sint16);
        }
        const bool pixel = mode == "float16" || mode == "sint" || (mode == "all" && SwitchOn());
        if (mode == "all" && !SwitchOn()) std::puts("pixel stores: skipped with the switch off, their result is undefined there");
        if (pixel && mode != "sint") Pixel(*device, texels, Float16);
        if (pixel && mode != "float16") Pixel(*device, texels, Sint16);
        std::puts("stage local uint storage tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
