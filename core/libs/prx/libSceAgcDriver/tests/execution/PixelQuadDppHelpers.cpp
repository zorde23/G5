#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 64;
constexpr std::uint32_t Cell = 8;
constexpr float Unset = -7.0f;
constexpr float Step = 2.0f / Width;
alignas(256) std::array<float, Width * Height * 4> Pixels{};

alignas(256) constexpr std::array<std::uint32_t, 8> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf800020f, 0x03020100, 0xf80008cf, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 13> PixelCode{
    0xbe84047e, 0xbefe0a7e, 0xc8080000, 0xc8090001, 0x7e0602fa, 0xff00b102, 0x08080503,
    0x360808ff, 0x7fffffff, 0xbefe0404, 0xf800180f, 0x04040404, 0xbf810000,
};

std::vector<std::array<float, 4>> Triangles() {
    constexpr std::array<std::array<std::array<float, 2>, 3>, 4> shapes{{
        {{{1.3f, 1.2f}, {6.9f, 2.4f}, {2.7f, 6.8f}}},
        {{{6.6f, 1.1f}, {5.2f, 6.7f}, {1.1f, 3.3f}}},
        {{{1.0f, 6.9f}, {3.9f, 1.4f}, {6.95f, 6.1f}}},
        {{{2.2f, 1.6f}, {6.8f, 4.1f}, {1.6f, 5.4f}}},
    }};
    std::vector<std::array<float, 4>> vertices;
    for (std::uint32_t cy = 0; cy < Height / Cell; ++cy) {
        for (std::uint32_t cx = 0; cx < Width / Cell; ++cx) {
            for (const auto& corner : shapes[(cx + cy * 3u) % shapes.size()]) {
                const float px = static_cast<float>(cx * Cell) + corner[0];
                const float py = static_cast<float>(cy * Cell) + corner[1];
                vertices.push_back({px / Width * 2.0f - 1.0f, 1.0f - py / Height * 2.0f, 0.5f, 1.0f});
            }
        }
    }
    return vertices;
}

bool Covered(std::uint32_t x, std::uint32_t y) {
    return Pixels[(y * Width + x) * 4u] != Unset;
}

std::array<std::uint32_t, 3> SpirvCounts(const ShaderRecompiler::RecompileResult& result) {
    const auto spirv = result.spirv.Words();
    std::array<std::uint32_t, 3> counts{};
    for (std::size_t cursor = 5; cursor < spirv.size();) {
        const auto length = spirv[cursor] >> 16u;
        Require(length != 0 && length <= spirv.size() - cursor, "pixel quad DPP: truncated SPIR-V instruction");
        const auto op = spirv[cursor] & 0xffffu;
        if (op == spv::OpGroupNonUniformQuadBroadcast) ++counts[0];
        if (op == spv::OpGroupNonUniformShuffle) ++counts[1];
        if (op == spv::OpGroupNonUniformBallot) ++counts[2];
        cursor += length;
    }
    return counts;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        auto target = device->Target();
        target.fragmentShaderBarycentricEnabled = false;
        const bool switched = std::getenv("APS5_TMP_DPP_QUAD_OPS") != nullptr;
        if (switched && !TargetHasCapability(target, spv::CapabilityGroupNonUniformQuad)) {
            std::puts("skipped, the device has no fragment quad operations");
            return VulkanTestSkipped;
        }
        static const auto triangles = Triangles();
        const auto address = reinterpret_cast<std::uintptr_t>(triangles.data());
        const std::array<std::uint32_t, 4> vertexUserData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(triangles.size()), 0x01016facu};
        const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
        ShaderRecompiler::RecompileRequest vertex{
            {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
            {64u, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
            target, {0, 0, 0, 64}
        };
        vertex.useCache = false;
        const auto vertexResult = ShaderRecompiler::Recompile(vertex);
        const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
        ShaderRecompiler::ShaderPixelStageInfo pixel{};
        pixel.interpolatorCount = 1;
        pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PerspectiveCenter);
        pixel.hasPerspectiveCenterVgpr = true;
        pixel.targetOutputMode[0] = 9;
        pixel.targetExportMapping.fill(0xe4u);
        const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
        ShaderRecompiler::RecompileRequest fragment{
            {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
            {64u, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory}, target, PixelPushLayout(vertexPush, target)
        };
        fragment.useCache = false;
        const auto pixelResult = ShaderRecompiler::Recompile(fragment);
        const auto counts = SpirvCounts(pixelResult);
        std::printf("switch %s: %u QuadBroadcast, %u Shuffle, %u Ballot in the pixel shader\n", switched ? "on" : "off", counts[0], counts[1], counts[2]);
        if (switched) Require(counts[0] != 0u && counts[1] == 0u, "pixel quad DPP: the switched quad_perm did not become QuadBroadcast");
        else Require(counts[0] == 0u && counts[1] != 0u, "pixel quad DPP: the quad_perm used QuadBroadcast without the switch");
        if (!switched) {
            std::puts("texels: skipped with the switch off, helper lanes in a Shuffle are undefined there");
            std::puts("pixel quad DPP helper tests passed");
            return 0;
        }

        AgcDriver::Graphics::State state{};
        state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
        state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(Pixels), 0xe4u, AgcDriver::Graphics::ColorTileMode::Linear, 16u};
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
        Pixels.fill(Unset);
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &vertexResult, 0}, {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}}};
        device->Draw(state, {0, static_cast<std::uint32_t>(triangles.size()), 0, 1, 0, false}, shaders);
        device->WaitIdle();

        std::uint32_t edge = 0, interior = 0, wrongEdge = 0, wrongInterior = 0;
        std::string first;
        for (std::uint32_t y = 0; y < Height; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) {
                if (!Covered(x, y)) continue;
                const auto qx = x & ~1u, qy = y & ~1u;
                const bool full = Covered(qx, qy) && Covered(qx + 1u, qy) && Covered(qx, qy + 1u) && Covered(qx + 1u, qy + 1u);
                const auto value = Pixels[(y * Width + x) * 4u];
                const bool wrong = std::fabs(value - Step) > 1e-4f;
                (full ? interior : edge) += 1u;
                (full ? wrongInterior : wrongEdge) += wrong ? 1u : 0u;
                if (wrong && first.empty()) first = "(" + std::to_string(x) + ", " + std::to_string(y) + ") " + (full ? "interior" : "edge") + " holds " + std::to_string(value);
            }
        }
        std::printf("switch %s: %u edge pixels (%u wrong), %u interior pixels (%u wrong), expected %f\n", switched ? "on" : "off", edge, wrongEdge, interior, wrongInterior, Step);
        Require(edge > 200u && interior > 50u, "pixel quad DPP: the triangles covered too few edge or interior quads");
        Require(wrongEdge == 0u && wrongInterior == 0u, "pixel quad DPP: a quad_perm neighbour read in WQM is wrong at " + first);
        std::puts("pixel quad DPP helper tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
