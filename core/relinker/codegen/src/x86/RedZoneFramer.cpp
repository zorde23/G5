#include <codegen/x86/RedZoneFramer.hpp>
#include <codegen/CodegenException.hpp>
#include <codegen/IInstructionScanner.hpp>
#include <codegen/x86/StubBodyBuilder.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <algorithm>
#include <bitset>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>

namespace Codegen {

namespace {

constexpr std::uint8_t kRegRsp = 4;
constexpr std::uint8_t kRegRbp = 5;
constexpr std::size_t kJumpBytes = 5;
constexpr std::int64_t kRedZoneBytes = 0x80;
constexpr std::int64_t kStackAlignment = 16;
constexpr std::uint8_t kJumpBack[] = {0xe9, 0x00, 0x00, 0x00, 0x00};

std::vector<std::uint8_t> lowerRsp(const std::int64_t shift) {
    return {0x48, 0x8d, 0x64, 0x24, static_cast<std::uint8_t>(-shift)};
}

std::vector<std::uint8_t> raiseRsp(const std::int64_t shift) {
    const auto value = static_cast<std::uint32_t>(shift);
    return {0x48, 0x8d, 0xa4, 0x24, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
}

struct Insn {
    std::size_t Offset;
    std::size_t Length;
    DecodedInstructionInfo Info;
};

struct BranchSite {
    std::size_t Target;
    std::size_t Source;
    bool Call;
};

bool enteredFromOutside(const std::vector<BranchSite>& branches, const std::size_t start, const std::size_t limit, const bool callAtStart) {
    for (auto hit = std::lower_bound(branches.begin(), branches.end(), start, [](const BranchSite& site, const std::size_t value) { return site.Target < value; }); hit != branches.end() && hit->Target < limit; ++hit)
        if ((hit->Source < start || hit->Source >= limit) && !(hit->Target == start && (callAtStart ? hit->Call : true))) return true;
    return false;
}

std::optional<std::uint8_t> pushedRegister(const std::uint8_t* code, const Insn& insn) {
    if (insn.Length == 1 && code[0] >= 0x50 && code[0] <= 0x57 && code[0] != 0x50 + kRegRsp) return static_cast<std::uint8_t>(code[0] - 0x50);
    if (insn.Length == 2 && code[0] == 0x41 && code[1] >= 0x50 && code[1] <= 0x57) return static_cast<std::uint8_t>(code[1] - 0x50 + 8);
    return std::nullopt;
}

std::optional<std::uint8_t> poppedRegister(const std::uint8_t* code, const Insn& insn) {
    if (insn.Length == 1 && code[0] >= 0x58 && code[0] <= 0x5f && code[0] != 0x58 + kRegRsp) return static_cast<std::uint8_t>(code[0] - 0x58);
    if (insn.Length == 2 && code[0] == 0x41 && code[1] >= 0x58 && code[1] <= 0x5f) return static_cast<std::uint8_t>(code[1] - 0x58 + 8);
    return std::nullopt;
}

bool pushesOrPops(const std::uint8_t* code, const Insn& insn) {
    if (pushedRegister(code, insn) || poppedRegister(code, insn)) return true;
    const auto& info = insn.Info;
    const auto lead = code[info.OpcodeOffset];
    if (info.RexPrefix == 0 && (lead == 0xc4 || lead == 0xc5 || lead == 0x62 || lead == 0x8f) && info.Opcode != 0x8f) return false;
    if (info.IsTwoByteOpcode) return info.Opcode == 0xa0 || info.Opcode == 0xa1 || info.Opcode == 0xa8 || info.Opcode == 0xa9;
    const auto reg = static_cast<std::uint8_t>((info.ModRmByte >> 3) & 7);
    return (info.Opcode >= 0x50 && info.Opcode <= 0x5f) || info.Opcode == 0x68 || info.Opcode == 0x6a || info.Opcode == 0x9c || info.Opcode == 0x9d || (info.Opcode == 0x8f && info.HasModRm && reg == 0) || (info.Opcode == 0xff && info.HasModRm && reg == 6);
}

std::optional<std::int64_t> rspAdjustment(const std::uint8_t* code, const Insn& insn) {
    std::int32_t value = 0;
    if (insn.Length == 4 && code[0] == 0x48 && code[1] == 0x83 && (code[2] == 0xec || code[2] == 0xc4)) {
        const std::int64_t amount = static_cast<std::int8_t>(code[3]);
        return code[2] == 0xec ? -amount : amount;
    }
    if (insn.Length == 7 && code[0] == 0x48 && code[1] == 0x81 && (code[2] == 0xec || code[2] == 0xc4)) {
        std::memcpy(&value, code + 3, 4);
        return code[2] == 0xec ? -std::int64_t{value} : std::int64_t{value};
    }
    if (insn.Length == 5 && std::memcmp(code, "\x48\x8d\x64\x24", 4) == 0) return static_cast<std::int8_t>(code[4]);
    if (insn.Length == 8 && std::memcmp(code, "\x48\x8d\xa4\x24", 4) == 0) {
        std::memcpy(&value, code + 4, 4);
        return value;
    }
    return std::nullopt;
}

struct MemoryUse {
    bool UsesRsp = false;
    bool RegisterRsp = false;
    bool RbpBased = false;
    std::int64_t RbpDisp = 0;
    bool RspBased = false;
    std::int64_t RspDisp = 0;
    std::uint8_t Mod = 0;
    std::size_t DispOffset = 0;
    bool Indexed = false;
    bool Address = false;
};

bool xopPrefixed(const std::uint8_t* code, const Insn& insn) {
    const auto at = insn.Info.OpcodeOffset;
    return code[at] == 0x8f && at + 1 < insn.Length && (code[at + 1] & 0x1f) >= 8;
}

void extensionBits(const std::uint8_t* code, const Insn& insn, bool& r, bool& x, bool& b) {
    const auto& info = insn.Info;
    r = x = b = false;
    if (info.RexPrefix != 0) {
        r = (info.RexPrefix & 4) != 0;
        x = (info.RexPrefix & 2) != 0;
        b = (info.RexPrefix & 1) != 0;
        return;
    }
    const auto lead = code[info.OpcodeOffset];
    if ((lead == 0xc4 || lead == 0x62 || xopPrefixed(code, insn)) && info.OpcodeOffset + 1 < insn.Length) {
        r = (code[info.OpcodeOffset + 1] & 0x80) == 0;
        x = (code[info.OpcodeOffset + 1] & 0x40) == 0;
        b = (code[info.OpcodeOffset + 1] & 0x20) == 0;
    } else if (lead == 0xc5 && info.OpcodeOffset + 1 < insn.Length) {
        r = (code[info.OpcodeOffset + 1] & 0x80) == 0;
    }
}

enum class Analysis : std::uint8_t { Understood, Unknown, FrameHazard };

bool vectorOpcode(const std::uint8_t op) {
    return (op >= 0x10 && op <= 0x17) || op == 0x28 || op == 0x29 || op == 0x2e || op == 0x2f || (op >= 0x51 && op <= 0x6d) || op == 0x6f || op == 0x70 || (op >= 0x74 && op <= 0x76) || op == 0x7f ||
           op == 0xc2 || op == 0xc6 || op == 0xd4 || op == 0xd6 || op == 0xdb || op == 0xdf || op == 0xe6 || op == 0xeb || op == 0xef || op == 0xf4 || (op >= 0xfa && op <= 0xfe);
}

Analysis analyzeForFrame(const std::uint8_t* code, const Insn& insn, const bool rspBaseAllowed, const bool vexVectors = false) {
    const auto& info = insn.Info;
    const auto at = info.OpcodeOffset;
    const auto lead = code[at];
    const auto frameRegister = [](const unsigned reg) { return reg == kRegRsp || reg == kRegRbp; };
    if (lead == 0xc8 || lead == 0xc9) return Analysis::FrameHazard;
    if (info.AddressSize32 || info.TruncatesBranchTarget) return Analysis::Unknown;
    bool r = false;
    bool x = false;
    bool b = false;
    extensionBits(code, insn, r, x, b);
    const auto modrm = info.ModRmByte;
    const auto digit = static_cast<std::uint8_t>((modrm >> 3) & 7);
    enum class Form : std::uint8_t { None, General, Digit, Vector };
    std::optional<Form> form;
    std::optional<unsigned> vexRegister;
    const bool vex = info.RexPrefix == 0 && (lead == 0xc4 || lead == 0xc5 || lead == 0x62 || xopPrefixed(code, insn));
    const auto op = info.Opcode;
    if (vex && vexVectors && (lead == 0xc5 || lead == 0xc4) && at + (lead == 0xc5 ? 2u : 3u) < insn.Length &&
        (lead == 0xc5 || (code[at + 1] & 0x1fu) == 1) && vectorOpcode(code[at + (lead == 0xc5 ? 2 : 3)])) {
        form = Form::Vector;
    } else if (vex) {
        if (lead != 0xc4 || at + 3 >= insn.Length) return Analysis::Unknown;
        const auto map = code[at + 1] & 0x1fu;
        const auto vexOp = code[at + 3];
        const auto vvvv = (~code[at + 2] >> 3) & 0xfu;
        const bool vvvvUsed = map == 2 && (vexOp == 0xf2 || vexOp == 0xf3 || vexOp == 0xf5 || vexOp == 0xf6 || vexOp == 0xf7);
        if (map == 2 && (vexOp == 0xf2 || vexOp == 0xf5 || vexOp == 0xf6 || vexOp == 0xf7)) form = Form::General;
        else if (map == 2 && vexOp == 0xf3 && digit >= 1 && digit <= 3) form = Form::Digit;
        else if (map == 3 && vexOp == 0xf0) form = Form::General;
        else return Analysis::Unknown;
        if (vvvvUsed) vexRegister = vvvv;
    } else if (!info.IsTwoByteOpcode) {
        if (op <= 0x3d && (op & 7) <= 3) form = Form::General;
        else if (op <= 0x3d && ((op & 7) == 4 || (op & 7) == 5)) form = Form::None;
        else if (op == 0x63 || op == 0x69 || op == 0x6b || (op >= 0x84 && op <= 0x8b) || op == 0x8d) form = Form::General;
        else if (op == 0x80 || op == 0x81 || op == 0x83 || op == 0xc0 || op == 0xc1 || (op >= 0xd0 && op <= 0xd3) || op == 0xf6 || op == 0xf7) form = Form::Digit;
        else if ((op == 0xc6 || op == 0xc7) && digit == 0) form = Form::Digit;
        else if ((op == 0xfe || op == 0xff) && digit <= 1) form = Form::Digit;
        else if ((op >= 0x70 && op <= 0x7f) || op == 0xeb || op == 0xe9 || op == 0x90 || op == 0x98 || op == 0x99 || op == 0xa8 || op == 0xa9) form = Form::None;
        else if (op >= 0xb0 && op <= 0xbf) return frameRegister((op & 7u) | (b ? 8u : 0u)) ? Analysis::FrameHazard : Analysis::Understood;
        else if (op >= 0x91 && op <= 0x97) return frameRegister((op & 7u) | (b ? 8u : 0u)) ? Analysis::FrameHazard : Analysis::Unknown;
        else if (op >= 0x50 && op <= 0x5f) return frameRegister((op & 7u) | (b ? 8u : 0u)) ? Analysis::FrameHazard : Analysis::Unknown;
    } else {
        if ((op >= 0x40 && op <= 0x4f) || op == 0xaf || op == 0xb6 || op == 0xb7 || op == 0xbe || op == 0xbf) form = Form::General;
        else if ((op >= 0x90 && op <= 0x9f) || (op == 0x1f && digit == 0)) form = Form::Digit;
        else if (op >= 0x80 && op <= 0x8f) form = Form::None;
        else if (vectorOpcode(op)) form = Form::Vector;
    }
    if (!form) return Analysis::Unknown;
    if (*form == Form::None) return !info.HasModRm && (info.FlowKind == ControlFlowKind::Sequential || info.HasBranchTarget) ? Analysis::Understood : Analysis::Unknown;
    if (!info.HasModRm || info.ModRmOffset == 0 || info.ModRmOffset >= insn.Length) return Analysis::Unknown;
    const auto mod = static_cast<std::uint8_t>(modrm >> 6);
    const auto rm = static_cast<std::uint8_t>(modrm & 7);
    if (*form == Form::General && frameRegister(digit | (r ? 8u : 0u))) return Analysis::FrameHazard;
    if (vexRegister && frameRegister(*vexRegister)) return Analysis::FrameHazard;
    if (mod == 3) return *form == Form::Vector || !frameRegister(rm | (b ? 8u : 0u)) ? Analysis::Understood : Analysis::FrameHazard;
    if (rm == 5 && mod == 0) return Analysis::Understood;
    unsigned base = rm | (b ? 8u : 0u);
    bool indexed = false;
    if (rm == 4) {
        const auto sibAt = info.ModRmOffset + 1;
        if (sibAt >= insn.Length) return Analysis::Unknown;
        const auto sib = code[sibAt];
        const auto index = ((sib >> 3) & 7u) | (x ? 8u : 0u);
        if (index != kRegRsp) {
            if (frameRegister(index)) return Analysis::FrameHazard;
            indexed = true;
        }
        base = (sib & 7u) | (b ? 8u : 0u);
        if (mod == 0 && (sib & 7u) == 5) return Analysis::Understood;
    }
    const bool takesAddress = !vex && !info.IsTwoByteOpcode && op == 0x8d;
    if (base == kRegRsp) return rspBaseAllowed && !indexed && !takesAddress ? Analysis::Understood : Analysis::FrameHazard;
    if (base == kRegRbp && (indexed || takesAddress)) return Analysis::FrameHazard;
    return Analysis::Understood;
}

bool analyzedForAllocation(const std::uint8_t* code, const Insn& insn, const bool rspBaseAllowed = false, const bool vexVectors = false) {
    const auto at = insn.Info.OpcodeOffset;
    if (insn.Length == 2 && at == 0 && code[0] == 0xcd && (code[1] == 0x41 || code[1] == 0x29)) return true;
    return analyzeForFrame(code, insn, rspBaseAllowed, vexVectors) == Analysis::Understood;
}

bool twoByteRegisterIsGeneral(const std::uint8_t op) {
    return (op >= 0x40 && op <= 0x4f) || op == 0xa3 || op == 0xa4 || op == 0xa5 || op == 0xab || op == 0xac || op == 0xad || op == 0xaf || op == 0xb0 || op == 0xb1 || op == 0xb3 || op == 0xb6 || op == 0xb7 || op == 0xb8 || (op >= 0xbb && op <= 0xbf) || op == 0xc0 || op == 0xc1 || op == 0xc3;
}

bool twoByteRmIsGeneral(const std::uint8_t op) {
    return twoByteRegisterIsGeneral(op) || op == 0x6e || op == 0x7e || op == 0xba;
}

bool vexGeneralUsesRsp(const std::uint8_t* code, const Insn& insn, const std::uint8_t mod, const std::uint8_t reg, const std::uint8_t rm, const bool r, const bool b) {
    const auto at = insn.Info.OpcodeOffset;
    const auto lead = code[at];
    if ((lead != 0xc4 && lead != 0xc5) || at + 3 >= insn.Length) return false;
    const auto map = lead == 0xc5 ? std::uint8_t{1} : static_cast<std::uint8_t>(code[at + 1] & 0x1f);
    const auto vvvv = static_cast<std::uint8_t>((~code[lead == 0xc5 ? at + 1 : at + 2] >> 3) & 0xf);
    const auto op = code[lead == 0xc5 ? at + 2 : at + 3];
    const bool general = (map == 2 && (op == 0xf2 || op == 0xf3 || op == 0xf5 || op == 0xf6 || op == 0xf7)) || (map == 3 && op == 0xf0);
    return general && ((reg == kRegRsp && !r) || (mod == 3 && rm == kRegRsp && !b) || vvvv == kRegRsp);
}

MemoryUse memoryUse(const std::uint8_t* code, const Insn& insn) {
    MemoryUse use;
    const auto& info = insn.Info;
    if (!info.HasModRm || info.ModRmOffset == 0 || info.ModRmOffset >= insn.Length) return use;
    bool r = false;
    bool x = false;
    bool b = false;
    extensionBits(code, insn, r, x, b);
    const auto modrm = info.ModRmByte;
    const auto mod = static_cast<std::uint8_t>(modrm >> 6);
    const auto reg = static_cast<std::uint8_t>((modrm >> 3) & 7);
    const auto rm = static_cast<std::uint8_t>(modrm & 7);
    const bool vex = info.RexPrefix == 0 && (code[info.OpcodeOffset] == 0xc4 || code[info.OpcodeOffset] == 0xc5 || code[info.OpcodeOffset] == 0x62 || xopPrefixed(code, insn));
    if (!vex && !info.IsTwoByteOpcode && reg == kRegRsp && !r) {
        const auto op = info.Opcode;
        if ((op <= 0x3f && (op & 7) <= 3) || (op >= 0x84 && op <= 0x8b) || op == 0x8d) use.UsesRsp = use.RegisterRsp = true;
    }
    if (!vex && info.IsTwoByteOpcode && ((reg == kRegRsp && !r && twoByteRegisterIsGeneral(info.Opcode)) || (mod == 3 && rm == kRegRsp && !b && twoByteRmIsGeneral(info.Opcode)))) use.UsesRsp = use.RegisterRsp = true;
    if (vex && vexGeneralUsesRsp(code, insn, mod, reg, rm, r, b)) use.UsesRsp = use.RegisterRsp = true;
    if (mod == 3) {
        const bool generalPurpose = !vex && !info.IsTwoByteOpcode && !(info.Opcode >= 0xd8 && info.Opcode <= 0xdf);
        if (generalPurpose && rm == kRegRsp && !b) use.UsesRsp = use.RegisterRsp = true;
        return use;
    }
    std::size_t at = info.ModRmOffset + 1;
    std::uint8_t base = rm;
    bool baseExtended = b;
    bool indexed = false;
    if (rm == 4) {
        if (at >= insn.Length) return use;
        const auto sib = code[at++];
        base = sib & 7;
        indexed = ((sib >> 3) & 7) != 4 || x;
        if (mod == 0 && base == 5) return use;
    } else if (mod == 0 && rm == 5) {
        return use;
    }
    std::int64_t disp = 0;
    if (mod == 1 && at < insn.Length) disp = static_cast<std::int8_t>(code[at]);
    if (mod == 2 && at + 4 <= insn.Length) {
        std::int32_t value = 0;
        std::memcpy(&value, code + at, 4);
        disp = value;
    }
    if (base == kRegRsp && !baseExtended) {
        use.UsesRsp = true;
        use.RspBased = true;
        use.RspDisp = disp;
        use.Mod = mod;
        use.DispOffset = at;
        use.Indexed = indexed;
        use.Address = !vex && !info.IsTwoByteOpcode && info.Opcode == 0x8d;
    }
    if (base == kRegRbp && !baseExtended && mod != 0) {
        use.RbpBased = true;
        use.RbpDisp = disp;
        use.Indexed = indexed;
        use.Address = !vex && !info.IsTwoByteOpcode && info.Opcode == 0x8d;
    }
    return use;
}

bool vexEncoded(const std::uint8_t* code, const Insn& insn) {
    const auto lead = code[insn.Info.OpcodeOffset];
    return insn.Info.RexPrefix == 0 && (lead == 0xc4 || lead == 0xc5 || lead == 0x62 || xopPrefixed(code, insn));
}

bool usesRspOutsideOperands(const std::uint8_t* code, const Insn& insn) {
    const auto& info = insn.Info;
    const auto lead = code[info.OpcodeOffset];
    if (lead == 0xc8 || lead == 0xc9) return true;
    if (vexEncoded(code, insn)) {
        const auto map = lead == 0xc5 ? std::uint8_t{1} : static_cast<std::uint8_t>(code[info.OpcodeOffset + 1] & 0x1f);
        const auto op = code[info.OpcodeOffset + (lead == 0xc5 ? 2 : 3)];
        return lead == 0xc4 && map == 2 && op >= 0x90 && op <= 0x93 && memoryUse(code, insn).RspBased;
    }
    const bool b = (info.RexPrefix & 1) != 0;
    const bool r = (info.RexPrefix & 4) != 0;
    const auto reg = static_cast<std::uint8_t>((info.ModRmByte >> 3) & 7);
    const auto mod = static_cast<std::uint8_t>(info.ModRmByte >> 6);
    const auto rm = static_cast<std::uint8_t>(info.ModRmByte & 7);
    if (info.IsTwoByteOpcode) return (info.Opcode == 0xc8 + kRegRsp && !b) || (info.Opcode >= 0x90 && info.Opcode <= 0x9f && info.HasModRm && mod == 3 && rm == kRegRsp && !b);
    if (info.Opcode == 0xb0 + kRegRsp && info.RexPrefix != 0 && !b) return true;
    if ((info.Opcode == 0x90 + kRegRsp || info.Opcode == 0xb8 + kRegRsp) && !b) return true;
    return (info.Opcode == 0x63 || info.Opcode == 0x69 || info.Opcode == 0x6b) && info.HasModRm && reg == kRegRsp && !r;
}

std::optional<std::vector<std::uint8_t>> movedDisplacement(const MemoryUse& use, const std::int64_t shift) {
    const auto moved = use.RspDisp + shift;
    if (use.Mod == 1 && moved <= std::numeric_limits<std::int8_t>::max()) return std::vector<std::uint8_t>{static_cast<std::uint8_t>(moved)};
    if (use.Mod == 2 && moved <= std::numeric_limits<std::int32_t>::max()) {
        const auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>(moved));
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> withDisplacement(const std::uint8_t* code, const Insn& insn, const MemoryUse& use, const std::int64_t disp) {
    if (disp < std::numeric_limits<std::int32_t>::min() || disp > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
    const std::size_t oldBytes = use.Mod == 1 ? 1 : use.Mod == 2 ? 4 : 0;
    if (use.DispOffset + oldBytes > insn.Length) return std::nullopt;
    const bool narrow = disp >= std::numeric_limits<std::int8_t>::min() && disp <= std::numeric_limits<std::int8_t>::max();
    std::vector<std::uint8_t> bytes(code, code + use.DispOffset);
    bytes[insn.Info.ModRmOffset] = static_cast<std::uint8_t>((bytes[insn.Info.ModRmOffset] & 0x3f) | (narrow ? 0x40 : 0x80));
    const auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>(disp));
    for (std::size_t index = 0; index < (narrow ? 1u : 4u); ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
    bytes.insert(bytes.end(), code + use.DispOffset + oldBytes, code + insn.Length);
    return bytes;
}

struct OperandStub {
    std::size_t First;
    std::size_t Last;
    std::size_t Begin;
    std::size_t End;
    std::vector<std::uint8_t> Body;
    std::size_t Operands = 0;
};

struct ExitSite {
    std::size_t First;
    std::size_t Tail;
    std::size_t Begin;
    std::size_t TailBegin;
    std::size_t End;
    std::vector<std::uint8_t> Lead;
    std::size_t Operands = 0;
    std::vector<std::pair<std::size_t, std::size_t>> Branches;
};

std::optional<std::uint8_t> conditionCode(const std::uint8_t* code, const Insn& insn) {
    const auto& info = insn.Info;
    if (info.FlowKind != ControlFlowKind::ConditionalBranch || !info.HasBranchTarget || info.HasRipRelativeDisp || info.TruncatesBranchTarget) return std::nullopt;
    for (std::size_t prefix = 0; prefix < info.OpcodeOffset; ++prefix)
        if (code[prefix] != 0x2e && code[prefix] != 0x3e && (code[prefix] & 0xf0) != 0x40) return std::nullopt;
    if (!info.IsTwoByteOpcode && info.Opcode >= 0x70 && info.Opcode <= 0x7f) return static_cast<std::uint8_t>(info.Opcode & 0xf);
    if (info.IsTwoByteOpcode && info.Opcode >= 0x80 && info.Opcode <= 0x8f) return static_cast<std::uint8_t>(info.Opcode & 0xf);
    return std::nullopt;
}

void frameSegment(
    const std::vector<std::uint8_t>& fileBytes,
    const Domain::ProgramHeader& ph,
    const std::vector<RedZoneFunction>& functions,
    const std::vector<TrampolineSite>& existing,
    std::vector<TrampolineSite>& sites,
    std::vector<RedZoneDisplacement>& displacements,
    RedZoneFrameReport& report,
    const bool shrinkWrapped,
    const bool countFramePointerAllocation,
    const bool suspectFramePointerLeaves,
    const bool stubJumpsStayInBody,
    const bool exitBranches
) {
    const auto segOffset = static_cast<std::size_t>(ph.Offset);
    const auto segSize = static_cast<std::size_t>(ph.FileSize);
    if (segOffset > fileBytes.size() || segSize > fileBytes.size() - segOffset) return;
    const std::vector<std::uint8_t> seg(fileBytes.begin() + static_cast<std::ptrdiff_t>(segOffset), fileBytes.begin() + static_cast<std::ptrdiff_t>(segOffset + segSize));
    const auto scanner = MakeInstructionScanner();
    const X64InstructionDecoder decoder;

    std::set<std::size_t> targets;
    std::vector<BranchSite> branchSites;
    for (const auto& match : scanner->ScanCodeSection(seg, 0, seg.size())) {
        const auto info = decoder.DecodeInstruction(seg.data() + match.Offset, match.Length);
        if (!info.HasBranchTarget || info.HasRipRelativeDisp) continue;
        const auto target = static_cast<std::int64_t>(match.Offset + match.Length) + info.BranchDisp;
        if (target < 0 || static_cast<std::size_t>(target) >= seg.size()) continue;
        branchSites.push_back({static_cast<std::size_t>(target), static_cast<std::size_t>(match.Offset), info.FlowKind == ControlFlowKind::Call});
        if (info.FlowKind != ControlFlowKind::Call) targets.insert(static_cast<std::size_t>(target));
    }
    std::sort(branchSites.begin(), branchSites.end(), [](const BranchSite& left, const BranchSite& right) { return left.Target < right.Target; });
    std::vector<std::pair<std::size_t, std::size_t>> taken;
    for (const auto& site : existing) {
        if (site.Address < ph.MappedAddress || site.Address >= ph.MappedAddress + segSize) continue;
        const auto at = static_cast<std::size_t>(site.Address - ph.MappedAddress);
        taken.emplace_back(at, at + site.Length);
    }
    const auto existingSites = taken.size();
    const auto overlapsExisting = [&](const std::size_t begin, const std::size_t end) {
        return std::any_of(taken.begin(), taken.end(), [&](const auto& range) { return begin < range.second && range.first < end; });
    };
    const auto insideExistingSite = [&](const std::size_t begin, const std::size_t end) {
        return std::any_of(taken.begin(), taken.begin() + static_cast<std::ptrdiff_t>(existingSites), [&](const auto& range) { return range.first <= begin && end <= range.second; });
    };
    const auto targetInside = [&](const std::size_t begin, const std::size_t end) {
        const auto hit = targets.upper_bound(begin);
        return hit != targets.end() && *hit < end;
    };
    const auto frameShrinkWrapped = [&](const RedZoneFunction& function, const std::vector<Insn>& body, const std::size_t start, const std::size_t limit) -> const char* {
        const auto at = [&](const std::size_t k) { return seg.data() + body[k].Offset; };
        const auto isRet = [&](const std::size_t k) { return body[k].Length == 1 && at(k)[0] == 0xc3; };
        std::size_t first = 0;
        while (first < body.size() && !pushedRegister(at(first), body[first])) ++first;
        if (first == 0 || first == body.size()) return "shrink-wrapped: no push run after the entry";
        std::size_t last = first;
        std::vector<std::uint8_t> pushed;
        while (last < body.size())
            if (const auto reg = pushedRegister(at(last), body[last])) { pushed.push_back(*reg); ++last; } else break;
        const auto pushBegin = body[first].Offset;
        const auto pushEnd = body[last - 1].Offset + body[last - 1].Length;
        const auto lastFlow = body.back().Info.FlowKind;
        if (lastFlow != ControlFlowKind::Return && lastFlow != ControlFlowKind::UnconditionalJump) return "shrink-wrapped: falls through past its end";
        std::map<std::size_t, std::size_t> index;
        for (std::size_t k = 0; k < body.size(); ++k) index.emplace(body[k].Offset, k);
        for (std::size_t k = 0; k < body.size(); ++k)
            if (usesRspOutsideOperands(at(k), body[k])) return "shrink-wrapped: other use of rsp";
        const auto branchTarget = [&](const std::size_t k) -> std::optional<std::size_t> {
            const auto& info = body[k].Info;
            if (!info.HasBranchTarget || info.HasRipRelativeDisp) return std::nullopt;
            const auto target = static_cast<std::int64_t>(body[k].Offset + body[k].Length) + info.BranchDisp;
            if (target < static_cast<std::int64_t>(start) || target >= static_cast<std::int64_t>(limit)) return std::nullopt;
            const auto hit = index.find(static_cast<std::size_t>(target));
            if (hit == index.end()) return std::nullopt;
            return hit->second;
        };
        for (std::size_t k = 0; k < first; ++k) {
            const auto flow = body[k].Info.FlowKind;
            if (pushesOrPops(at(k), body[k]) || memoryUse(at(k), body[k]).UsesRsp || rspAdjustment(at(k), body[k])) return "shrink-wrapped: rsp used before the push run";
            if (flow == ControlFlowKind::IndirectJump) return "shrink-wrapped: indirect jump";
            if (flow == ControlFlowKind::Return && !isRet(k)) return "shrink-wrapped: ret with an immediate";
            if (flow == ControlFlowKind::UnconditionalJump || flow == ControlFlowKind::ConditionalBranch) {
                const auto target = branchTarget(k);
                if (!target) return "shrink-wrapped: branch leaves the function";
                if (*target > first && !isRet(*target)) return "shrink-wrapped: early branch past the push run";
            }
        }
        struct Exit {
            std::size_t First;
            std::size_t Ret;
        };
        std::vector<Exit> exits;
        std::set<std::size_t> popped;
        std::int64_t depth = 0;
        bool wholeRedZone = false;
        for (std::size_t k = last; k < body.size(); ++k) {
            const auto& insn = body[k];
            const auto flow = insn.Info.FlowKind;
            const auto use = memoryUse(at(k), insn);
            if (use.RegisterRsp || rspAdjustment(at(k), insn)) return "shrink-wrapped: other use of rsp";
            if (use.RspBased && at(k)[insn.Info.OpcodeOffset] == 0x62) return "shrink-wrapped: rsp operand that cannot move with rsp";
            if (use.RspBased) {
                if (use.RspDisp < 0) depth = std::max(depth, -use.RspDisp);
                if (use.Indexed || (use.Address && use.RspDisp < 0)) wholeRedZone = true;
            }
            if (flow == ControlFlowKind::IndirectJump) return "shrink-wrapped: indirect jump";
            if (flow == ControlFlowKind::UnconditionalJump || flow == ControlFlowKind::ConditionalBranch) {
                const auto target = branchTarget(k);
                if (!target) return "shrink-wrapped: branch leaves the function";
                if (*target < last) return "shrink-wrapped: branch back before the push run";
            }
            if (flow != ControlFlowKind::Return) continue;
            if (!isRet(k)) return "shrink-wrapped: ret with an immediate";
            std::size_t q = k;
            std::vector<std::uint8_t> pops;
            while (q > last && pops.size() < pushed.size())
                if (const auto reg = poppedRegister(at(q - 1), body[q - 1])) { pops.push_back(*reg); --q; } else break;
            if (pops != pushed) return "shrink-wrapped: exit does not pop the push run";
            for (auto j = q; j < k; ++j) popped.insert(j);
            exits.push_back({q, k});
        }
        if (exits.empty()) return "shrink-wrapped: no ret exit";
        for (std::size_t k = last; k < body.size(); ++k)
            if (!popped.contains(k) && pushesOrPops(at(k), body[k])) return "shrink-wrapped: push or pop inside the body";
        if (enteredFromOutside(branchSites, start, limit, false)) return "shrink-wrapped: branch from outside into the body";
        for (std::size_t k = 0; k < body.size(); ++k) {
            const auto target = branchTarget(k);
            if (!target) continue;
            for (const auto& exit : exits) {
                if (*target > exit.First && *target < exit.Ret) return "shrink-wrapped: branch into an exit's pops";
                if (*target == exit.Ret && k >= last) return "shrink-wrapped: branch to a ret past the pops";
            }
        }
        const auto shift = wholeRedZone ? kRedZoneBytes : std::min(kRedZoneBytes, (depth + kStackAlignment - 1) / kStackAlignment * kStackAlignment);
        if (shift == 0) return "shrink-wrapped: no rsp operand below the push run";

        auto entry = std::vector<std::uint8_t>(seg.begin() + static_cast<std::ptrdiff_t>(pushBegin), seg.begin() + static_cast<std::ptrdiff_t>(pushEnd));
        const auto lower = lowerRsp(shift);
        entry.insert(entry.end(), lower.begin(), lower.end());
        auto entryEnd = pushEnd;
        auto entryLast = last;
        while (entryEnd - pushBegin < kJumpBytes && entryLast < body.size()) {
            const auto& covered = body[entryLast];
            if (covered.Offset != entryEnd || covered.Info.FlowKind != ControlFlowKind::Sequential || covered.Info.HasRipRelativeDisp || popped.contains(entryLast)) break;
            const auto use = memoryUse(at(entryLast), covered);
            const auto bytes = use.RspBased ? withDisplacement(at(entryLast), covered, use, use.RspDisp + shift) : std::vector<std::uint8_t>(at(entryLast), at(entryLast) + covered.Length);
            if (!bytes) break;
            entry.insert(entry.end(), bytes->begin(), bytes->end());
            entryEnd += covered.Length;
            ++entryLast;
        }
        if (entryEnd - pushBegin < kJumpBytes) return "shrink-wrapped: push run shorter than a jump";
        if (targetInside(pushBegin, entryEnd)) return "shrink-wrapped: branch into the push run";
        if (overlapsExisting(pushBegin, entryEnd)) return "shrink-wrapped: overlaps another stub";

        struct Site {
            std::size_t Begin;
            std::size_t End;
            std::vector<std::uint8_t> Code;
        };
        std::vector<Site> exitSites;
        for (const auto& exit : exits) {
            const auto begin = body[exit.First].Offset;
            auto end = begin;
            auto k = exit.First;
            while (end - begin < kJumpBytes && k < exit.Ret) end += body[k++].Length;
            if (end - begin < kJumpBytes && !targets.contains(body[exit.Ret].Offset)) end += body[exit.Ret].Length;
            if (end - begin < kJumpBytes) return "shrink-wrapped: exit shorter than a jump";
            if (targetInside(begin, end)) return "shrink-wrapped: branch into an exit";
            if (overlapsExisting(begin, end) || (begin < entryEnd && pushBegin < end)) return "shrink-wrapped: overlaps another stub";
            auto code = raiseRsp(shift);
            code.insert(code.end(), seg.begin() + static_cast<std::ptrdiff_t>(begin), seg.begin() + static_cast<std::ptrdiff_t>(end));
            exitSites.push_back({begin, end, std::move(code)});
        }
        std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> moves;
        for (auto k = entryLast; k < body.size(); ++k) {
            const auto use = memoryUse(at(k), body[k]);
            if (!use.RspBased) continue;
            if (overlapsExisting(body[k].Offset, body[k].Offset + body[k].Length)) return "shrink-wrapped: overlaps another stub";
            auto moved = movedDisplacement(use, shift);
            if (!moved) return "shrink-wrapped: rsp operand that cannot move";
            moves.emplace_back(body[k].Offset + use.DispOffset, std::move(*moved));
        }

        const auto addSite = [&](const std::size_t begin, const std::size_t end, std::vector<std::uint8_t> code) {
            TrampolineSite site;
            site.Offset = ph.Offset + begin;
            site.Address = ph.MappedAddress + begin;
            site.Length = end - begin;
            site.OriginalBytes.assign(seg.begin() + static_cast<std::ptrdiff_t>(begin), seg.begin() + static_cast<std::ptrdiff_t>(end));
            site.ReturnBranchOffset = code.size();
            code.insert(code.end(), std::begin(kJumpBack), std::end(kJumpBack));
            site.Body = std::move(code);
            taken.emplace_back(begin, end);
            sites.push_back(std::move(site));
        };
        addSite(pushBegin, entryEnd, std::move(entry));
        for (auto& site : exitSites) addSite(site.Begin, site.End, std::move(site.Code));
        for (auto& [offset, bytes] : moves) displacements.push_back({ph.Offset + offset, std::move(bytes)});
        ++report.Framed;
        ++report.FramedWithoutFramePointer;
        ++report.ShrinkWrapped;
        report.Exits += exits.size();
        report.MovedOperands += moves.size();
        report.Leaves.push_back({function.Begin, function.End, {}});
        return nullptr;
    };
    for (const auto& function : functions) {
        if (function.Begin < ph.MappedAddress || function.End <= function.Begin || function.End - ph.MappedAddress > segSize) continue;
        const auto start = static_cast<std::size_t>(function.Begin - ph.MappedAddress);
        const auto limit = static_cast<std::size_t>(function.End - ph.MappedAddress);
        ++report.Functions;
        const auto skip = [&](const char* reason) {
            ++report.Skipped[reason];
            report.Leaves.push_back({function.Begin, function.End, reason});
        };

        const std::vector<std::uint8_t> code(seg.begin() + static_cast<std::ptrdiff_t>(start), seg.begin() + static_cast<std::ptrdiff_t>(limit));
        std::vector<Insn> body;
        const auto matches = scanner->ScanCodeSection(code, 0, code.size());
        const auto int3 = [&](const auto& match) { return code[match.Offset] == 0xcc && match.Length == 1; };
        for (std::size_t m = 0; m < matches.size(); ++m) {
            if (int3(matches[m]) && std::all_of(matches.begin() + static_cast<std::ptrdiff_t>(m), matches.end(), int3)) break;
            body.push_back({start + static_cast<std::size_t>(matches[m].Offset), matches[m].Length, decoder.DecodeInstruction(code.data() + matches[m].Offset, matches[m].Length)});
        }
        bool contiguous = !body.empty() && body.front().Offset == start;
        for (std::size_t k = 1; k < body.size() && contiguous; ++k) contiguous = body[k - 1].Offset + body[k - 1].Length == body[k].Offset;
        if (contiguous) contiguous = std::all_of(code.begin() + static_cast<std::ptrdiff_t>(body.back().Offset + body.back().Length - start), code.end(), [](std::uint8_t byte) { return byte == 0xcc; });
        if (body.size() < 3) continue;
        const auto at = [&](const std::size_t k) { return seg.data() + body[k].Offset; };
        const bool rbpPrologue = body[0].Length == 1 && at(0)[0] == 0x55 && body[1].Length == 3 && std::memcmp(at(1), "\x48\x89\xe5", 3) == 0;
        std::vector<std::uint8_t> pushed;
        std::size_t p = rbpPrologue ? 2 : 0;
        while (p < body.size())
            if (const auto reg = pushedRegister(at(p), body[p])) { pushed.push_back(*reg); ++p; } else break;
        std::int64_t allocation = 0;
        if (!rbpPrologue && p < body.size())
            if (const auto adjustment = rspAdjustment(at(p), body[p]); adjustment && *adjustment < 0) { allocation = -*adjustment; ++p; }
        std::vector<std::uint8_t> expected(pushed.begin(), pushed.end());
        if (rbpPrologue) expected.insert(expected.begin(), kRegRbp);
        const auto popsBefore = [&](const std::size_t k) -> std::optional<std::size_t> {
            std::size_t q = k;
            std::vector<std::uint8_t> popped;
            while (q > p && popped.size() < expected.size())
                if (const auto reg = poppedRegister(at(q - 1), body[q - 1])) { popped.push_back(*reg); --q; } else break;
            if (popped != expected) return std::nullopt;
            return q;
        };
        std::set<std::size_t> frees;
        if (allocation > 0)
            for (std::size_t k = p; k < body.size(); ++k)
                if (body[k].Info.FlowKind == ControlFlowKind::Return)
                    if (const auto q = popsBefore(k); q && *q > p && rspAdjustment(at(*q - 1), body[*q - 1]) == allocation) frees.insert(*q - 1);
        std::set<std::size_t> bodyStarts;
        const bool allocatesAtP = p < body.size() && body[p].Length >= 4 && at(p)[2] == 0xec && rspAdjustment(at(p), body[p]).value_or(0) < 0;
        for (std::size_t k = allocatesAtP ? p + 1 : p; k < body.size(); ++k) bodyStarts.insert(body[k].Offset);
        const auto branchStaysInBody = [&](const std::size_t k) {
            const auto& insn = body[k];
            if (!insn.Info.HasBranchTarget) return true;
            const auto target = static_cast<std::int64_t>(insn.Offset + insn.Length) + insn.Info.BranchDisp;
            return !insn.Info.HasRipRelativeDisp && target >= 0 && bodyStarts.contains(static_cast<std::size_t>(target));
        };
        const auto endsWithoutFallThrough = !body.empty() && (body.back().Info.FlowKind == ControlFlowKind::Return || body.back().Info.FlowKind == ControlFlowKind::UnconditionalJump);
        std::int64_t framePointerAllocation = 0;
        if (countFramePointerAllocation && rbpPrologue && p < body.size() && body[p].Length >= 4 && at(p)[2] == 0xec)
            if (const auto adjustment = rspAdjustment(at(p), body[p]); adjustment && *adjustment < 0) {
                const auto allocated = -*adjustment;
                const auto pushedBytes = static_cast<std::int64_t>(8 * pushed.size());
                std::set<std::size_t> epilogue;
                bool clean = true;
                std::size_t returns = 0;
                for (std::size_t k = p + 1; k < body.size() && clean; ++k) {
                    if (body[k].Info.FlowKind != ControlFlowKind::Return) continue;
                    ++returns;
                    const auto q = popsBefore(k);
                    if (!q || *q <= p + 1 || body[k].Length != 1 || at(k)[0] != 0xc3) { clean = false; break; }
                    const auto* freeing = at(*q - 1);
                    const auto& freeInsn = body[*q - 1];
                    const bool added = freeInsn.Length >= 4 && freeing[2] == 0xc4 && rspAdjustment(freeing, freeInsn) == allocated;
                    const bool restored = freeInsn.Length == 4 && std::memcmp(freeing, "\x48\x8d\x65", 3) == 0 && static_cast<std::int8_t>(freeing[3]) == -pushedBytes;
                    if ((!added && !restored) || targetInside(freeInsn.Offset, body[k].Offset + body[k].Length)) clean = false;
                    for (auto j = *q - 1; j <= k; ++j) epilogue.insert(j);
                }
                if (returns == 0 || !endsWithoutFallThrough || !contiguous) clean = false;
                for (std::size_t k = p + 1; k < body.size() && clean; ++k) {
                    if (epilogue.contains(k)) continue;
                    const auto& insn = body[k];
                    if (stubJumpsStayInBody && insideExistingSite(insn.Offset, insn.Offset + insn.Length)) continue;
                    const auto use = memoryUse(at(k), insn);
                    if (!analyzedForAllocation(at(k), insn, false, stubJumpsStayInBody) || use.UsesRsp || pushesOrPops(at(k), insn) || (use.RbpBased && (use.Indexed || use.Address))) clean = false;
                    if (!branchStaysInBody(k)) clean = false;
                }
                if (clean) framePointerAllocation = allocated;
            }

        bool leaf = true;
        bool redZone = false;
        const char* reason = nullptr;
        std::vector<ExitSite> exits;
        std::map<std::size_t, std::pair<std::size_t, std::vector<std::uint8_t>>> moves;
        std::vector<std::size_t> stubbed;
        std::vector<std::size_t> stackMoves;
        const auto functionEnd = limit;
        std::int64_t depth = 0;
        bool wholeRedZone = rbpPrologue;
        for (std::size_t k = p; k < body.size(); ++k) {
            const auto use = memoryUse(at(k), body[k]);
            if (!use.RspBased) continue;
            if (use.RspDisp < 0) depth = std::max(depth, -use.RspDisp);
            if (use.Indexed || (use.Address && use.RspDisp < 0)) wholeRedZone = true;
        }
        const auto shift = wholeRedZone ? kRedZoneBytes : std::min(kRedZoneBytes, (depth + kStackAlignment - 1) / kStackAlignment * kStackAlignment);
        for (std::size_t k = p; k < body.size(); ++k) {
            const auto& insn = body[k];
            const auto* code = at(k);
            const auto flow = insn.Info.FlowKind;
            if (flow == ControlFlowKind::Call || flow == ControlFlowKind::IndirectCall) { leaf = false; break; }
            if (frees.contains(k)) continue;
            const auto use = memoryUse(code, insn);
            if (use.RbpBased && rbpPrologue && use.RbpDisp < -static_cast<std::int64_t>(8 * pushed.size()) - framePointerAllocation) redZone = true;
            if (use.RspBased && use.RspDisp < 0) redZone = true;
            if (reason != nullptr) continue;
            if (code[insn.Info.OpcodeOffset] == 0xc9 || code[insn.Info.OpcodeOffset] == 0xc8) { reason = "leave or enter"; continue; }
            if (!rbpPrologue && pushed.empty() && allocation == 0 && pushesOrPops(code, insn)) { reason = "push or pop in a leaf without pushes"; continue; }
            if (!rbpPrologue && pushesOrPops(code, insn)) stackMoves.push_back(k);
            if (rbpPrologue) {
                if (use.UsesRsp) { reason = "other use of rsp"; continue; }
            } else {
                if (use.RegisterRsp) { reason = "other use of rsp"; continue; }
                if (use.RspBased) {
                    if (code[insn.Info.OpcodeOffset] == 0x62) { reason = "rsp operand that cannot move with rsp"; continue; }
                    if (overlapsExisting(insn.Offset, insn.Offset + insn.Length)) { reason = "overlaps another stub"; continue; }
                    if (auto moved = movedDisplacement(use, shift)) moves.emplace(k, std::pair{insn.Offset + use.DispOffset, std::move(*moved)});
                    else stubbed.push_back(k);
                }
            }
            if (flow == ControlFlowKind::IndirectJump) { reason = "indirect jump"; continue; }
            if ((flow == ControlFlowKind::UnconditionalJump || flow == ControlFlowKind::ConditionalBranch) && insn.Info.HasBranchTarget) {
                const auto target = static_cast<std::int64_t>(insn.Offset + insn.Length) + insn.Info.BranchDisp;
                if (insn.Info.HasRipRelativeDisp || target < static_cast<std::int64_t>(start) || target >= static_cast<std::int64_t>(functionEnd)) { reason = "branch leaves the function"; continue; }
            }
            if (flow == ControlFlowKind::Return) {
                if (insn.Length != 1 || code[0] != 0xc3) { reason = "ret with an immediate"; continue; }
                const auto pops = popsBefore(k);
                if (!pops) { reason = "exit does not pop the prologue's registers"; continue; }
                auto q = *pops;
                if (allocation > 0) {
                    if (q == p || !frees.contains(q - 1)) { reason = "exit does not free the prologue's allocation"; continue; }
                    --q;
                }
                ExitSite exit{q, q, body[q].Offset, body[q].Offset, insn.Offset + 1, {}};
                while (exit.End - exit.Begin < kJumpBytes && exit.First > p) {
                    const auto j = exit.First - 1;
                    const auto& prior = body[j];
                    if (exitBranches && prior.Offset + prior.Length == exit.Begin && prior.Info.FlowKind == ControlFlowKind::ConditionalBranch) {
                        const auto condition = conditionCode(at(j), prior);
                        const auto target = static_cast<std::int64_t>(prior.Offset + prior.Length) + prior.Info.BranchDisp;
                        if (!condition || target <= static_cast<std::int64_t>(start) || target >= static_cast<std::int64_t>(prior.Offset) || !bodyStarts.contains(static_cast<std::size_t>(target)) || frees.contains(j)) break;
                        const std::vector<std::uint8_t> jump{0x0f, static_cast<std::uint8_t>(0x80 | *condition), 0, 0, 0, 0};
                        exit.Lead.insert(exit.Lead.begin(), jump.begin(), jump.end());
                        for (auto& branch : exit.Branches) branch.first += jump.size();
                        exit.Branches.emplace_back(0, static_cast<std::size_t>(target));
                        exit.Begin = prior.Offset;
                        exit.First = j;
                        continue;
                    }
                    if (prior.Offset + prior.Length != exit.Begin || prior.Info.FlowKind != ControlFlowKind::Sequential || prior.Info.HasRipRelativeDisp || frees.contains(j) || pushesOrPops(at(j), prior)) break;
                    const auto priorUse = memoryUse(at(j), prior);
                    const auto bytes = priorUse.RspBased ? withDisplacement(at(j), prior, priorUse, priorUse.RspDisp + shift) : std::vector<std::uint8_t>(at(j), at(j) + prior.Length);
                    if (!bytes) break;
                    exit.Lead.insert(exit.Lead.begin(), bytes->begin(), bytes->end());
                    for (auto& branch : exit.Branches) branch.first += bytes->size();
                    if (priorUse.RspBased) ++exit.Operands;
                    exit.Begin = prior.Offset;
                    exit.First = j;
                }
                if (exit.End - exit.Begin < kJumpBytes) { reason = "exit shorter than a jump"; continue; }
                if (targetInside(exit.Begin, exit.End)) { reason = exit.First == exit.Tail ? "branch into an exit" : "branch into the instructions before a short exit"; continue; }
                exits.push_back(std::move(exit));
            }
        }
        if (suspectFramePointerLeaves && leaf && !redZone && rbpPrologue) {
            const auto allocated = allocatesAtP ? -rspAdjustment(at(p), body[p]).value_or(0) : 0;
            const auto pushedBytes = static_cast<std::int64_t>(8 * pushed.size());
            std::set<std::size_t> epilogue;
            for (std::size_t k = p + 1; k < body.size(); ++k) {
                if (body[k].Info.FlowKind != ControlFlowKind::Return || body[k].Length != 1 || at(k)[0] != 0xc3) continue;
                std::optional<std::size_t> first;
                if (pushed.empty() && body[k - 1].Length == 1 && at(k - 1)[0] == 0xc9) first = k - 1;
                else if (const auto q = popsBefore(k)) {
                    first = *q;
                    if (*q > p + 1) {
                        const auto* freeing = at(*q - 1);
                        const auto& freeInsn = body[*q - 1];
                        const bool added = allocatesAtP && freeInsn.Length >= 4 && freeing[2] == 0xc4 && rspAdjustment(freeing, freeInsn) == allocated;
                        const bool restored = freeInsn.Length == 4 && std::memcmp(freeing, "\x48\x8d\x65", 3) == 0 && static_cast<std::int8_t>(freeing[3]) == -pushedBytes;
                        if (added || restored) first = *q - 1;
                        else if (allocatesAtP) first.reset();
                    } else if (allocatesAtP) {
                        first.reset();
                    }
                }
                if (!first || targetInside(body[*first].Offset, body[k].Offset + body[k].Length)) continue;
                for (auto j = *first; j <= k; ++j) epilogue.insert(j);
            }
            if (allocatesAtP && !epilogue.empty()) epilogue.insert(p);
            for (std::size_t k = p; k < body.size(); ++k) {
                if (epilogue.contains(k)) continue;
                bool hazard = body[k].Info.FlowKind == ControlFlowKind::Return || analyzeForFrame(at(k), body[k], true) != Analysis::Understood;
                if (!hazard && body[k].Info.HasBranchTarget && !branchStaysInBody(k)) {
                    const auto target = static_cast<std::int64_t>(body[k].Offset + body[k].Length) + body[k].Info.BranchDisp;
                    hazard = target >= static_cast<std::int64_t>(start) && target < static_cast<std::int64_t>(limit);
                }
                if (!hazard) continue;
                redZone = true;
                reason = "frame-pointer leaf with an instruction the framer does not prove safe";
                break;
            }
        }
        if (!leaf || !redZone) continue;
        ++report.RedZoneFunctions;
        if (shrinkWrapped && reason != nullptr && !rbpPrologue && pushed.empty() && allocation == 0 && std::any_of(body.begin(), body.end(), [&](const Insn& insn) { return pushedRegister(seg.data() + insn.Offset, insn).has_value(); })) {
            reason = frameShrinkWrapped(function, body, start, limit);
            if (reason == nullptr) continue;
        }
        const auto prologueEnd = p > 0 ? body[p - 1].Offset + body[p - 1].Length : start;
        if (reason == nullptr && std::any_of(stackMoves.begin(), stackMoves.end(), [&](const std::size_t k) { return std::none_of(exits.begin(), exits.end(), [&](const ExitSite& exit) { return exit.Begin <= body[k].Offset && body[k].Offset < exit.End; }); })) reason = "push or pop inside the body";
        auto entryEnd = prologueEnd;
        auto entryLast = p;
        std::vector<std::uint8_t> entryTail;
        std::size_t entryOperands = 0;
        while (entryEnd - start < kJumpBytes && entryLast < body.size()) {
            const auto& covered = body[entryLast];
            if (covered.Offset != entryEnd || covered.Info.FlowKind != ControlFlowKind::Sequential || covered.Info.HasRipRelativeDisp || frees.contains(entryLast) || pushesOrPops(at(entryLast), covered)) break;
            if (std::any_of(exits.begin(), exits.end(), [&](const ExitSite& exit) { return covered.Offset < exit.End && exit.Begin < covered.Offset + covered.Length; })) break;
            const auto use = memoryUse(at(entryLast), covered);
            const auto bytes = use.RspBased ? withDisplacement(at(entryLast), covered, use, use.RspDisp + shift) : std::vector<std::uint8_t>(at(entryLast), at(entryLast) + covered.Length);
            if (!bytes) break;
            entryTail.insert(entryTail.end(), bytes->begin(), bytes->end());
            if (use.RspBased) ++entryOperands;
            entryEnd += covered.Length;
            ++entryLast;
        }
        if (reason == nullptr && entryEnd - start < kJumpBytes) reason = "prologue shorter than a jump";
        if (reason == nullptr && targetInside(start, entryEnd)) reason = entryEnd == prologueEnd ? "branch into the prologue" : "branch into the instructions after a short prologue";
        if (reason == nullptr && entryEnd != prologueEnd && targets.contains(start)) reason = "branch to the start of a widened prologue";
        if (reason == nullptr && exits.empty()) reason = "no ret exit";
        if (reason == nullptr && std::any_of(exits.begin(), exits.end(), [&](const ExitSite& exit) { return exit.Begin < entryEnd; })) reason = "prologue shorter than a jump";
        if (reason == nullptr && (overlapsExisting(start, entryEnd) || std::any_of(exits.begin(), exits.end(), [&](const ExitSite& exit) { return overlapsExisting(exit.Begin, exit.End); }))) reason = "overlaps another stub";
        const auto carried = [&](const std::size_t k) {
            moves.erase(k);
            stubbed.erase(std::remove(stubbed.begin(), stubbed.end(), k), stubbed.end());
        };
        for (auto k = p; k < entryLast; ++k) carried(k);
        for (const auto& exit : exits)
            for (auto k = exit.First; k < exit.Tail; ++k) carried(k);
        std::vector<OperandStub> stubs;
        const auto overlapsSite = [&](const std::size_t begin, const std::size_t end) {
            if (begin < entryEnd && start < end) return true;
            if (std::any_of(exits.begin(), exits.end(), [&](const ExitSite& exit) { return begin < exit.End && exit.Begin < end; })) return true;
            return std::any_of(stubs.begin(), stubs.end(), [&](const OperandStub& stub) { return begin < stub.End && stub.Begin < end; });
        };
        for (const auto k : stubbed) {
            if (reason != nullptr) break;
            if (!stubs.empty() && k < stubs.back().Last) continue;
            OperandStub stub{k, k, body[k].Offset, body[k].Offset, {}};
            while (stub.Last < body.size() && stub.End - stub.Begin < kJumpBytes) {
                const auto& covered = body[stub.Last];
                if (covered.Offset != stub.End || covered.Info.FlowKind != ControlFlowKind::Sequential || covered.Info.HasRipRelativeDisp || frees.contains(stub.Last)) break;
                const auto use = memoryUse(at(stub.Last), covered);
                const auto bytes = use.RspBased ? withDisplacement(at(stub.Last), covered, use, use.RspDisp + shift) : std::vector<std::uint8_t>(at(stub.Last), at(stub.Last) + covered.Length);
                if (!bytes) break;
                stub.Body.insert(stub.Body.end(), bytes->begin(), bytes->end());
                if (use.RspBased) ++stub.Operands;
                stub.End += covered.Length;
                ++stub.Last;
            }
            while (stub.End - stub.Begin < kJumpBytes && stub.First > p) {
                const auto j = stub.First - 1;
                const auto& prior = body[j];
                if (prior.Offset + prior.Length != stub.Begin || prior.Info.FlowKind != ControlFlowKind::Sequential || prior.Info.HasRipRelativeDisp || frees.contains(j)) break;
                const auto use = memoryUse(at(j), prior);
                const auto bytes = use.RspBased ? withDisplacement(at(j), prior, use, use.RspDisp + shift) : std::vector<std::uint8_t>(at(j), at(j) + prior.Length);
                if (!bytes) break;
                stub.Body.insert(stub.Body.begin(), bytes->begin(), bytes->end());
                if (use.RspBased) ++stub.Operands;
                stub.Begin = prior.Offset;
                stub.First = j;
            }
            if (stub.End - stub.Begin < kJumpBytes || targetInside(stub.Begin, stub.End) || overlapsSite(stub.Begin, stub.End) || overlapsExisting(stub.Begin, stub.End)) reason = "rsp operand that cannot move and has no room for a stub";
            else stubs.push_back(std::move(stub));
        }
        if (reason != nullptr) { skip(reason); continue; }
        for (const auto& stub : stubs)
            for (auto k = stub.First; k < stub.Last; ++k) moves.erase(k);

        const auto addSite = [&](const std::size_t begin, const std::size_t end, std::vector<std::uint8_t> code) {
            TrampolineSite site;
            site.Offset = ph.Offset + begin;
            site.Address = ph.MappedAddress + begin;
            site.Length = end - begin;
            site.OriginalBytes.assign(seg.begin() + static_cast<std::ptrdiff_t>(begin), seg.begin() + static_cast<std::ptrdiff_t>(end));
            site.ReturnBranchOffset = code.size();
            code.insert(code.end(), std::begin(kJumpBack), std::end(kJumpBack));
            site.Body = std::move(code);
            taken.emplace_back(begin, end);
            sites.push_back(std::move(site));
        };
        const auto original = [&](const std::size_t begin, const std::size_t end) { return std::vector<std::uint8_t>(seg.begin() + static_cast<std::ptrdiff_t>(begin), seg.begin() + static_cast<std::ptrdiff_t>(end)); };
        auto entry = original(start, prologueEnd);
        const auto lower = lowerRsp(shift);
        entry.insert(entry.end(), lower.begin(), lower.end());
        entry.insert(entry.end(), entryTail.begin(), entryTail.end());
        addSite(start, entryEnd, std::move(entry));
        for (auto& exit : exits) {
            auto code = std::move(exit.Lead);
            const auto raise = raiseRsp(shift);
            code.insert(code.end(), raise.begin(), raise.end());
            const auto tail = original(exit.TailBegin, exit.End);
            code.insert(code.end(), tail.begin(), tail.end());
            addSite(exit.Begin, exit.End, std::move(code));
            for (const auto& [at, target] : exit.Branches)
                sites.back().Relocations.push_back({at + 2, at + 6, static_cast<std::int64_t>(target) - static_cast<std::int64_t>(exit.Begin)});
        }
        for (auto& stub : stubs) addSite(stub.Begin, stub.End, std::move(stub.Body));
        for (auto& [k, move] : moves) displacements.push_back({ph.Offset + move.first, std::move(move.second)});
        ++report.Framed;
        report.Leaves.push_back({function.Begin, function.End, {}});
        if (!rbpPrologue) ++report.FramedWithoutFramePointer;
        report.Exits += exits.size();
        if (entryLast != p) ++report.WidenedPrologues;
        report.WidenedOperands += entryOperands;
        for (const auto& exit : exits) {
            if (exit.First != exit.Tail) ++report.WidenedExits;
            report.WidenedOperands += exit.Operands;
        }
        report.MovedOperands += moves.size();
        report.OperandStubs += stubs.size();
        for (const auto& stub : stubs) report.StubbedOperands += stub.Operands;
    }
}

std::size_t pureStoreWidth(const std::uint8_t* code, const Insn& insn) {
    const auto& info = insn.Info;
    const auto at = info.OpcodeOffset;
    const auto modrm = info.ModRmByte;
    if (!info.HasModRm || (modrm >> 6) == 3) return 0;
    for (std::size_t prefix = 0; prefix < at; ++prefix)
        if (code[prefix] == 0x64 || code[prefix] == 0x65 || code[prefix] == 0x67) return 0;
    const auto lead = code[at];
    if (info.RexPrefix == 0 && (lead == 0xc4 || lead == 0xc5)) {
        if (lead == 0xc4 && (at + 3 >= insn.Length || (code[at + 1] & 0x1f) != 1)) return 0;
        const auto payload = code[at + (lead == 0xc5 ? 1 : 2)];
        const auto op = code[at + (lead == 0xc5 ? 2 : 3)];
        const auto pp = payload & 3u;
        const std::size_t vector = (payload & 4u) != 0 ? 32 : 16;
        if (op == 0x11) return pp == 2 ? 4 : pp == 3 ? 8 : vector;
        if (op == 0x29 || (op == 0x7f && (pp == 1 || pp == 2))) return vector;
        return 0;
    }
    if (lead == 0x62 || lead == 0x8f) return 0;
    bool operand16 = false;
    bool f3 = false;
    bool f2 = false;
    for (std::size_t prefix = 0; prefix < at; ++prefix) {
        if (code[prefix] == 0x66) operand16 = true;
        else if (code[prefix] == 0xf3) f3 = true;
        else if (code[prefix] == 0xf2) f2 = true;
    }
    const bool wide = (info.RexPrefix & 8) != 0;
    const auto op = info.Opcode;
    const auto digit = (modrm >> 3) & 7;
    if (!info.IsTwoByteOpcode) {
        if (op == 0x88 || (op == 0xc6 && digit == 0)) return 1;
        if (op == 0x89 || (op == 0xc7 && digit == 0)) return wide ? 8 : operand16 ? 2 : 4;
        return 0;
    }
    if (op == 0x38 || op == 0x3a) return 0;
    if (op == 0x11) return f3 ? 4 : f2 ? 8 : 16;
    if (op == 0x29 || (op == 0x7f && (operand16 || f3))) return 16;
    if ((op == 0x13 || op == 0x17) && !f3 && !f2) return 8;
    if (op == 0xd6 && operand16) return 8;
    return 0;
}

std::vector<RedZoneDeadRange> deadRanges(const std::vector<std::uint8_t>& seg, const std::uint64_t base, const std::vector<BranchSite>& branches, const std::size_t start, const std::size_t limit, const bool killStores, bool& tailCallExit) {
    if (enteredFromOutside(branches, start, limit, true)) return {};
    const auto scanner = MakeInstructionScanner();
    const X64InstructionDecoder decoder;
    const std::vector<std::uint8_t> code(seg.begin() + static_cast<std::ptrdiff_t>(start), seg.begin() + static_cast<std::ptrdiff_t>(limit));
    std::vector<Insn> body;
    for (const auto& match : scanner->ScanCodeSection(code, 0, code.size())) {
        if (code[match.Offset] == 0xcc && match.Length == 1) break;
        body.push_back({start + static_cast<std::size_t>(match.Offset), match.Length, decoder.DecodeInstruction(code.data() + match.Offset, match.Length)});
    }
    const auto count = body.size();
    if (count == 0) return {};
    const auto at = [&](const std::size_t k) { return seg.data() + body[k].Offset; };
    std::map<std::size_t, std::size_t> index;
    for (std::size_t k = 0; k < count; ++k) index.emplace(body[k].Offset, k);
    std::vector<std::vector<std::size_t>> next(count);
    std::vector<std::int64_t> effect(count, 0);
    std::vector<std::optional<std::int64_t>> reads(count);
    std::vector<std::optional<std::int64_t>> stores(count);
    std::vector<std::size_t> storeWidth(count, 0);
    std::vector<std::size_t> exits;
    for (std::size_t k = 0; k < count; ++k) {
        const auto& insn = body[k];
        const auto flow = insn.Info.FlowKind;
        if (killStores && flow == ControlFlowKind::IndirectJump) {
            const auto use = memoryUse(at(k), insn);
            const auto modrm = insn.Info.ModRmByte;
            const bool sibIndexed = (modrm & 7) == 4 && insn.Info.ModRmOffset + 1 < insn.Length && (((at(k)[insn.Info.ModRmOffset + 1] >> 3) & 7) != 4 || (insn.Info.RexPrefix & 2) != 0);
            const bool throughMemory = insn.Info.HasModRm && (modrm >> 6) != 3 && !sibIndexed && !use.Indexed;
            if (!throughMemory || use.RspBased || use.UsesRsp || usesRspOutsideOperands(at(k), insn)) return {};
            exits.push_back(k);
            continue;
        }
        if (flow == ControlFlowKind::Call || flow == ControlFlowKind::IndirectCall || flow == ControlFlowKind::IndirectJump) return {};
        if (const auto adjustment = rspAdjustment(at(k), insn)) {
            effect[k] = *adjustment;
        } else {
            const auto use = memoryUse(at(k), insn);
            const bool bitIndexed = insn.Info.IsTwoByteOpcode && (insn.Info.Opcode == 0xa3 || insn.Info.Opcode == 0xab || insn.Info.Opcode == 0xb3 || insn.Info.Opcode == 0xbb);
            if (usesRspOutsideOperands(at(k), insn) || use.RegisterRsp || (use.RspBased && (use.Indexed || use.Address || bitIndexed || at(k)[insn.Info.OpcodeOffset] == 0x62))) return {};
            if (pushedRegister(at(k), insn)) effect[k] = -8;
            else if (poppedRegister(at(k), insn)) { effect[k] = 8; reads[k] = 0; }
            else if (pushesOrPops(at(k), insn)) return {};
            const auto width = killStores && use.RspBased ? pureStoreWidth(at(k), insn) : 0;
            if (width != 0) {
                stores[k] = use.RspDisp;
                storeWidth[k] = width;
            } else if (use.RspBased) {
                reads[k] = reads[k] ? std::min(*reads[k], use.RspDisp) : use.RspDisp;
            }
        }
        if (flow == ControlFlowKind::Return) { reads[k] = reads[k] ? std::min(*reads[k], std::int64_t{0}) : 0; continue; }
        if (flow == ControlFlowKind::UnconditionalJump || flow == ControlFlowKind::ConditionalBranch) {
            if (!insn.Info.HasBranchTarget || insn.Info.HasRipRelativeDisp) return {};
            const auto target = static_cast<std::int64_t>(insn.Offset + insn.Length) + insn.Info.BranchDisp;
            if (target < static_cast<std::int64_t>(start) || target >= static_cast<std::int64_t>(limit)) return {};
            const auto hit = index.find(static_cast<std::size_t>(target));
            if (hit == index.end()) return {};
            next[k].push_back(hit->second);
            if (flow == ControlFlowKind::UnconditionalJump) continue;
        } else if (flow != ControlFlowKind::Sequential) {
            return {};
        }
        if (k + 1 == count || body[k + 1].Offset != insn.Offset + insn.Length) return {};
        next[k].push_back(k + 1);
    }
    std::vector<std::optional<std::int64_t>> depth(count);
    depth[0] = 0;
    std::vector<std::size_t> pending{0};
    while (!pending.empty()) {
        const auto k = pending.back();
        pending.pop_back();
        for (const auto s : next[k]) {
            const auto value = *depth[k] + effect[k];
            if (!depth[s]) { depth[s] = value; pending.push_back(s); }
            else if (*depth[s] != value) return {};
        }
    }
    for (const auto k : exits)
        if (depth[k] && *depth[k] != 0) return {};
    if (!exits.empty())
        for (std::size_t k = 0; k < count; ++k)
            if (!depth[k] && !(body[k].Info.FlowKind == ControlFlowKind::Sequential && (at(k)[body[k].Info.OpcodeOffset] == 0x90 || (body[k].Info.IsTwoByteOpcode && body[k].Info.Opcode == 0x1f)))) return {};
    if (killStores) {
        constexpr std::int64_t kSpan = 1024;
        using Bytes = std::bitset<static_cast<std::size_t>(kSpan)>;
        const auto mark = [](Bytes& bytes, const std::int64_t from, const std::int64_t to, const bool value) {
            for (auto offset = std::max(from, -kSpan); offset < std::min(to, std::int64_t{0}); ++offset) bytes.set(static_cast<std::size_t>(offset + kSpan), value);
        };
        for (std::size_t k = 0; k < count; ++k)
            if (depth[k] && (*depth[k] > 0 || *depth[k] < -kSpan / 2 || (reads[k] && *depth[k] + *reads[k] < -kSpan / 2) || (stores[k] && *depth[k] + *stores[k] < -kSpan / 2))) return {};
        std::vector<Bytes> live(count);
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t k = count; k-- > 0;) {
                if (!depth[k]) continue;
                Bytes in;
                for (const auto s : next[k]) in |= live[s];
                if (stores[k]) mark(in, *depth[k] + *stores[k], *depth[k] + *stores[k] + static_cast<std::int64_t>(storeWidth[k]), false);
                if (reads[k]) mark(in, *depth[k] + *reads[k], 0, true);
                if (in != live[k]) { live[k] = in; changed = true; }
            }
        }
        tailCallExit = !exits.empty();
        std::vector<RedZoneDeadRange> dead;
        for (std::size_t k = 0; k < count; ++k) {
            if (!depth[k]) continue;
            bool below = false;
            for (auto offset = -kSpan; offset < *depth[k] && !below; ++offset) below = live[k].test(static_cast<std::size_t>(offset + kSpan));
            if (below) continue;
            const auto begin = base + body[k].Offset;
            const auto end = begin + body[k].Length;
            if (!dead.empty() && dead.back().End == begin) dead.back().End = end;
            else dead.push_back({begin, end});
        }
        return dead;
    }
    constexpr auto kNone = std::numeric_limits<std::int64_t>::max();
    std::vector<std::int64_t> lowest(count, kNone);
    for (std::size_t k = 0; k < count; ++k)
        if (depth[k] && reads[k]) lowest[k] = *depth[k] + *reads[k];
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t k = count; k-- > 0;)
            for (const auto s : next[k])
                if (lowest[s] < lowest[k]) { lowest[k] = lowest[s]; changed = true; }
    }
    std::vector<RedZoneDeadRange> dead;
    for (std::size_t k = 0; k < count; ++k) {
        if (!depth[k] || lowest[k] < *depth[k]) continue;
        const auto begin = base + body[k].Offset;
        const auto end = begin + body[k].Length;
        if (!dead.empty() && dead.back().End == begin) dead.back().End = end;
        else dead.push_back({begin, end});
    }
    return dead;
}

}

std::vector<TrampolineSite> TrampolineRedZoneRanges(
    const std::vector<std::uint8_t>& fileBytes,
    const std::vector<Domain::ProgramHeader>& codeSegments,
    const std::vector<RedZoneDeadRange>& live,
    const std::vector<TrampolineSite>& existing,
    std::vector<RedZoneDeadRange>& covered,
    RedZoneTrampolineReport& report
) {
    std::vector<TrampolineSite> sites;
    const std::vector<std::uint8_t> lower{0x48, 0x8d, 0x64, 0x24, 0x80};
    const std::vector<std::uint8_t> raise{0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};
    const auto scanner = MakeInstructionScanner();
    const X64InstructionDecoder decoder;
    for (const auto& ph : codeSegments) {
        const auto segOffset = static_cast<std::size_t>(ph.Offset);
        const auto segSize = static_cast<std::size_t>(ph.FileSize);
        if (segOffset > fileBytes.size() || segSize > fileBytes.size() - segOffset) continue;
        const auto* seg = fileBytes.data() + segOffset;
        std::set<std::size_t> targets;
        bool scanned = false;
        std::vector<std::pair<std::size_t, std::size_t>> taken;
        for (const auto& site : existing)
            if (site.Address >= ph.MappedAddress && site.Address < ph.MappedAddress + segSize) taken.emplace_back(static_cast<std::size_t>(site.Address - ph.MappedAddress), static_cast<std::size_t>(site.Address - ph.MappedAddress) + site.Length);
        for (const auto& range : live) {
            if (range.Begin < ph.MappedAddress || range.End <= range.Begin || range.End - ph.MappedAddress > segSize) continue;
            if (!scanned) {
                scanned = true;
                for (const auto& match : scanner->ScanCodeSection(std::vector<std::uint8_t>(seg, seg + segSize), 0, segSize)) {
                    const auto info = decoder.DecodeInstruction(seg + match.Offset, match.Length);
                    if (!info.HasBranchTarget || info.HasRipRelativeDisp) continue;
                    const auto target = static_cast<std::int64_t>(match.Offset + match.Length) + info.BranchDisp;
                    if (target >= 0 && static_cast<std::size_t>(target) < segSize) targets.insert(static_cast<std::size_t>(target));
                }
            }
            const auto begin = static_cast<std::size_t>(range.Begin - ph.MappedAddress);
            const auto end = static_cast<std::size_t>(range.End - ph.MappedAddress);
            const auto limit = std::min(segSize, end + 64);
            std::vector<Insn> body;
            for (const auto stop : {limit, end}) {
                body.clear();
                try {
                    const std::vector<std::uint8_t> window(seg + begin, seg + stop);
                    for (const auto& match : scanner->ScanCodeSection(window, 0, window.size())) {
                        if (begin + match.Offset != (body.empty() ? begin : body.back().Offset + body.back().Length)) break;
                        body.push_back({begin + static_cast<std::size_t>(match.Offset), match.Length, decoder.DecodeInstruction(window.data() + match.Offset, match.Length)});
                    }
                    break;
                } catch (const std::exception&) {
                    body.clear();
                }
            }
            if (body.empty()) { ++report.Refused["range that does not decode"]; continue; }
            const auto code = [&](const std::size_t k) { return seg + body[k].Offset; };
            const auto guarded = [&](const std::size_t k) {
                const auto& insn = body[k];
                if (!insn.Info.HasMemoryOperand || insn.Info.FlowKind != ControlFlowKind::Sequential || insn.Info.SegmentPrefix != 0) return false;
                const auto use = memoryUse(code(k), insn);
                return !use.RspBased && !use.UsesRsp && !use.RegisterRsp && !usesRspOutsideOperands(code(k), insn) && !pushesOrPops(code(k), insn);
            };
            const auto movable = [&](const std::size_t k) {
                const auto& insn = body[k];
                const auto flow = insn.Info.FlowKind;
                if (code(k)[insn.Info.OpcodeOffset] == 0xcc || insn.Info.TruncatesBranchTarget || insn.Info.SegmentPrefix != 0) return false;
                if (flow == ControlFlowKind::Sequential) return true;
                return (flow == ControlFlowKind::ConditionalBranch || flow == ControlFlowKind::UnconditionalJump) && insn.Info.HasBranchTarget && !insn.Info.HasRipRelativeDisp;
            };
            const auto refuse = [&](const char* why) { ++report.Refused[why]; };
            std::size_t next = 0;
            for (std::size_t k = 0; k < body.size() && body[k].Offset < end; ++k) {
                if (k < next || !guarded(k)) continue;
                if (body[k].Info.FlowKind != ControlFlowKind::Sequential) { refuse("guarded instruction is not sequential"); continue; }
                std::size_t first = k;
                std::size_t last = k + 1;
                const auto bytes = [&] { return body[last - 1].Offset + body[last - 1].Length - body[first].Offset; };
                bool stop = false;
                while (bytes() < 5 && !stop) {
                    if (last < body.size() && body[last - 1].Info.FlowKind == ControlFlowKind::Sequential && movable(last)) { ++last; continue; }
                    if (first > next && movable(first - 1) && body[first - 1].Info.FlowKind == ControlFlowKind::Sequential) { --first; continue; }
                    stop = true;
                }
                if (bytes() < 5) { refuse("no room for a jump"); continue; }
                const auto siteBegin = body[first].Offset;
                const auto siteEnd = siteBegin + bytes();
                const auto inside = targets.upper_bound(siteBegin);
                if (inside != targets.end() && *inside < siteEnd) { refuse("branch into the site"); continue; }
                if (std::any_of(taken.begin(), taken.end(), [&](const auto& span) { return siteBegin < span.second && span.first < siteEnd; })) { refuse("overlaps another stub"); continue; }
                StubBodyBuilder builder;
                bool encoded = true;
                std::size_t wrapped = 0;
                try {
                    for (auto j = first; j < last; ++j) {
                        const std::span<const std::uint8_t> instruction(code(j), body[j].Length);
                        builder.Seek(static_cast<std::int64_t>(body[j].Offset - siteBegin));
                        if (body[j].Info.FlowKind != ControlFlowKind::Sequential) {
                            builder.MoveControlFlow(instruction, body[j].Info);
                            continue;
                        }
                        const bool wrap = guarded(j);
                        if (wrap) builder.Raw(lower);
                        builder.Move(instruction, body[j].Info.HasRipRelativeDisp ? std::optional<std::size_t>(body[j].Info.RipRelativeDispOffset) : std::nullopt);
                        if (wrap) { builder.Raw(raise); ++wrapped; }
                    }
                } catch (const CodegenException&) {
                    encoded = false;
                }
                if (!encoded) { refuse("instruction that cannot move into a stub"); continue; }
                auto lowered = builder.Finish();
                TrampolineSite site;
                site.Offset = ph.Offset + siteBegin;
                site.Address = ph.MappedAddress + siteBegin;
                site.Length = siteEnd - siteBegin;
                site.OriginalBytes.assign(seg + siteBegin, seg + siteEnd);
                site.Body = std::move(lowered.Bytes);
                site.ReturnBranchOffset = lowered.ReturnBranchOffset;
                site.Relocations = std::move(lowered.Relocations);
                sites.push_back(std::move(site));
                taken.emplace_back(siteBegin, siteEnd);
                covered.push_back({ph.MappedAddress + siteBegin, ph.MappedAddress + siteEnd});
                report.Instructions += wrapped;
                ++report.Sites;
                next = last;
            }
        }
    }
    std::sort(covered.begin(), covered.end(), [](const auto& left, const auto& right) { return left.Begin < right.Begin; });
    return sites;
}

std::vector<std::vector<RedZoneDeadRange>> FindRedZoneDeadRanges(
    const std::vector<std::uint8_t>& fileBytes,
    const std::vector<Domain::ProgramHeader>& codeSegments,
    const std::vector<RedZoneFunction>& leaves,
    const bool killStores,
    std::vector<std::uint64_t>* tailCallLeaves
) {
    std::vector<std::vector<RedZoneDeadRange>> result(leaves.size());
    for (const auto& ph : codeSegments) {
        const auto segOffset = static_cast<std::size_t>(ph.Offset);
        const auto segSize = static_cast<std::size_t>(ph.FileSize);
        if (segOffset > fileBytes.size() || segSize > fileBytes.size() - segOffset) continue;
        const std::vector<std::uint8_t> seg(fileBytes.begin() + static_cast<std::ptrdiff_t>(segOffset), fileBytes.begin() + static_cast<std::ptrdiff_t>(segOffset + segSize));
        std::optional<std::vector<BranchSite>> branches;
        for (std::size_t index = 0; index < leaves.size(); ++index) {
            const auto& leaf = leaves[index];
            if (leaf.Begin < ph.MappedAddress || leaf.End <= leaf.Begin || leaf.End - ph.MappedAddress > segSize) continue;
            if (!branches) {
                branches.emplace();
                const auto scanner = MakeInstructionScanner();
                const X64InstructionDecoder decoder;
                for (const auto& match : scanner->ScanCodeSection(seg, 0, seg.size())) {
                    const auto info = decoder.DecodeInstruction(seg.data() + match.Offset, match.Length);
                    if (!info.HasBranchTarget || info.HasRipRelativeDisp) continue;
                    const auto target = static_cast<std::int64_t>(match.Offset + match.Length) + info.BranchDisp;
                    if (target >= 0 && static_cast<std::size_t>(target) < seg.size()) branches->push_back({static_cast<std::size_t>(target), static_cast<std::size_t>(match.Offset), info.FlowKind == ControlFlowKind::Call});
                }
                std::sort(branches->begin(), branches->end(), [](const BranchSite& left, const BranchSite& right) { return left.Target < right.Target; });
            }
            bool tailCallExit = false;
            result[index] = deadRanges(seg, ph.MappedAddress, *branches, static_cast<std::size_t>(leaf.Begin - ph.MappedAddress), static_cast<std::size_t>(leaf.End - ph.MappedAddress), killStores, tailCallExit);
            if (tailCallExit && tailCallLeaves != nullptr) tailCallLeaves->push_back(leaf.Begin);
        }
    }
    return result;
}

std::vector<TrampolineSite> FrameRedZoneFunctions(
    const std::vector<std::uint8_t>& fileBytes,
    const std::vector<Domain::ProgramHeader>& codeSegments,
    const std::vector<RedZoneFunction>& functions,
    const std::vector<TrampolineSite>& existing,
    RedZoneFrameReport& report,
    std::vector<RedZoneDisplacement>& displacements,
    const bool shrinkWrapped,
    const bool countFramePointerAllocation,
    const bool suspectFramePointerLeaves,
    const bool stubJumpsStayInBody,
    const bool exitBranches
) {
    std::vector<TrampolineSite> sites;
    for (const auto& ph : codeSegments) frameSegment(fileBytes, ph, functions, existing, sites, displacements, report, shrinkWrapped, countFramePointerAllocation, suspectFramePointerLeaves, stubJumpsStayInBody, exitBranches);
    return sites;
}

}
