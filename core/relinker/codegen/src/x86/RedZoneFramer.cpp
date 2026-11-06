#include <codegen/x86/RedZoneFramer.hpp>
#include <codegen/IInstructionScanner.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>

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
};

void frameSegment(
    const std::vector<std::uint8_t>& fileBytes,
    const Domain::ProgramHeader& ph,
    const std::vector<RedZoneFunction>& functions,
    const std::vector<TrampolineSite>& existing,
    std::vector<TrampolineSite>& sites,
    std::vector<RedZoneDisplacement>& displacements,
    RedZoneFrameReport& report,
    const bool shrinkWrapped
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
    const auto overlapsExisting = [&](const std::size_t begin, const std::size_t end) {
        return std::any_of(taken.begin(), taken.end(), [&](const auto& range) { return begin < range.second && range.first < end; });
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
        for (const auto& match : scanner->ScanCodeSection(code, 0, code.size())) {
            if (code[match.Offset] == 0xcc && match.Length == 1) break;
            body.push_back({start + static_cast<std::size_t>(match.Offset), match.Length, decoder.DecodeInstruction(code.data() + match.Offset, match.Length)});
        }
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
            if (use.RbpBased && rbpPrologue && use.RbpDisp < -static_cast<std::int64_t>(8 * pushed.size())) redZone = true;
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
                    if (prior.Offset + prior.Length != exit.Begin || prior.Info.FlowKind != ControlFlowKind::Sequential || prior.Info.HasRipRelativeDisp || frees.contains(j) || pushesOrPops(at(j), prior)) break;
                    const auto priorUse = memoryUse(at(j), prior);
                    const auto bytes = priorUse.RspBased ? withDisplacement(at(j), prior, priorUse, priorUse.RspDisp + shift) : std::vector<std::uint8_t>(at(j), at(j) + prior.Length);
                    if (!bytes) break;
                    exit.Lead.insert(exit.Lead.begin(), bytes->begin(), bytes->end());
                    if (priorUse.RspBased) ++exit.Operands;
                    exit.Begin = prior.Offset;
                    exit.First = j;
                }
                if (exit.End - exit.Begin < kJumpBytes) { reason = "exit shorter than a jump"; continue; }
                if (targetInside(exit.Begin, exit.End)) { reason = exit.First == exit.Tail ? "branch into an exit" : "branch into the instructions before a short exit"; continue; }
                exits.push_back(std::move(exit));
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

std::vector<RedZoneDeadRange> deadRanges(const std::vector<std::uint8_t>& seg, const std::uint64_t base, const std::vector<BranchSite>& branches, const std::size_t start, const std::size_t limit) {
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
    for (std::size_t k = 0; k < count; ++k) {
        const auto& insn = body[k];
        const auto flow = insn.Info.FlowKind;
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
            if (use.RspBased) reads[k] = reads[k] ? std::min(*reads[k], use.RspDisp) : use.RspDisp;
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

std::vector<std::vector<RedZoneDeadRange>> FindRedZoneDeadRanges(
    const std::vector<std::uint8_t>& fileBytes,
    const std::vector<Domain::ProgramHeader>& codeSegments,
    const std::vector<RedZoneFunction>& leaves
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
            result[index] = deadRanges(seg, ph.MappedAddress, *branches, static_cast<std::size_t>(leaf.Begin - ph.MappedAddress), static_cast<std::size_t>(leaf.End - ph.MappedAddress));
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
    const bool shrinkWrapped
) {
    std::vector<TrampolineSite> sites;
    for (const auto& ph : codeSegments) frameSegment(fileBytes, ph, functions, existing, sites, displacements, report, shrinkWrapped);
    return sites;
}

}
