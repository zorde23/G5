#include <codegen/x86/RedZoneFramer.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint64_t Base = 0x1000;

std::vector<std::uint8_t> padTo(std::vector<std::uint8_t> bytes, const std::size_t size) {
    bytes.resize(size, 0xcc);
    return bytes;
}

void append(std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& more) {
    bytes.insert(bytes.end(), more.begin(), more.end());
}

std::vector<std::uint8_t> callTo(const std::size_t from, const std::size_t target) {
    const auto displacement = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(from + 5);
    return {0xe8, static_cast<std::uint8_t>(displacement), static_cast<std::uint8_t>(displacement >> 8), static_cast<std::uint8_t>(displacement >> 16), static_cast<std::uint8_t>(displacement >> 24)};
}

const std::vector<std::uint8_t> Prologue{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x53};
const std::vector<std::uint8_t> Exit{0x5b, 0x41, 0x5f, 0x5d, 0xc3};
const std::vector<std::uint8_t> StoreBelowPushes{0x48, 0x89, 0x7d, 0xe8};
const std::vector<std::uint8_t> LoadBelowPushes{0x48, 0x8b, 0x45, 0xe8};
const std::vector<std::uint8_t> LoadAbovePushes{0x48, 0x8b, 0x45, 0x08};

std::vector<std::uint8_t> leaf(const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> bytes = Prologue;
    append(bytes, body);
    append(bytes, Exit);
    return bytes;
}

struct Image {
    std::vector<std::uint8_t> bytes;
    std::vector<Domain::ProgramHeader> segments;
    std::vector<Codegen::RedZoneFunction> functions;
};

Image image(const std::vector<std::vector<std::uint8_t>>& functions) {
    Image result;
    std::vector<std::size_t> starts;
    std::size_t at = 0x40;
    for (const auto& function : functions) {
        starts.push_back(at);
        at += (function.size() + 15) / 16 * 16 + 16;
    }
    std::vector<std::uint8_t> caller;
    for (const auto start : starts) append(caller, callTo(caller.size(), start));
    caller.push_back(0xc3);
    result.bytes = padTo(caller, 0x40);
    for (std::size_t index = 0; index < functions.size(); ++index) {
        result.bytes = padTo(result.bytes, starts[index]);
        append(result.bytes, functions[index]);
        result.functions.push_back({Base + starts[index], Base + starts[index] + functions[index].size()});
    }
    result.bytes = padTo(result.bytes, (result.bytes.size() + 15) / 16 * 16 + 16);
    result.segments.push_back({1, 5, 0, Base, Base, result.bytes.size(), result.bytes.size(), 16});
    return result;
}

void framesRedZoneLeaf() {
    std::vector<std::uint8_t> body = StoreBelowPushes;
    append(body, LoadBelowPushes);
    const auto input = image({leaf(body)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.RedZoneFunctions == 1 && report.Framed == 1 && report.Exits == 1, "a red-zone leaf was not framed");
    require(sites.size() == 2, "a framed leaf needs one prologue and one exit site");
    require(displacements.empty() && report.FramedWithoutFramePointer == 0, "a leaf with a frame pointer had rsp operands moved");

    const auto& entry = sites[0];
    require(entry.Address == Base + 0x40 && entry.Length == Prologue.size(), "the prologue site does not cover the pushes");
    require(entry.OriginalBytes == Prologue, "the prologue site lost its bytes");
    std::vector<std::uint8_t> entryBody = Prologue;
    append(entryBody, {0x48, 0x8d, 0x64, 0x24, 0x80, 0xe9, 0, 0, 0, 0});
    require(entry.Body == entryBody && entry.ReturnBranchOffset == Prologue.size() + 5, "the prologue stub does not lower rsp after the pushes");

    const auto& exit = sites[1];
    require(exit.Address == Base + 0x40 + Prologue.size() + body.size() && exit.Length == Exit.size(), "the exit site does not cover the pops and ret");
    std::vector<std::uint8_t> exitBody{0x48, 0x8d, 0xa4, 0x24, 0x80, 0, 0, 0};
    append(exitBody, Exit);
    append(exitBody, {0xe9, 0, 0, 0, 0});
    require(exit.Body == exitBody && exit.ReturnBranchOffset == 8 + Exit.size(), "the exit stub does not raise rsp before the pops and leave the ret the early je reaches");
}

const std::vector<std::uint8_t> FramelessPrologue{0x53, 0x41, 0x56, 0x41, 0x57};
const std::vector<std::uint8_t> FramelessExit{0x41, 0x5f, 0x41, 0x5e, 0x5b, 0xc3};
const std::vector<std::uint8_t> StoreBelowRsp{0x48, 0x89, 0x7c, 0x24, 0xf0};
const std::vector<std::uint8_t> LoadBelowRsp{0x48, 0x8b, 0x44, 0x24, 0xf0};
const std::vector<std::uint8_t> LoadAboveRsp{0x48, 0x8b, 0x44, 0x24, 0x08};
const std::vector<std::uint8_t> LoadFarAboveRsp{0x48, 0x8b, 0x44, 0x24, 0x78};
const std::vector<std::uint8_t> LoadAtRsp{0x48, 0x8b, 0x04, 0x24};
const std::vector<std::uint8_t> StoreDeepBelowRsp{0x48, 0x89, 0x74, 0x24, 0xd8};
const std::vector<std::uint8_t> LoadDeepBelowRsp{0x48, 0x8b, 0x44, 0x24, 0xd8};
const std::vector<std::uint8_t> AddressBelowRsp{0x48, 0x8d, 0x44, 0x24, 0xe0};
const std::vector<std::uint8_t> AddressAboveRsp{0x48, 0x8d, 0x44, 0x24, 0x08};
const std::vector<std::uint8_t> StoreIndexedBelowRsp{0x48, 0x89, 0x7c, 0xcc, 0xf0};
const std::vector<std::uint8_t> StoreImmediateFarAboveRsp{0xc7, 0x44, 0x24, 0x7c, 0x78, 0x56, 0x34, 0x12};
const std::vector<std::uint8_t> LoadIndexedAtRsp{0x48, 0x8b, 0x04, 0xcc};
const std::vector<std::uint8_t> AddRdxToRax{0x48, 0x01, 0xd0};
const std::vector<std::uint8_t> VexStoreAtRsp{0xc5, 0xf8, 0x11, 0x04, 0x24};
const std::vector<std::uint8_t> EvexStoreBelowRsp{0x62, 0xf1, 0x7c, 0x48, 0x11, 0x44, 0x24, 0xff};
const std::vector<std::uint8_t> LoadRipRelative{0x48, 0x8b, 0x05, 0x00, 0x00, 0x00, 0x00};

std::vector<std::uint8_t> stubBody(std::vector<std::uint8_t> code) {
    append(code, {0xe9, 0, 0, 0, 0});
    return code;
}

std::vector<std::uint8_t> framelessLeaf(const std::vector<std::uint8_t>& prologue, const std::vector<std::uint8_t>& body, const std::vector<std::uint8_t>& exit) {
    std::vector<std::uint8_t> bytes = prologue;
    append(bytes, body);
    append(bytes, exit);
    return bytes;
}

void framesLeafWithoutFramePointer() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, LoadBelowRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, body, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.RedZoneFunctions == 1 && report.Framed == 1 && report.FramedWithoutFramePointer == 1 && report.MovedOperands == 2, "a leaf that only pushes was not framed");
    require(sites.size() == 2, "a framed leaf needs one prologue and one exit site");
    const auto& entry = sites[0];
    require(entry.Address == Base + 0x40 && entry.Length == FramelessPrologue.size(), "the prologue site does not cover the pushes");
    std::vector<std::uint8_t> entryBody = FramelessPrologue;
    append(entryBody, {0x48, 0x8d, 0x64, 0x24, 0xf0, 0xe9, 0, 0, 0, 0});
    require(entry.Body == entryBody, "the prologue stub does not lower rsp by the leaf's depth below rsp after the pushes");
    const auto& exit = sites[1];
    require(exit.Address == Base + 0x40 + FramelessPrologue.size() + body.size() && exit.OriginalBytes == FramelessExit, "the exit site does not cover the pops and ret");
    std::vector<std::uint8_t> exitBody{0x48, 0x8d, 0xa4, 0x24, 0x10, 0, 0, 0};
    append(exitBody, FramelessExit);
    append(exitBody, {0xe9, 0, 0, 0, 0});
    require(exit.Body == exitBody, "the exit stub does not raise rsp by the leaf's depth below rsp before the pops");
    const auto storeDisplacement = 0x40 + FramelessPrologue.size() + 4;
    const auto loadDisplacement = storeDisplacement + StoreBelowRsp.size();
    require(displacements.size() == 2, "both rsp operands must move");
    require(displacements[0].Offset == storeDisplacement && displacements[0].Bytes == std::vector<std::uint8_t>{0x00}, "the store below rsp was not moved to the lowered rsp");
    require(displacements[1].Offset == loadDisplacement && displacements[1].Bytes == std::vector<std::uint8_t>{0x00}, "the load below rsp was not moved to the lowered rsp");
}

void shiftsByTheDeepestOperandBelowRsp() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, StoreDeepBelowRsp);
    append(body, LoadDeepBelowRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, body, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && sites.size() == 2 && displacements.size() == 3, "a leaf reaching 0x28 bytes below rsp was not framed");
    std::vector<std::uint8_t> entryBody = FramelessPrologue;
    append(entryBody, {0x48, 0x8d, 0x64, 0x24, 0xd0, 0xe9, 0, 0, 0, 0});
    require(sites[0].Body == entryBody, "rsp is not lowered by the deepest displacement rounded up to 16");
    require(std::vector<std::uint8_t>(sites[1].Body.begin(), sites[1].Body.begin() + 8) == std::vector<std::uint8_t>{0x48, 0x8d, 0xa4, 0x24, 0x30, 0, 0, 0}, "rsp is not raised by the shift it was lowered by");
    require(displacements[0].Bytes == std::vector<std::uint8_t>{0x20} && displacements[1].Bytes == std::vector<std::uint8_t>{0x08} && displacements[2].Bytes == std::vector<std::uint8_t>{0x08}, "the operands below rsp were not moved by the shift");
}

void movesPositiveOperandsThatFitAfterTheShift() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, LoadAboveRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, body, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.MovedOperands == 2 && sites.size() == 2, "a leaf with an 8-bit displacement above rsp that fits after the shift was not framed");
    const auto loadDisplacement = 0x40 + FramelessPrologue.size() + StoreBelowRsp.size() + 4;
    require(displacements.size() == 2 && displacements[1].Offset == loadDisplacement && displacements[1].Bytes == std::vector<std::uint8_t>{0x18}, "the load above rsp was not moved in place by the shift");
}

void shiftsByTheWholeRedZoneForAddressesBelowRsp() {
    std::vector<std::uint8_t> below = StoreBelowRsp;
    append(below, AddressBelowRsp);
    std::vector<std::uint8_t> above = StoreBelowRsp;
    append(above, AddressAboveRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, below, FramelessExit), framelessLeaf(FramelessPrologue, above, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 2 && sites.size() == 4 && displacements.size() == 4, "leaves that take the address of a stack slot were not framed");
    require(sites[0].Body[FramelessPrologue.size() + 4] == 0x80 && displacements[0].Bytes == std::vector<std::uint8_t>{0x70} && displacements[1].Bytes == std::vector<std::uint8_t>{0x60}, "a leaf that takes the address of a slot below rsp was not shifted by the whole red zone");
    require(sites[2].Body[FramelessPrologue.size() + 4] == 0xf0 && displacements[2].Bytes == std::vector<std::uint8_t>{0x00} && displacements[3].Bytes == std::vector<std::uint8_t>{0x18}, "a leaf that only takes the address of a slot above rsp was shifted by more than its depth");
}

void shiftsByTheWholeRedZoneForIndexedOperands() {
    std::vector<std::uint8_t> body = StoreIndexedBelowRsp;
    append(body, LoadBelowRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, body, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && sites.size() == 2 && displacements.size() == 2, "a leaf with an indexed operand below rsp was not framed");
    require(sites[0].Body[FramelessPrologue.size() + 4] == 0x80 && displacements[0].Bytes == std::vector<std::uint8_t>{0x70} && displacements[1].Bytes == std::vector<std::uint8_t>{0x70}, "a leaf whose indexed operand can reach deeper than its displacement was not shifted by the whole red zone");
}

void stubsOperandsWhoseDisplacementOverflows() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, LoadFarAboveRsp);
    append(body, StoreImmediateFarAboveRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, body, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.MovedOperands == 1 && report.OperandStubs == 2 && report.StubbedOperands == 2 && sites.size() == 4, "a leaf whose 8-bit displacements above rsp overflow after the shift was not framed with operand stubs");
    const auto load = Base + 0x40 + FramelessPrologue.size() + StoreBelowRsp.size();
    require(sites[2].Address == load && sites[2].Length == LoadFarAboveRsp.size() && sites[2].OriginalBytes == LoadFarAboveRsp, "the stub site does not cover the load that cannot move in place");
    require(sites[2].Body == stubBody({0x48, 0x8b, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00}) && sites[2].ReturnBranchOffset == 8, "the load was not re-encoded with a 32-bit displacement moved by the shift");
    require(sites[3].Address == load + LoadFarAboveRsp.size() && sites[3].OriginalBytes == StoreImmediateFarAboveRsp, "the stub site does not cover the store of an immediate that cannot move in place");
    require(sites[3].Body == stubBody({0xc7, 0x84, 0x24, 0x8c, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12}), "the store lost its immediate when its displacement was widened");
    require(displacements.size() == 1 && displacements[0].Bytes == std::vector<std::uint8_t>{0x00}, "the store below rsp was not moved in place");
}

void stubsOperandsAtRspWithTheInstructionsAfterThem() {
    std::vector<std::uint8_t> atRsp = StoreBelowRsp;
    append(atRsp, LoadAtRsp);
    append(atRsp, LoadAboveRsp);
    std::vector<std::uint8_t> indexed = StoreBelowRsp;
    append(indexed, LoadIndexedAtRsp);
    append(indexed, AddRdxToRax);
    const auto input = image({framelessLeaf(FramelessPrologue, atRsp, FramelessExit), framelessLeaf(FramelessPrologue, indexed, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 2 && report.MovedOperands == 2 && report.OperandStubs == 2 && report.StubbedOperands == 3 && sites.size() == 6, "leaves with operands at rsp were not framed with operand stubs");
    const auto first = 0x40 + FramelessPrologue.size() + StoreBelowRsp.size();
    require(sites[2].Address == Base + first && sites[2].Length == LoadAtRsp.size() + LoadAboveRsp.size(), "the stub site does not run on to the next instruction to make room for its jump");
    require(sites[2].Body == stubBody({0x48, 0x8b, 0x44, 0x24, 0x10, 0x48, 0x8b, 0x44, 0x24, 0x18}), "the load at rsp and the load the stub covers after it were not both moved by the shift");
    for (const auto& displacement : displacements) require(displacement.Offset < first || displacement.Offset >= first + sites[2].Length, "an rsp operand inside a stub site was also moved in place");
    require(sites[3].Body[FramelessPrologue.size() + 4] == 0x80, "a leaf with an indexed operand at rsp was not shifted by the whole red zone");
    require(sites[5].OriginalBytes.size() == LoadIndexedAtRsp.size() + AddRdxToRax.size() && sites[5].Body == stubBody({0x48, 0x8b, 0x84, 0xcc, 0x80, 0x00, 0x00, 0x00, 0x48, 0x01, 0xd0}), "the indexed load at rsp was not re-encoded with a 32-bit displacement of 0x80 ahead of the add it covers");
}

void stubsVexOperandsAtRsp() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, VexStoreAtRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, body, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.OperandStubs == 1 && report.StubbedOperands == 1 && sites.size() == 3, "a leaf with a VEX store at rsp was not framed with an operand stub");
    require(sites[2].OriginalBytes == VexStoreAtRsp && sites[2].Body == stubBody({0xc5, 0xf8, 0x11, 0x44, 0x24, 0x10}), "the VEX store at rsp was not re-encoded with an 8-bit displacement moved by the shift");
}

void stubsOperandsAtRspWithTheInstructionsBeforeThem() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, LoadAtRsp);
    append(body, LoadRipRelative);
    const auto input = image({framelessLeaf(FramelessPrologue, body, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.MovedOperands == 0 && report.OperandStubs == 1 && report.StubbedOperands == 2 && sites.size() == 3 && displacements.empty(), "a leaf whose short operand at rsp sits before a rip-relative instruction was not framed with a stub over the instruction before it");
    const auto store = Base + 0x40 + FramelessPrologue.size();
    require(sites[2].Address == store && sites[2].Length == StoreBelowRsp.size() + LoadAtRsp.size(), "the stub site does not start at the store before the operand at rsp");
    require(sites[2].Body == stubBody({0x48, 0x89, 0x7c, 0x24, 0x00, 0x48, 0x8b, 0x44, 0x24, 0x10}), "the store before the operand and the operand at rsp were not both moved by the shift");
}

void skipsLeavesWithOperandsThatCannotMove() {
    std::vector<std::uint8_t> evex = StoreBelowRsp;
    append(evex, EvexStoreBelowRsp);
    std::vector<std::uint8_t> beforeExit = StoreBelowRsp;
    append(beforeExit, LoadAtRsp);
    std::vector<std::uint8_t> beforeTarget = StoreBelowRsp;
    append(beforeTarget, LoadAtRsp);
    append(beforeTarget, {0xff, 0xc0, 0x3b, 0x44, 0x24, 0xf0, 0x72, 0xf8});
    std::vector<std::uint8_t> afterShortPrologue = LoadRipRelative;
    append(afterShortPrologue, StoreBelowRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, evex, FramelessExit), framelessLeaf(FramelessPrologue, beforeExit, FramelessExit), framelessLeaf(FramelessPrologue, beforeTarget, FramelessExit), framelessLeaf({0x41, 0x56, 0x41, 0x57}, afterShortPrologue, {0x41, 0x5f, 0x41, 0x5e, 0xc3})});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(sites.empty() && displacements.empty() && report.RedZoneFunctions == 4, "a leaf whose rsp operands can neither move nor be stubbed, or whose prologue is too short, was framed");
    require(report.Skipped.at("rsp operand that cannot move with rsp") == 1, "an EVEX operand, whose 8-bit displacement is scaled, was not refused");
    require(report.Skipped.at("rsp operand that cannot move and has no room for a stub") == 2, "an operand at rsp followed by an exit or a branch target was not refused for want of room");
    require(report.Skipped.at("prologue shorter than a jump") == 1, "the leaf whose short prologue is followed by a rip-relative instruction was not reported with its reason");
}

const std::vector<std::uint8_t> PushRbx{0x53};
const std::vector<std::uint8_t> PopRbxExit{0x5b, 0xc3};
const std::vector<std::uint8_t> Ret{0xc3};

void widensShortProloguesAndExits() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, LoadBelowRsp);
    append(body, AddRdxToRax);
    const auto input = image({framelessLeaf(PushRbx, body, PopRbxExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.WidenedPrologues == 1 && report.WidenedExits == 1 && report.WidenedOperands == 1 && report.MovedOperands == 1 && sites.size() == 2 && report.Skipped.empty(), "a leaf with a 1-byte push and a 2-byte exit was not framed by widening both sites");
    const auto& entry = sites[0];
    require(entry.Address == Base + 0x40 && entry.Length == PushRbx.size() + StoreBelowRsp.size(), "the prologue site does not run on over the store after the push");
    require(entry.Body == stubBody({0x53, 0x48, 0x8d, 0x64, 0x24, 0xf0, 0x48, 0x89, 0x7c, 0x24, 0x00}) && entry.ReturnBranchOffset == 11, "the store after the push was not moved by the shift after rsp is lowered");
    const auto& exit = sites[1];
    const auto add = Base + 0x40 + PushRbx.size() + StoreBelowRsp.size() + LoadBelowRsp.size();
    require(exit.Address == add && exit.Length == AddRdxToRax.size() + PopRbxExit.size(), "the exit site does not start at the add before the pop");
    std::vector<std::uint8_t> exitBody = AddRdxToRax;
    append(exitBody, {0x48, 0x8d, 0xa4, 0x24, 0x10, 0x00, 0x00, 0x00});
    append(exitBody, PopRbxExit);
    require(exit.Body == stubBody(exitBody), "the add was not copied ahead of raising rsp and the pop");
    require(displacements.size() == 1 && displacements[0].Offset == 0x40 + PushRbx.size() + StoreBelowRsp.size() + 4 && displacements[0].Bytes == std::vector<std::uint8_t>{0x00}, "the load between the widened sites was not moved in place");
}

void widensAShortFramePointerPrologue() {
    const std::vector<std::uint8_t> framePrologue{0x55, 0x48, 0x89, 0xe5};
    const std::vector<std::uint8_t> popRbpExit{0x5d, 0xc3};
    std::vector<std::uint8_t> body = StoreBelowPushes;
    append(body, LoadBelowPushes);
    const auto input = image({framelessLeaf(framePrologue, body, popRbpExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.FramedWithoutFramePointer == 0 && report.WidenedPrologues == 1 && report.WidenedExits == 1 && report.WidenedOperands == 0 && sites.size() == 2 && displacements.empty(), "a frame-pointer leaf without other pushes was not framed by widening its 4-byte prologue and 2-byte exit");
    std::vector<std::uint8_t> entryBody = framePrologue;
    append(entryBody, {0x48, 0x8d, 0x64, 0x24, 0x80});
    append(entryBody, StoreBelowPushes);
    require(sites[0].OriginalBytes.size() == framePrologue.size() + StoreBelowPushes.size() && sites[0].Body == stubBody(entryBody), "the store through rbp was not copied unchanged after rsp is lowered by the whole red zone");
    std::vector<std::uint8_t> exitBody = LoadBelowPushes;
    append(exitBody, {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00});
    append(exitBody, popRbpExit);
    require(sites[1].OriginalBytes.size() == LoadBelowPushes.size() + popRbpExit.size() && sites[1].Body == stubBody(exitBody), "the load through rbp was not copied unchanged ahead of raising rsp and popping rbp");
}

void framesLeavesWithoutPushes() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, LoadBelowRsp);
    const auto input = image({framelessLeaf({}, body, Ret)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.WidenedPrologues == 1 && report.WidenedExits == 1 && report.WidenedOperands == 2 && sites.size() == 2 && displacements.empty(), "a leaf that only spills below rsp and returns was not framed");
    require(sites[0].Address == Base + 0x40 && sites[0].OriginalBytes == StoreBelowRsp, "the entry site of a leaf without pushes does not cover its first instruction");
    require(sites[0].Body == stubBody({0x48, 0x8d, 0x64, 0x24, 0xf0, 0x48, 0x89, 0x7c, 0x24, 0x00}), "the entry stub does not lower rsp before the store moved by the shift");
    std::vector<std::uint8_t> exitBody{0x48, 0x8b, 0x44, 0x24, 0x00, 0x48, 0x8d, 0xa4, 0x24, 0x10, 0x00, 0x00, 0x00};
    append(exitBody, Ret);
    require(sites[1].OriginalBytes.size() == LoadBelowRsp.size() + Ret.size() && sites[1].Body == stubBody(exitBody), "the exit stub does not run the load moved by the shift, raise rsp and return");
}

void skipsFragmentsThatPopWithoutPushing() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, LoadBelowRsp);
    const auto input = image({framelessLeaf({}, body, PopRbxExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(sites.empty() && displacements.empty() && report.Skipped.at("push or pop in a leaf without pushes") == 1, "a fragment without pushes that pops a register its caller pushed was framed");
}

void framesShrinkWrappedLeavesOnlyWhenAsked() {
    const std::vector<std::uint8_t> shrinkWrapped{0x48, 0x85, 0xff, 0x74, 0x14, 0x53, 0x41, 0x56, 0x41, 0x57, 0x48, 0x89, 0x7c, 0x24, 0xf8, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0x41, 0x5f, 0x41, 0x5e, 0x5b, 0xc3};
    const std::vector<std::uint8_t> earlyBranchPastPushes{0x48, 0x85, 0xff, 0x74, 0x0a, 0x53, 0x41, 0x56, 0x41, 0x57, 0x48, 0x89, 0x7c, 0x24, 0xf8, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0x41, 0x5f, 0x41, 0x5e, 0x5b, 0xc3};
    const auto input = image({shrinkWrapped, earlyBranchPastPushes});
    Codegen::RedZoneFrameReport plain;
    std::vector<Codegen::RedZoneDisplacement> plainDisplacements;
    require(Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, plain, plainDisplacements).empty() && plain.Skipped.at("push or pop in a leaf without pushes") == 2, "a leaf pushing after an early exit was framed without being asked");
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements, true);
    require(report.Framed == 1 && report.ShrinkWrapped == 1 && report.Skipped.at("shrink-wrapped: early branch past the push run") == 1, "the shrink-wrapped leaf was not framed or the leaf branching past its pushes was");
    require(sites.size() == 2 && sites[0].Address == input.functions[0].Begin + 5 && sites[1].Address == input.functions[0].Begin + 20, "the shrink-wrapped leaf was not framed at its pushes and its pops");
    std::vector<std::uint8_t> entry{0x53, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x64, 0x24, 0xf0, 0xe9, 0, 0, 0, 0};
    require(sites[0].Body == entry, "the entry stub does not push and then lower rsp");
    std::vector<std::uint8_t> exit{0x48, 0x8d, 0xa4, 0x24, 0x10, 0x00, 0x00, 0x00, 0x41, 0x5f, 0x41, 0x5e, 0x5b, 0xe9, 0, 0, 0, 0};
    require(sites[1].Body == exit, "the exit stub does not raise rsp before the pops and leave the ret the early je reaches");
    require(displacements.size() == 2 && displacements[0].Bytes == std::vector<std::uint8_t>{0x08} && displacements[1].Bytes == std::vector<std::uint8_t>{0x08}, "the spill and load below rsp were not moved by the shift");
}

void skipsShrinkWrappedLeavesWithHiddenRspUsesOrOutsideEntries() {
    const std::vector<std::uint8_t> head{0x48, 0x85, 0xff, 0x74, 0x0f, 0x53, 0x41, 0x56, 0x41, 0x57, 0x48, 0x89, 0x7c, 0x24, 0xf8};
    const std::vector<std::uint8_t> tail{0x41, 0x5f, 0x41, 0x5e, 0x5b, 0xc3};
    const auto shape = [&](const std::vector<std::uint8_t>& middle) {
        auto bytes = head;
        bytes[4] = static_cast<std::uint8_t>(10 + middle.size() + 5);
        append(bytes, middle);
        append(bytes, tail);
        return bytes;
    };
    const auto leaveInBody = shape({0xc9, 0x90, 0x90});
    const auto shortXchg = shape({0x48, 0x94, 0x90});
    const auto movRspImm = shape({0x48, 0xbc, 1, 0, 0, 0, 0, 0, 0, 0});
    const auto imulRsp = shape({0x48, 0x6b, 0x27, 0x02});
    const auto gather = shape({0xc4, 0xe2, 0xe9, 0x90, 0x04, 0x24});
    const auto target = shape({0x90, 0x90, 0x90});
    const std::vector<std::uint8_t> intoBody{0xe9, 0, 0, 0, 0};
    auto input = image({leaveInBody, shortXchg, movRspImm, imulRsp, gather, target, intoBody});
    const auto from = static_cast<std::size_t>(input.functions[6].Begin - Base);
    const auto into = static_cast<std::int32_t>(input.functions[5].Begin - Base + 15) - static_cast<std::int32_t>(from + 5);
    for (std::size_t index = 0; index < 4; ++index) input.bytes[from + 1 + index] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(into) >> (8 * index));
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements, true);
    std::string seen;
    for (const auto& [why, count] : report.Skipped) seen += why + "=" + std::to_string(count) + "; ";
    require(sites.empty() && report.ShrinkWrapped == 0, "a shrink-wrapped leaf with a hidden rsp use or an outside entry was framed: " + seen);
    require(report.Skipped.at("shrink-wrapped: other use of rsp") == 5 && report.Skipped.at("shrink-wrapped: branch from outside into the body") == 1, "the skipped leaves were not listed with their reasons: " + seen);
}

void keepsLeavesThatBranchToAColdPartWhole() {
    const std::vector<std::uint8_t> hot{0x48, 0x89, 0x7c, 0x24, 0xf8, 0x89, 0x06, 0x85, 0xd2, 0x0f, 0x85, 0, 0, 0, 0, 0xc3};
    const std::vector<std::uint8_t> cold{0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3};
    const std::vector<std::uint8_t> alone{0x48, 0x89, 0x7c, 0x24, 0xf8, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0x89, 0x06, 0x85, 0xd2, 0x74, 0x00, 0xc3};
    auto input = image({hot, cold, alone});
    const auto from = static_cast<std::size_t>(input.functions[0].Begin - Base) + 9;
    const auto into = static_cast<std::int32_t>(input.functions[1].Begin - Base) - static_cast<std::int32_t>(from + 6);
    for (std::size_t index = 0; index < 4; ++index) input.bytes[from + 2 + index] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(into) >> (8 * index));
    const auto dead = Codegen::FindRedZoneDeadRanges(input.bytes, input.segments, {input.functions[0], input.functions[2]});
    require(dead[0].empty(), "a leaf whose cold part reads its red zone had instructions called dead");
    require(!dead[1].empty() && dead[1].front().Begin == input.functions[2].Begin + 10, "a leaf that reads its spill before a store was not called dead from the store on");
}

void keepsLeavesWithHiddenReachOrForeignEntriesWhole() {
    const std::vector<std::uint8_t> alone{0x48, 0x89, 0x7c, 0x24, 0xf8, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0x89, 0x06, 0x85, 0xd2, 0x74, 0x00, 0xc3};
    const std::vector<std::uint8_t> bitIndexed{0x48, 0x89, 0x7c, 0x24, 0xf8, 0x48, 0xc7, 0xc1, 0xc0, 0xff, 0xff, 0xff, 0x48, 0x0f, 0xa3, 0x0c, 0x24, 0x0f, 0x92, 0xc0, 0xc3};
    const std::vector<std::uint8_t> lowByteRsp{0x48, 0x89, 0x7c, 0x24, 0xf8, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0x40, 0xb4, 0x00, 0xc3};
    const std::vector<std::uint8_t> setRsp{0x48, 0x89, 0x7c, 0x24, 0xf8, 0x48, 0x8b, 0x44, 0x24, 0xf8, 0x40, 0x0f, 0x94, 0xc4, 0xc3};
    const std::vector<std::uint8_t> jump{0xe9, 0, 0, 0, 0};
    auto input = image({alone, bitIndexed, lowByteRsp, setRsp, alone, jump, alone, jump});
    const auto patch = [&](const std::size_t from, const std::uint64_t to) {
        const auto at = static_cast<std::size_t>(from - Base);
        const auto rel = static_cast<std::int32_t>(to - from - 5);
        for (std::size_t index = 0; index < 4; ++index) input.bytes[at + 1 + index] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(rel) >> (8 * index));
    };
    patch(input.functions[5].Begin, input.functions[4].Begin);
    patch(input.functions[7].Begin, input.functions[6].Begin + 16);
    const auto dead = Codegen::FindRedZoneDeadRanges(input.bytes, input.segments, {input.functions[0], input.functions[1], input.functions[2], input.functions[3], input.functions[4], input.functions[6]});
    require(!dead[0].empty(), "the plain leaf had no dead instructions, so the cases below prove nothing");
    require(dead[1].empty(), "a leaf with a bit test indexed by a register below its RSP operand had instructions called dead");
    require(dead[2].empty(), "a leaf that writes SPL had instructions called dead");
    require(dead[3].empty(), "a leaf that sets SPL had instructions called dead");
    require(dead[4].empty(), "a leaf another function jumps to, which may arrive with a pointer below its RSP, had instructions called dead");
    require(dead[5].empty(), "a leaf whose internally targeted ret is also reached from another function had instructions called dead");
}

void skipsWideningOverBranchTargets() {
    const std::vector<std::uint8_t> loopToStart{0x48, 0x89, 0x7c, 0x24, 0xf0, 0xff, 0xc9, 0x75, 0xf7, 0x48, 0x8b, 0x44, 0x24, 0xf0, 0xc3};
    std::vector<std::uint8_t> jumpToPop = StoreBelowRsp;
    append(jumpToPop, {0x85, 0xff, 0x74, 0x03});
    append(jumpToPop, AddRdxToRax);
    const auto input = image({loopToStart, framelessLeaf(PushRbx, jumpToPop, PopRbxExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(sites.empty() && displacements.empty() && report.RedZoneFunctions == 2, "a leaf with a branch into a widened site was framed");
    require(report.Skipped.at("branch to the start of a widened prologue") == 1, "a leaf without pushes whose loop branches back to its first instruction was not refused");
    require(report.Skipped.at("branch into the instructions before a short exit") == 1, "a short exit that a branch reaches past the add before it was not refused");
}

void skipsBranchesIntoTheInstructionsAfterAShortPrologue() {
    std::vector<std::uint8_t> loopToStore = StoreBelowRsp;
    append(loopToStore, {0xff, 0xc9, 0x75, 0xf7});
    append(loopToStore, LoadBelowRsp);
    const auto input = image({framelessLeaf(PushRbx, loopToStore, PopRbxExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(sites.empty() && displacements.empty() && report.RedZoneFunctions == 1, "a leaf whose loop branches to the store the widened prologue borrows was framed");
    require(report.Skipped.at("branch into the instructions after a short prologue") == 1, "a loop to the instruction after a 1-byte push was not refused with its reason");
}

void widensAShortPrologueThatAllocates() {
    const std::vector<std::uint8_t> allocate{0x48, 0x83, 0xec, 0x08};
    const std::vector<std::uint8_t> freeExit{0x48, 0x83, 0xc4, 0x08, 0xc3};
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, {0x31, 0xc0, 0x48, 0x03, 0x44, 0x24, 0xf0});
    const auto input = image({framelessLeaf(allocate, body, freeExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.FramedWithoutFramePointer == 1 && report.WidenedPrologues == 1 && report.WidenedExits == 0 && report.WidenedOperands == 1 && report.MovedOperands == 1 && sites.size() == 2 && report.Skipped.empty(), "a leaf whose 4-byte sub is its whole prologue was not framed by widening the prologue over the store after it");
    std::vector<std::uint8_t> entryBody = allocate;
    append(entryBody, {0x48, 0x8d, 0x64, 0x24, 0xf0, 0x48, 0x89, 0x7c, 0x24, 0x00});
    require(sites[0].Address == Base + 0x40 && sites[0].OriginalBytes.size() == allocate.size() + StoreBelowRsp.size() && sites[0].Body == stubBody(entryBody), "the store after the sub was not moved by the shift after the sub and the lowering lea");
    std::vector<std::uint8_t> exitBody{0x48, 0x8d, 0xa4, 0x24, 0x10, 0x00, 0x00, 0x00};
    append(exitBody, freeExit);
    require(sites[1].OriginalBytes == freeExit && sites[1].Body == stubBody(exitBody), "rsp is not raised by the shift before the add that frees the allocation");
    require(displacements.size() == 1 && displacements[0].Offset == 0x40 + allocate.size() + StoreBelowRsp.size() + 2 + 4 && displacements[0].Bytes == std::vector<std::uint8_t>{0x00}, "the load between the widened prologue and the exit was not moved in place");
}

void widensBothExitsOfALeafWithoutPushes() {
    const auto twoExits = [](const std::uint8_t branch) {
        std::vector<std::uint8_t> bytes = StoreBelowRsp;
        append(bytes, {0x48, 0x89, 0x74, 0x24, 0xf8, 0x48, 0x39, 0xf7, branch, 0x06});
        append(bytes, LoadBelowRsp);
        append(bytes, Ret);
        append(bytes, {0x48, 0x8b, 0x44, 0x24, 0xf8});
        append(bytes, Ret);
        return bytes;
    };
    const auto input = image({twoExits(0x74), twoExits(0x75)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 2 && report.Exits == 4 && report.WidenedPrologues == 2 && report.WidenedExits == 4 && report.WidenedOperands == 6 && report.MovedOperands == 2 && sites.size() == 6 && report.Skipped.empty(), "leaves without pushes that return from two places were not framed with a widened prologue and two widened exits");
    require(sites[0].Body == stubBody({0x48, 0x8d, 0x64, 0x24, 0xf0, 0x48, 0x89, 0x7c, 0x24, 0x00}), "the entry stub does not lower rsp before the store moved by the shift");
    std::vector<std::uint8_t> firstExit{0x48, 0x8b, 0x44, 0x24, 0x00, 0x48, 0x8d, 0xa4, 0x24, 0x10, 0x00, 0x00, 0x00};
    append(firstExit, Ret);
    std::vector<std::uint8_t> secondExit{0x48, 0x8b, 0x44, 0x24, 0x08, 0x48, 0x8d, 0xa4, 0x24, 0x10, 0x00, 0x00, 0x00};
    append(secondExit, Ret);
    require(sites[1].Address == Base + 0x40 + 15 && sites[1].Body == stubBody(firstExit), "the first ret was not widened over the load before it, moved by the shift");
    require(sites[2].Address == Base + 0x40 + 21 && sites[2].Body == stubBody(secondExit), "the ret a branch reaches at its load was not widened over that load, moved by the shift");
    require(displacements[0].Offset == 0x40 + 9 && displacements[0].Bytes == std::vector<std::uint8_t>{0x08}, "the second spill was not moved in place");
}

void widensExitsOverVexOperandsAtRsp() {
    const std::vector<std::uint8_t> andnBelowRsp{0xc4, 0xe2, 0xf0, 0xf2, 0x44, 0x24, 0xf8};
    std::vector<std::uint8_t> body{0x48, 0x89, 0x7c, 0x24, 0xf8};
    append(body, andnBelowRsp);
    const auto input = image({framelessLeaf({}, body, Ret)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.WidenedPrologues == 1 && report.WidenedExits == 1 && report.WidenedOperands == 2 && sites.size() == 2 && displacements.empty(), "a leaf whose ret follows a VEX andn below rsp was not framed by widening its exit over the andn");
    require(sites[0].Body == stubBody({0x48, 0x8d, 0x64, 0x24, 0xf0, 0x48, 0x89, 0x7c, 0x24, 0x08}), "the spill was not moved by the shift after rsp is lowered");
    std::vector<std::uint8_t> exitBody{0xc4, 0xe2, 0xf0, 0xf2, 0x44, 0x24, 0x08, 0x48, 0x8d, 0xa4, 0x24, 0x10, 0x00, 0x00, 0x00};
    append(exitBody, Ret);
    require(sites[1].OriginalBytes.size() == andnBelowRsp.size() + Ret.size() && sites[1].Body == stubBody(exitBody), "the VEX andn was not re-encoded with its displacement moved by the shift ahead of raising rsp");
}

void framesFramePointerLeavesThatPushInTheirBody() {
    const std::vector<std::uint8_t> framePrologue{0x55, 0x48, 0x89, 0xe5};
    std::vector<std::uint8_t> body = StoreBelowPushes;
    append(body, {0x53, 0x48, 0x89, 0xf3});
    append(body, LoadBelowPushes);
    append(body, {0x48, 0x01, 0xd8, 0x5b, 0x48, 0x01, 0xd0});
    const auto input = image({framelessLeaf(framePrologue, body, {0x5d, 0xc3})});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.FramedWithoutFramePointer == 0 && report.WidenedPrologues == 1 && report.WidenedExits == 1 && report.WidenedOperands == 0 && sites.size() == 2 && displacements.empty() && report.Skipped.empty(), "a frame-pointer leaf that pushes and pops rbx in its body was not framed");
    std::vector<std::uint8_t> entryBody = framePrologue;
    append(entryBody, {0x48, 0x8d, 0x64, 0x24, 0x80});
    append(entryBody, StoreBelowPushes);
    require(sites[0].Body == stubBody(entryBody), "the store through rbp was not copied unchanged after rsp is lowered by the whole red zone");
    require(sites[1].Body == stubBody({0x48, 0x01, 0xd0, 0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00, 0x5d, 0xc3}), "the add before pop rbp was not copied ahead of raising rsp");
}

void skipsLeavesThatCopyRsp() {
    std::vector<std::uint8_t> withCmov = StoreBelowRsp;
    append(withCmov, {0x48, 0x0f, 0x45, 0xc4});
    std::vector<std::uint8_t> withShlx = StoreBelowRsp;
    append(withShlx, {0xc4, 0xe2, 0xf1, 0xf7, 0xc4});
    const auto input = image({framelessLeaf(FramelessPrologue, withCmov, FramelessExit), framelessLeaf(FramelessPrologue, withShlx, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(sites.empty() && displacements.empty() && report.Skipped.at("other use of rsp") == 2, "a leaf that copies rsp into another register was framed");
}

void tellsPopsWithoutRexFromXop() {
    std::vector<std::uint8_t> popBelowRsp = StoreBelowPushes;
    append(popBelowRsp, {0x52, 0x8f, 0x44, 0x24, 0xf0});
    std::vector<std::uint8_t> popRspWithFramePointer = StoreBelowPushes;
    append(popRspWithFramePointer, {0x52, 0x8f, 0xc4});
    std::vector<std::uint8_t> popRsp = StoreBelowRsp;
    append(popRsp, {0x8f, 0xc4});
    const auto pops = image({leaf(popBelowRsp), leaf(popRspWithFramePointer), framelessLeaf(FramelessPrologue, popRsp, FramelessExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(pops.bytes, pops.segments, pops.functions, {}, report, displacements);
    require(sites.empty() && displacements.empty() && report.RedZoneFunctions == 3, "a pop without REX to an rsp operand or into rsp, read as an XOP prefix, was framed");
    require(report.Skipped.at("other use of rsp") == 3, "a pop without REX to an rsp operand or into rsp was not reported as another use of rsp");

    std::vector<std::uint8_t> xopAtR12 = StoreBelowRsp;
    append(xopAtR12, {0x8f, 0xc8, 0x78, 0xc0, 0x04, 0x24, 0x01});
    std::vector<std::uint8_t> xopBelowRsp = StoreBelowRsp;
    append(xopBelowRsp, {0x8f, 0xe8, 0x78, 0xc0, 0x44, 0x24, 0xf0, 0x01});
    const auto xops = image({framelessLeaf(FramelessPrologue, xopAtR12, FramelessExit), framelessLeaf(FramelessPrologue, xopBelowRsp, FramelessExit)});
    Codegen::RedZoneFrameReport xopReport;
    std::vector<Codegen::RedZoneDisplacement> xopDisplacements;
    const auto xopSites = Codegen::FrameRedZoneFunctions(xops.bytes, xops.segments, xops.functions, {}, xopReport, xopDisplacements);
    require(xopReport.Framed == 2 && xopReport.MovedOperands == 3 && xopReport.OperandStubs == 0 && xopSites.size() == 4, "an XOP operand was not read with the base register extension of its XOP prefix");
}

const std::vector<std::uint8_t> InflatePrologue{0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40};
const std::vector<std::uint8_t> InflatePops{0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3};
const std::vector<std::uint8_t> InflateFree{0x48, 0x83, 0xc4, 0x40};

std::vector<std::uint8_t> inflateExit() {
    std::vector<std::uint8_t> bytes = InflateFree;
    append(bytes, InflatePops);
    return bytes;
}

std::vector<std::uint8_t> inflateBody() {
    return {
        0x48, 0x89, 0x7c, 0x24, 0xe0,
        0x48, 0x89, 0x7c, 0x24, 0x00,
        0x48, 0x89, 0x74, 0x24, 0x08,
        0x48, 0x89, 0x54, 0x24, 0x38,
        0xb9, 0x04, 0x00, 0x00, 0x00,
        0x31, 0xc0,
        0x48, 0x03, 0x44, 0x24, 0x08,
        0x48, 0x03, 0x44, 0x24, 0xe0,
        0x48, 0x03, 0x44, 0x24, 0x00,
        0xff, 0xc9,
        0x75, 0xed,
        0x48, 0x8b, 0x54, 0x24, 0x38,
    };
}

std::vector<std::uint8_t> inflateCompareBody(const std::vector<std::uint8_t>& prior, const std::uint8_t back) {
    std::vector<std::uint8_t> bytes{
        0x48, 0x89, 0x7c, 0x24, 0xe0,
        0x48, 0x89, 0x7c, 0x24, 0x00,
        0xb9, 0x04, 0x00, 0x00, 0x00,
        0x31, 0xc0,
        0x48, 0x03, 0x44, 0x24, 0xe0,
    };
    append(bytes, prior);
    append(bytes, {0x48, 0x39, 0x3c, 0x24, 0x75, 0x03, 0x48, 0x01, 0xf0, 0xff, 0xc9, 0x75, back});
    return bytes;
}

const std::vector<std::uint8_t> AddRdiToR8{0x49, 0x01, 0xf8};
const std::vector<std::uint8_t> CompareRdiAtRspShifted{0x48, 0x39, 0x7c, 0x24, 0x20};

void stubsShortOperandsAtRspBeforeABranchWithTheInstructionsBeforeThem() {
    const auto withAdd = inflateCompareBody(AddRdiToR8, 0xeb);
    std::vector<std::uint8_t> withLoad{
        0x48, 0x89, 0x7c, 0x24, 0xe0,
        0x48, 0x89, 0x7c, 0x24, 0x00,
        0xb9, 0x04, 0x00, 0x00, 0x00,
        0x31, 0xc0,
        0x48, 0x03, 0x44, 0x24, 0xe0,
        0x48, 0x39, 0x3c, 0x24, 0x75, 0x03, 0x48, 0x01, 0xf0, 0xff, 0xc9, 0x75, 0xee,
    };
    const auto input = image({framelessLeaf(InflatePrologue, withAdd, inflateExit()), framelessLeaf(InflatePrologue, withLoad, inflateExit())});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 2 && report.OperandStubs == 2 && report.StubbedOperands == 3 && report.MovedOperands == 5 && sites.size() == 6 && report.Skipped.empty(), "inflate_fast's 4-byte cmp at rsp between an instruction and a branch was not framed with a stub over the instruction before it");
    const auto add = input.functions[0].Begin + InflatePrologue.size() + 22;
    require(sites[2].Address == add && sites[2].Length == AddRdiToR8.size() + 4, "the stub site does not start at the add before the cmp at rsp");
    std::vector<std::uint8_t> addBody = AddRdiToR8;
    append(addBody, CompareRdiAtRspShifted);
    require(sites[2].Body == stubBody(addBody), "the add was not copied ahead of the cmp re-encoded with a displacement of the shift");
    const auto load = input.functions[1].Begin + InflatePrologue.size() + 17;
    require(sites[5].Address == load && sites[5].Length == 9, "the stub site does not start at the load before the cmp at rsp");
    std::vector<std::uint8_t> loadBody{0x48, 0x03, 0x44, 0x24, 0x00};
    append(loadBody, CompareRdiAtRspShifted);
    require(sites[5].Body == stubBody(loadBody), "the load before the cmp, itself an rsp operand, was not moved by the shift in the stub");
    for (const auto& displacement : displacements) {
        const auto at = Base + displacement.Offset;
        require((at < sites[2].Address || at >= sites[2].Address + sites[2].Length) && (at < sites[5].Address || at >= sites[5].Address + sites[5].Length), "an rsp operand inside a stub site was also moved in place");
    }
}

void skipsShortOperandsAtRspWhoseWidenedStubCoversABranchTarget() {
    std::vector<std::uint8_t> body{
        0x48, 0x89, 0x7c, 0x24, 0xe0,
        0x48, 0x89, 0x7c, 0x24, 0x00,
        0xb9, 0x04, 0x00, 0x00, 0x00,
        0x31, 0xc0,
    };
    append(body, AddRdiToR8);
    append(body, {0x48, 0x39, 0x3c, 0x24, 0x75, 0x03, 0x48, 0x01, 0xf0, 0xff, 0xc9, 0x75, 0xf3});
    const auto input = image({framelessLeaf(InflatePrologue, body, inflateExit())});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(sites.empty() && displacements.empty() && report.RedZoneFunctions == 1 && report.Skipped.at("rsp operand that cannot move and has no room for a stub") == 1, "a cmp at rsp whose loop branches back to it was framed with a stub that starts at the add before it");
}

void framesLeavesThatAllocateBelowTheirPushes() {
    const auto body = inflateBody();
    const auto input = image({framelessLeaf(InflatePrologue, body, inflateExit())});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.RedZoneFunctions == 1 && report.Framed == 1 && report.FramedWithoutFramePointer == 1 && report.Exits == 1 && sites.size() == 2, "a leaf that allocates with sub after its pushes and spills below its allocation was not framed");
    const auto& entry = sites[0];
    require(entry.Address == Base + 0x40 && entry.OriginalBytes == InflatePrologue, "the prologue site does not cover the pushes and the sub");
    std::vector<std::uint8_t> entryBody = InflatePrologue;
    append(entryBody, {0x48, 0x8d, 0x64, 0x24, 0xe0, 0xe9, 0, 0, 0, 0});
    require(entry.Body == entryBody, "the prologue stub does not lower rsp by 0x20 after the sub");
    const auto& exit = sites[1];
    require(exit.Address == Base + 0x40 + InflatePrologue.size() + body.size() && exit.OriginalBytes == inflateExit(), "the exit site does not cover the add, the pops and the ret");
    std::vector<std::uint8_t> exitBody{0x48, 0x8d, 0xa4, 0x24, 0x20, 0, 0, 0};
    append(exitBody, inflateExit());
    append(exitBody, {0xe9, 0, 0, 0, 0});
    require(exit.Body == exitBody, "the exit stub does not raise rsp by 0x20 before the add");
    const std::vector<std::uint8_t> moved{0x00, 0x20, 0x28, 0x58, 0x28, 0x00, 0x20, 0x58};
    require(displacements.size() == moved.size() && report.MovedOperands == moved.size(), "not every rsp operand of the allocating leaf was moved");
    for (std::size_t index = 0; index < moved.size(); ++index)
        require(displacements[index].Bytes == std::vector<std::uint8_t>{moved[index]}, "an rsp operand of the allocating leaf was not moved by 0x20 from the post-sub rsp");
    require(displacements[0].Offset == 0x40 + InflatePrologue.size() + 4, "the spill below the allocation was not the first operand moved");
}

void framesLeavesThatAllocateWithLeaOrALargeSub() {
    const std::vector<std::uint8_t> leaPrologue{0x53, 0x41, 0x56, 0x48, 0x8d, 0x64, 0x24, 0xc0};
    const std::vector<std::uint8_t> leaExit{0x48, 0x8d, 0x64, 0x24, 0x40, 0x41, 0x5e, 0x5b, 0xc3};
    const std::vector<std::uint8_t> largePrologue{0x53, 0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00};
    const std::vector<std::uint8_t> largeExit{0x48, 0x81, 0xc4, 0x00, 0x01, 0x00, 0x00, 0x5b, 0xc3};
    std::vector<std::uint8_t> largeBody = StoreBelowRsp;
    append(largeBody, {0x48, 0x8b, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00});
    const auto input = image({framelessLeaf(leaPrologue, StoreBelowRsp, leaExit), framelessLeaf(largePrologue, largeBody, largeExit)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 2 && sites.size() == 4 && displacements.size() == 3, "leaves that allocate with lea or a 32-bit sub were not framed");
    require(sites[0].OriginalBytes == leaPrologue && sites[1].OriginalBytes == leaExit, "the lea allocation and free are not part of the prologue and exit sites");
    require(sites[2].OriginalBytes == largePrologue && sites[3].OriginalBytes == largeExit, "the 32-bit sub and add are not part of the prologue and exit sites");
    require(displacements[2].Bytes == std::vector<std::uint8_t>{0x90, 0x00, 0x00, 0x00}, "the 32-bit displacement into the allocation was not moved by the shift");
}

void skipsAllocatingLeavesWithoutAMatchingFree() {
    const std::vector<std::uint8_t> otherFree{0x48, 0x83, 0xc4, 0x30};
    std::vector<std::uint8_t> wrongFree = otherFree;
    append(wrongFree, InflatePops);
    std::vector<std::uint8_t> moreRsp = StoreBelowRsp;
    append(moreRsp, {0x48, 0x83, 0xec, 0x08});
    const auto input = image({framelessLeaf(InflatePrologue, StoreBelowRsp, InflatePops), framelessLeaf(InflatePrologue, StoreBelowRsp, wrongFree), framelessLeaf(InflatePrologue, moreRsp, inflateExit())});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(sites.empty() && displacements.empty() && report.RedZoneFunctions == 3, "an allocating leaf without a matching free, or with more rsp arithmetic, was framed");
    require(report.Skipped.at("exit does not free the prologue's allocation") == 1, "an exit that does not free the allocation was not reported with its own reason");
    require(report.Skipped.at("other use of rsp") == 2, "an add of another size before the pops, or more rsp arithmetic in the body, was not reported as another use of rsp");
}

void skipsLeavesWithoutFramePointerThatPushOrPop() {
    const std::vector<std::vector<std::uint8_t>> moves{
        {0x50, 0x48, 0x8b, 0x1c, 0x24, 0x58},
        {0x9c, 0x48, 0x83, 0x24, 0x24, 0xfe, 0x9d},
        {0x6a, 0x01, 0x48, 0x8b, 0x44, 0x24, 0x00, 0x58},
        {0xff, 0x74, 0x24, 0x08, 0x8f, 0x04, 0x24},
        {0x41, 0x50, 0x41, 0x58},
        {0x66, 0x50, 0x66, 0x58},
        {0x0f, 0xa8, 0x0f, 0xa9},
    };
    std::vector<std::vector<std::uint8_t>> functions;
    for (const auto& move : moves) {
        auto body = StoreBelowRsp;
        append(body, move);
        functions.push_back(framelessLeaf(FramelessPrologue, body, FramelessExit));
    }
    auto withFramePointer = StoreBelowPushes;
    append(withFramePointer, {0x52, 0x58});
    functions.push_back(leaf(withFramePointer));
    const auto input = image(functions);
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.RedZoneFunctions == functions.size() && report.FramedWithoutFramePointer == 0 && displacements.empty(), "a leaf without a frame pointer that pushes or pops between its prologue and its exits was framed, so its rsp operands after a push were moved past the pushed slot");
    require(report.Skipped.at("push or pop inside the body") == moves.size(), "a leaf without a frame pointer that pushes or pops between its prologue and its exits was not reported with its reason");
    require(report.Framed == 1 && sites.size() == 2, "a leaf with a frame pointer, whose rsp operands are refused anyway, was not framed because it pushes and pops");
}

Image calledThenUncalled(const std::vector<std::uint8_t>& called, const std::vector<std::uint8_t>& uncalled) {
    Image result;
    result.bytes = callTo(0, 0x40);
    result.bytes.push_back(0xc3);
    result.bytes = padTo(result.bytes, 0x40);
    append(result.bytes, called);
    result.bytes.resize(0x60, 0x90);
    append(result.bytes, uncalled);
    result.bytes = padTo(result.bytes, 0x80);
    result.segments.push_back({1, 5, 0, Base, Base, result.bytes.size(), result.bytes.size(), 16});
    result.functions.push_back({Base + 0x40, Base + 0x40 + called.size()});
    result.functions.push_back({Base + 0x60, Base + 0x60 + uncalled.size()});
    return result;
}

void framesEachFunctionFromItsOwnFde() {
    std::vector<std::uint8_t> body = StoreBelowRsp;
    append(body, LoadBelowRsp);
    const auto input = calledThenUncalled(framelessLeaf(FramelessPrologue, body, FramelessExit), framelessLeaf(FramelessPrologue, StoreBelowRsp, FramelessExit));
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Functions == 2 && report.Framed == 2 && report.Exits == 2 && sites.size() == 4 && displacements.size() == 3, "two adjacent functions were not framed each by its own FDE");
    require(sites[0].Address < Base + 0x60 && sites[1].Address + sites[1].Length <= Base + 0x60 && displacements[0].Offset < 0x60 && displacements[1].Offset < 0x60, "the first function's frame runs past its FDE end");
    require(sites[2].Address == Base + 0x60 && sites[2].OriginalBytes == FramelessPrologue && displacements[2].Offset > 0x60, "the function reached without a call was not framed from its own FDE start");
}

void treatsATailCallPastTheFdeEndAsLeaving() {
    std::vector<std::uint8_t> called = FramelessPrologue;
    append(called, StoreBelowRsp);
    append(called, {0x85, 0xff, 0x75, 0x06});
    append(called, FramelessExit);
    append(called, {0x41, 0x5f, 0x41, 0x5e, 0x5b, 0xe9, 0x02, 0x00, 0x00, 0x00});
    const auto input = calledThenUncalled(called, framelessLeaf(FramelessPrologue, StoreBelowRsp, FramelessExit));
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Skipped.at("branch leaves the function") == 1, "the tail call past the FDE end was not reported as leaving the function");
    require(report.Framed == 1 && sites.size() == 2 && sites[0].Address == Base + 0x60, "the tail-called function was not framed from its own FDE start");
}

void framesALeafAfterAJumpWithoutInt3FromItsOwnFde() {
    std::vector<std::uint8_t> first = FramelessPrologue;
    append(first, StoreBelowRsp);
    append(first, {0x31, 0xc0, 0xeb, 0x06});
    append(first, FramelessExit);
    append(first, {0x48, 0x3b, 0x44, 0x24, 0xf0, 0x73, 0xf3, 0x48, 0xff, 0xc0, 0xeb, 0xf4});
    require(first.size() == 0x20, "the first leaf must end where the second starts");
    std::vector<std::uint8_t> second = StoreBelowRsp;
    append(second, LoadBelowRsp);
    const auto input = calledThenUncalled(first, framelessLeaf(FramelessPrologue, second, FramelessExit));
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 2 && report.Exits == 2 && sites.size() == 4 && displacements.size() == 4, "a leaf right after a jmp without int3 padding was not framed on its own");
    require(sites[1].Address + sites[1].Length <= Base + 0x60 && displacements[1].Offset < 0x60, "the leaf ending in a jmp took the next leaf into its body");
    require(sites[2].Address == Base + 0x60 && sites[2].OriginalBytes == FramelessPrologue && displacements[2].Offset > 0x60, "the leaf after the jmp was not framed from its own FDE start");
}

void keepsColdBlocksAfterARetInsideTheFde() {
    std::vector<std::uint8_t> leaf = FramelessPrologue;
    append(leaf, StoreBelowRsp);
    append(leaf, {0x85, 0xff, 0x74, 0x12});
    append(leaf, LoadBelowRsp);
    append(leaf, FramelessExit);
    append(leaf, {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00});
    append(leaf, {0x48, 0x8b, 0x54, 0x24, 0xf0});
    append(leaf, FramelessExit);
    const auto input = image({leaf});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && report.Exits == 2 && sites.size() == 3, "the exit of a cold block after the ret inside the FDE was not framed");
    require(displacements.size() == 3 && displacements[2].Offset == 0x40 + 0x20 + 4 && displacements[2].Bytes == std::vector<std::uint8_t>{0x00}, "the rsp operand of a cold block after the ret inside the FDE was not moved");
}

void keepsLoopsAlignedAfterAJump() {
    std::vector<std::uint8_t> loop = FramelessPrologue;
    append(loop, StoreBelowRsp);
    append(loop, {0x31, 0xc0, 0xeb, 0x04, 0x66, 0x90, 0xff, 0xc0, 0x3b, 0x44, 0x24, 0xf0, 0x72, 0xf8});
    append(loop, FramelessExit);
    const auto input = image({loop});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Framed == 1 && sites.size() == 2 && displacements.size() == 2, "a leaf whose loop header is aligned after a jump was not framed");
}

void leavesOtherFunctions() {
    std::vector<std::uint8_t> nonLeaf = Prologue;
    append(nonLeaf, StoreBelowPushes);
    append(nonLeaf, {0xff, 0xd0});
    append(nonLeaf, Exit);
    const auto input = image({leaf(LoadAbovePushes), nonLeaf});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(sites.empty() && report.RedZoneFunctions == 0, "a leaf without red-zone use or a function that calls was framed");
}

void skipsSitesTakenByOtherStubs() {
    const auto input = image({leaf(StoreBelowPushes)});
    Codegen::TrampolineSite other{};
    other.Address = Base + 0x40;
    other.Length = 5;
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {other}, report, displacements);
    require(sites.empty() && report.Skipped.at("overlaps another stub") == 1, "a site over another stub was framed");
}

void framesAPointerOnlyFunctionByItsOwnBounds() {
    const auto first = leaf(StoreBelowPushes);
    std::vector<std::uint8_t> secondBody = StoreBelowPushes;
    append(secondBody, LoadBelowPushes);
    const auto second = leaf(secondBody);
    std::vector<std::uint8_t> bytes = callTo(0, 0x40);
    bytes.push_back(0xc3);
    bytes = padTo(bytes, 0x40);
    append(bytes, first);
    const auto secondStart = bytes.size();
    append(bytes, second);
    bytes = padTo(bytes, (bytes.size() + 15) / 16 * 16 + 16);
    const std::vector<Domain::ProgramHeader> segments{{1, 5, 0, Base, Base, bytes.size(), bytes.size(), 16}};
    const std::vector<Codegen::RedZoneFunction> functions{{Base + 0x40, Base + 0x40 + first.size()}, {Base + secondStart, Base + secondStart + second.size()}};
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(bytes, segments, functions, {}, report, displacements);
    require(report.Functions == 2 && report.Framed == 2 && report.Exits == 2, "a function reached only through a pointer was not framed by its own bounds");
    require(sites.size() == 4, "two framed leaves need two prologue and two exit sites");
    require(sites[2].Address == Base + secondStart && sites[2].OriginalBytes == Prologue, "the second function's prologue site is not at its own start");
    require(sites[1].Address + sites[1].Length == Base + 0x40 + first.size(), "the first function's exit runs past its end");
}

void leavesCodeWithoutAFunctionRange() {
    const auto input = image({leaf(StoreBelowPushes)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    const auto sites = Codegen::FrameRedZoneFunctions(input.bytes, input.segments, {}, {}, report, displacements);
    require(sites.empty() && report.Functions == 0, "code without a function range was framed");
}

}

void listsEachRedZoneLeafWithWhyItWasNotFramed() {
    std::vector<std::uint8_t> framed = StoreBelowRsp;
    append(framed, LoadBelowRsp);
    const auto input = image({framelessLeaf(FramelessPrologue, framed, FramelessExit), framelessLeaf({}, framed, PopRbxExit), leaf(LoadAbovePushes)});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements);
    require(report.Leaves.size() == 2, "the report must list the two red-zone leaves and not the leaf that stays above its pushes");
    require(report.Leaves[0].Begin == input.functions[0].Begin && report.Leaves[0].End == input.functions[0].End && report.Leaves[0].Reason.empty(), "the framed leaf was not listed by its bounds without a reason");
    require(report.Leaves[1].Begin == input.functions[1].Begin && report.Leaves[1].End == input.functions[1].End && report.Leaves[1].Reason == "push or pop in a leaf without pushes", "the skipped leaf was not listed by its bounds with its reason");
}

const std::vector<std::uint8_t> AllocatingPrologue{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x53, 0x48, 0x83, 0xec, 0x18};
const std::vector<std::uint8_t> FreeingExit{0x48, 0x83, 0xc4, 0x18, 0x5b, 0x41, 0x5f, 0x5d, 0xc3};
const std::vector<std::uint8_t> RestoringExit{0x48, 0x8d, 0x65, 0xf0, 0x5b, 0x41, 0x5f, 0x5d, 0xc3};

std::vector<std::uint8_t> allocatingLeaf(const std::vector<std::uint8_t>& body, const std::vector<std::uint8_t>& exit) {
    std::vector<std::uint8_t> bytes = AllocatingPrologue;
    append(bytes, body);
    append(bytes, exit);
    return bytes;
}

Codegen::RedZoneFrameReport allocationReport(const std::vector<std::uint8_t>& function, const bool count) {
    const auto input = image({function});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements, false, count);
    return report;
}

void countsAFramePointerLeafsAllocationOnlyWhenAsked() {
    const std::vector<std::uint8_t> insideAllocation{0x48, 0x89, 0x7d, 0xe0, 0x8b, 0x45, 0xdc};
    for (const auto& exit : {FreeingExit, RestoringExit}) {
        const auto leaf = allocatingLeaf(insideAllocation, exit);
        const auto counted = allocationReport(leaf, true);
        require(counted.RedZoneFunctions == 0 && counted.Leaves.empty(), "a frame-pointer leaf whose slots sit inside its allocation stayed a red-zone leaf");
        const auto uncounted = allocationReport(leaf, false);
        require(uncounted.RedZoneFunctions == 1 && uncounted.Leaves.size() == 1 && uncounted.Leaves[0].Reason == "other use of rsp", "without --frame-pointer-allocation a frame-pointer leaf that allocates is no longer listed as before");
    }
    const auto below = allocationReport(allocatingLeaf({0x48, 0x89, 0x7d, 0xd0}, FreeingExit), true);
    require(below.RedZoneFunctions == 1 && below.Leaves.size() == 1, "a slot below the allocation left the table");
    std::vector<std::uint8_t> moving = insideAllocation;
    append(moving, {0x48, 0x83, 0xec, 0x08});
    const auto moved = allocationReport(allocatingLeaf(moving, FreeingExit), true);
    require(moved.RedZoneFunctions == 1 && moved.Leaves.size() == 1, "a leaf that writes rsp again after its allocation left the table");
    const auto keeps = [&](const std::vector<std::uint8_t>& extra, const char* what) {
        std::vector<std::uint8_t> body = insideAllocation;
        append(body, extra);
        const auto report = allocationReport(allocatingLeaf(body, FreeingExit), true);
        require(report.RedZoneFunctions == 1 && report.Leaves.size() == 1, what);
    };
    keeps({0x48, 0x8b, 0x44, 0x24, 0x08}, "a leaf with an rsp operand left the table");
    keeps({0x48, 0x8d, 0x45, 0xe0}, "a leaf that takes the address of a slot inside its allocation left the table");
    keeps({0x51, 0x59}, "a leaf that pushes and pops in its body left the table");
    keeps({0x48, 0x83, 0xed, 0x40, 0x48, 0x89, 0x45, 0xf8}, "a leaf that moves rbp and then stores through it left the table");
    keeps({0x48, 0x89, 0xe8}, "a leaf that copies rbp to another register left the table");
    keeps({0x48, 0x89, 0x04, 0x29}, "a leaf that uses rbp as a SIB index left the table");
    keeps({0x48, 0x89, 0x44, 0x29, 0xf0}, "a leaf that uses rbp as a SIB index with a displacement left the table");
    keeps({0xc9}, "a leaf with leave in its body left the table");
    keeps({0xc8, 0x10, 0x00, 0x00}, "a leaf with enter in its body left the table");
    keeps({0x48, 0x94}, "a leaf that exchanges rsp left the table");
    keeps({0x67, 0x48, 0x89, 0x45, 0xe0}, "a leaf with a 32-bit address in its body left the table");
    keeps({0x48, 0x0f, 0xa3, 0x45, 0xe0}, "a leaf with a bit test on a slot left the table");
    keeps({0x74, 0x04}, "a leaf with a branch into its exit after the free left the table");
    keeps({0x48, 0x89, 0x04, 0x2d, 0xf0, 0xff, 0xff, 0xff}, "a leaf that uses rbp as a SIB index without a base left the table");
    keeps({0x48, 0xbc, 0, 0, 0, 0, 0, 0, 0, 0}, "a leaf that moves an immediate into rsp left the table");
    keeps({0x74, 0x05, 0x5b, 0x41, 0x5f, 0x5d, 0xc3}, "a leaf with an exit that does not free the allocation left the table");
    const auto limit = allocationReport(allocatingLeaf({0x48, 0x89, 0x7d, 0xd8}, FreeingExit), true);
    require(limit.RedZoneFunctions == 0 && limit.Leaves.empty(), "a slot at the lowest byte of the allocation kept the leaf in the table");
    const auto pastLimit = allocationReport(allocatingLeaf({0x48, 0x89, 0x7d, 0xd7}, FreeingExit), true);
    require(pastLimit.RedZoneFunctions == 1 && pastLimit.Leaves.size() == 1, "a slot one byte below the allocation left the table");
    std::vector<std::uint8_t> twoExits = insideAllocation;
    append(twoExits, {0x74, 0x09});
    append(twoExits, FreeingExit);
    const auto both = allocationReport(allocatingLeaf(twoExits, FreeingExit), true);
    require(both.RedZoneFunctions == 0 && both.Leaves.empty(), "a leaf whose two exits both free the allocation stayed in the table");
    std::vector<std::uint8_t> wide{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x53, 0x48, 0x81, 0xec, 0x18, 0x00, 0x00, 0x00};
    append(wide, insideAllocation);
    append(wide, {0x48, 0x81, 0xc4, 0x18, 0x00, 0x00, 0x00, 0x5b, 0x41, 0x5f, 0x5d, 0xc3});
    const auto wideReport = allocationReport(wide, true);
    require(wideReport.RedZoneFunctions == 0 && wideReport.Leaves.empty(), "a leaf with a 32-bit allocation stayed in the table");
    keeps({0x74, 0xec}, "a leaf with a branch into its prologue left the table");
    keeps({0x74, 0xf8}, "a leaf with a branch into the middle of an instruction left the table");
    keeps({0x74, 0x10}, "a leaf with a branch past its end left the table");
    keeps({0x66, 0x74, 0x00}, "a leaf with a 16-bit conditional branch left the table");
    keeps({0x66, 0xeb, 0x00}, "a leaf with a 16-bit short jump left the table");
    keeps({0x66, 0xe9, 0x00, 0x00}, "a leaf with a 16-bit near jump left the table");
    const auto removed = [&](const std::vector<std::uint8_t>& extra, const char* what) {
        std::vector<std::uint8_t> body = insideAllocation;
        append(body, extra);
        const auto report = allocationReport(allocatingLeaf(body, FreeingExit), true);
        require(report.RedZoneFunctions == 0 && report.Leaves.empty(), what);
    };
    removed({0x0f, 0x28, 0x6d, 0xe0, 0x0f, 0x28, 0xec}, "a leaf whose only frame-register-numbered operands are xmm registers stayed in the table");
    removed({0x4c, 0x89, 0x65, 0xe0, 0x4d, 0x89, 0xec}, "a leaf that uses r12 and r13 stayed in the table");
    removed({0xeb, 0x02, 0xcd, 0x41, 0x8b, 0x45, 0xdc}, "a leaf with an int 0x41 skipped by a jump stayed in the table");
    keeps({0xeb, 0x02, 0xcd, 0x41, 0x48, 0x89, 0x7d, 0xd0}, "a leaf with a store below its allocation after an int 0x41 left the table");
    keeps({0xeb, 0x01, 0xcc, 0x8b, 0x45, 0xdc}, "a leaf with code after an int3 inside its body left the table");
    keeps({0xeb, 0x02, 0xcd, 0x2d, 0x8b, 0x45, 0xdc}, "a leaf with an int 0x2d, which resumes past its own bytes, left the table");
    {
        auto tail = allocatingLeaf(insideAllocation, FreeingExit);
        append(tail, {0xcc, 0x90});
        const auto report = allocationReport(tail, true);
        require(report.RedZoneFunctions == 1 && report.Leaves.size() == 1, "a leaf with code after its int3 padding left the table");
    }
    {
        std::vector<std::uint8_t> body = insideAllocation;
        append(body, {0xeb, 0x02, 0xcd, 0x41});
        const auto uncounted = allocationReport(allocatingLeaf(body, FreeingExit), false);
        require(uncounted.RedZoneFunctions == 1 && uncounted.Leaves.size() == 1 && uncounted.Leaves[0].Reason == "other use of rsp", "without --frame-pointer-allocation an int 0x41 changed how the leaf is listed");
    }
    std::vector<std::uint8_t> pastInt3 = insideAllocation;
    append(pastInt3, {0x74, 0x0a});
    std::vector<std::uint8_t> exitThenInt3 = FreeingExit;
    append(exitThenInt3, {0xcc, 0xc3});
    const auto afterInt3 = allocationReport(allocatingLeaf(pastInt3, exitThenInt3), true);
    require(afterInt3.RedZoneFunctions == 1 && afterInt3.Leaves.size() == 1, "a leaf with a branch past an int3 inside its FDE left the table");
    std::vector<std::uint8_t> fallThrough = AllocatingPrologue;
    append(fallThrough, insideAllocation);
    append(fallThrough, {0x74, 0x09});
    append(fallThrough, FreeingExit);
    fallThrough.push_back(0x90);
    const auto fell = allocationReport(fallThrough, true);
    require(fell.RedZoneFunctions == 1 && fell.Leaves.size() == 1, "a leaf that falls off its end after a ret left the table");
    const auto immediate = allocationReport(allocatingLeaf(insideAllocation, {0x48, 0x83, 0xc4, 0x18, 0x5b, 0x41, 0x5f, 0x5d, 0xc2, 0x08, 0x00}), true);
    require(immediate.RedZoneFunctions == 1 && immediate.Leaves.size() == 1, "a leaf that returns with an immediate left the table");
    const auto shortRestore = allocationReport(allocatingLeaf(insideAllocation, {0x48, 0x8d, 0x65, 0xe8, 0x5b, 0x41, 0x5f, 0x5d, 0xc3}), true);
    require(shortRestore.RedZoneFunctions == 1 && shortRestore.Leaves.size() == 1, "a leaf whose exit restores rsp from rbp at the wrong offset left the table");
    removed({0xc4, 0xe2, 0xb0, 0xf5, 0x52, 0x08}, "a leaf with a BMI2 bzhi on general registers stayed in the table");
    keeps({0xc4, 0xe2, 0xd0, 0xf5, 0x52, 0x08}, "a leaf with a bzhi whose index register is rbp left the table");
    removed({0xc4, 0x62, 0xb0, 0xf5, 0x62, 0x08}, "a bzhi into r12 kept the leaf in the table");
    removed({0xc4, 0xe2, 0x90, 0xf5, 0x52, 0x08}, "a bzhi indexed by r13 kept the leaf in the table");
    removed({0xc4, 0xc2, 0xb0, 0xf5, 0xd4}, "a bzhi from r12 kept the leaf in the table");
    removed({0xc4, 0xa2, 0xb0, 0xf5, 0x14, 0x20}, "a bzhi over an r12 SIB index kept the leaf in the table");
    keeps({0xc4, 0xe2, 0xb0, 0xf5, 0x62, 0x08}, "a bzhi into rsp left the table");
    keeps({0xc4, 0xe2, 0xb0, 0xf5, 0xd5}, "a bzhi from rbp left the table");
    keeps({0xc4, 0xe2, 0xb0, 0xf5, 0x14, 0x28}, "a bzhi over an rbp SIB index left the table");
    std::vector<std::uint8_t> noReturn = AllocatingPrologue;
    append(noReturn, insideAllocation);
    append(noReturn, {0xeb, 0xfe});
    const auto tail = allocationReport(noReturn, true);
    require(tail.RedZoneFunctions == 1 && tail.Leaves.size() == 1, "a leaf without a ret left the table");
    const auto indexed = allocationReport(allocatingLeaf({0x48, 0x89, 0x7c, 0xcd, 0xe0}, FreeingExit), true);
    require(indexed.RedZoneFunctions == 1 && indexed.Leaves.size() == 1, "an indexed slot inside the allocation, which can reach below it, left the table");
}

Codegen::RedZoneFrameReport suspectReport(const std::vector<std::uint8_t>& function, const bool suspect) {
    const auto input = image({function});
    Codegen::RedZoneFrameReport report;
    std::vector<Codegen::RedZoneDisplacement> displacements;
    Codegen::FrameRedZoneFunctions(input.bytes, input.segments, input.functions, {}, report, displacements, false, false, suspect);
    return report;
}

void listsFramePointerLeavesWithUnanalyzedInstructionsWhenAsked() {
    const auto movedFramePointer = leaf({0x48, 0x83, 0xed, 0x40, 0x48, 0x89, 0x45, 0xf8});
    const auto quiet = suspectReport(movedFramePointer, false);
    require(quiet.RedZoneFunctions == 0 && quiet.Leaves.empty(), "without --suspect-frame-pointer-leaves a leaf that moves rbp was listed");
    const auto suspected = suspectReport(movedFramePointer, true);
    require(suspected.RedZoneFunctions == 1 && suspected.Framed == 0 && suspected.Leaves.size() == 1 && suspected.Leaves[0].Reason == "frame-pointer leaf with an instruction the framer does not prove safe", "a leaf that moves rbp was not listed as a suspect");
    const auto indexed = suspectReport(leaf({0x48, 0x89, 0x44, 0x29, 0xf0}), true);
    require(indexed.RedZoneFunctions == 1 && indexed.Framed == 0, "a leaf that indexes with rbp was not listed as a suspect");
    const auto early = suspectReport(leaf({0xc9, 0x48, 0x8b, 0x45, 0xf8}), true);
    require(early.RedZoneFunctions == 1 && early.Framed == 0, "a leaf that reads through rbp after leave was not listed as a suspect");
    const auto pointer = suspectReport(leaf({0x48, 0x8d, 0x44, 0x24, 0x08}), true);
    require(pointer.RedZoneFunctions == 1 && pointer.Framed == 0, "a leaf that takes the address of an rsp slot was not listed as a suspect");
    const auto loop = suspectReport(leaf({0x48, 0x8b, 0x45, 0x08, 0x75, 0xfa}), true);
    require(loop.RedZoneFunctions == 0 && loop.Leaves.empty(), "a frame-pointer leaf that loops back to its first body instruction was listed as a suspect");
    const auto intoPrologue = suspectReport(leaf({0x48, 0x8b, 0x45, 0x08, 0x75, 0xf3}), true);
    require(intoPrologue.RedZoneFunctions == 1 && intoPrologue.Framed == 0, "a frame-pointer leaf that branches into its prologue was not listed as a suspect");
    const auto suspects = [&](const std::vector<std::uint8_t>& function, const char* what) {
        const auto report = suspectReport(function, true);
        require(report.RedZoneFunctions == 1 && report.Framed == 0 && report.Leaves.size() == 1 && report.Leaves[0].Reason == "frame-pointer leaf with an instruction the framer does not prove safe", what);
    };
    suspects(leaf({0x48, 0x8b, 0x45, 0x08, 0x75, 0xfb}), "a frame-pointer leaf that branches into the middle of an instruction was not listed as a suspect");
    std::vector<std::uint8_t> leaveWithPushes = Prologue;
    append(leaveWithPushes, {0x48, 0x8b, 0x45, 0x08, 0xc9, 0xc3});
    suspects(leaveWithPushes, "a frame-pointer leaf that leaves without popping its pushes was not listed as a suspect");
    std::vector<std::uint8_t> wrongRestore = Prologue;
    append(wrongRestore, {0x48, 0x8b, 0x45, 0x08, 0x48, 0x8d, 0x65, 0xf8});
    append(wrongRestore, Exit);
    suspects(wrongRestore, "a frame-pointer leaf that restores rsp from rbp at the wrong offset was not listed as a suspect");
    std::vector<std::uint8_t> unbalanced = AllocatingPrologue;
    append(unbalanced, {0x48, 0x8b, 0x45, 0x08, 0x48, 0x83, 0xc4, 0x10, 0x5b, 0x41, 0x5f, 0x5d, 0xc3});
    suspects(unbalanced, "a frame-pointer leaf that frees less than it allocated was not listed as a suspect");
    std::vector<std::uint8_t> immediateReturn = Prologue;
    append(immediateReturn, {0x48, 0x8b, 0x45, 0x08, 0x5b, 0x41, 0x5f, 0x5d, 0xc2, 0x08, 0x00});
    suspects(immediateReturn, "a frame-pointer leaf that returns with an immediate was not listed as a suspect");
    std::vector<std::uint8_t> bareReturn = Prologue;
    append(bareReturn, {0x48, 0x8b, 0x45, 0x08, 0xc3});
    suspects(bareReturn, "a frame-pointer leaf that returns without popping rbp was not listed as a suspect");
    std::vector<std::uint8_t> unfreed = AllocatingPrologue;
    append(unfreed, {0x48, 0x8b, 0x45, 0x08});
    append(unfreed, Exit);
    suspects(unfreed, "a frame-pointer leaf that pops without freeing its allocation was not listed as a suspect");
    suspects(leaf({0x48, 0x8b, 0x45, 0x08, 0x74, 0x01}), "a frame-pointer leaf that branches into the middle of its exit was not listed as a suspect");
    std::vector<std::uint8_t> freed = AllocatingPrologue;
    append(freed, {0x48, 0x8b, 0x45, 0x08});
    append(freed, FreeingExit);
    const auto allocatedPlain = suspectReport(freed, true);
    require(allocatedPlain.RedZoneFunctions == 0 && allocatedPlain.Leaves.empty(), "a frame-pointer leaf with an allocation and a matching exit was listed as a suspect");
    suspects(leaf({0xc5, 0xfc, 0x10, 0x45, 0x10}), "a frame-pointer leaf with an instruction the framer does not decode was not listed as a suspect");
    const auto plain = suspectReport(leaf(LoadAbovePushes), true);
    require(plain.RedZoneFunctions == 0 && plain.Leaves.empty(), "a frame-pointer leaf with only analyzed instructions was listed as a suspect");
    std::vector<std::uint8_t> leaving{0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x45, 0x10, 0xc9, 0xc3};
    const auto leaveExit = suspectReport(leaving, true);
    require(leaveExit.RedZoneFunctions == 0 && leaveExit.Leaves.empty(), "a frame-pointer leaf that ends with leave; ret was listed as a suspect");
}

int main() {
    try {
        framesRedZoneLeaf();
        leavesOtherFunctions();
        skipsSitesTakenByOtherStubs();
        framesLeafWithoutFramePointer();
        shiftsByTheDeepestOperandBelowRsp();
        movesPositiveOperandsThatFitAfterTheShift();
        shiftsByTheWholeRedZoneForAddressesBelowRsp();
        shiftsByTheWholeRedZoneForIndexedOperands();
        framesLeavesThatAllocateBelowTheirPushes();
        framesLeavesThatAllocateWithLeaOrALargeSub();
        skipsAllocatingLeavesWithoutAMatchingFree();
        skipsLeavesWithoutFramePointerThatPushOrPop();
        stubsOperandsWhoseDisplacementOverflows();
        stubsOperandsAtRspWithTheInstructionsAfterThem();
        stubsVexOperandsAtRsp();
        stubsOperandsAtRspWithTheInstructionsBeforeThem();
        stubsShortOperandsAtRspBeforeABranchWithTheInstructionsBeforeThem();
        skipsShortOperandsAtRspWhoseWidenedStubCoversABranchTarget();
        skipsLeavesWithOperandsThatCannotMove();
        widensShortProloguesAndExits();
        widensAShortFramePointerPrologue();
        framesLeavesWithoutPushes();
        skipsFragmentsThatPopWithoutPushing();
        framesShrinkWrappedLeavesOnlyWhenAsked();
        skipsShrinkWrappedLeavesWithHiddenRspUsesOrOutsideEntries();
        keepsLeavesThatBranchToAColdPartWhole();
        keepsLeavesWithHiddenReachOrForeignEntriesWhole();
        skipsWideningOverBranchTargets();
        skipsBranchesIntoTheInstructionsAfterAShortPrologue();
        widensAShortPrologueThatAllocates();
        widensBothExitsOfALeafWithoutPushes();
        widensExitsOverVexOperandsAtRsp();
        framesFramePointerLeavesThatPushInTheirBody();
        skipsLeavesThatCopyRsp();
        framesEachFunctionFromItsOwnFde();
        treatsATailCallPastTheFdeEndAsLeaving();
        framesALeafAfterAJumpWithoutInt3FromItsOwnFde();
        keepsColdBlocksAfterARetInsideTheFde();
        keepsLoopsAlignedAfterAJump();
        framesAPointerOnlyFunctionByItsOwnBounds();
        leavesCodeWithoutAFunctionRange();
        listsEachRedZoneLeafWithWhyItWasNotFramed();
        tellsPopsWithoutRexFromXop();
        countsAFramePointerLeafsAllocationOnlyWhenAsked();
        listsFramePointerLeavesWithUnanalyzedInstructionsWhenAsked();
        std::cout << "red-zone framer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "red-zone framer tests failed: " << error.what() << '\n';
        return 1;
    }
}
