#include <relinker/analysis/RedZoneTable.hpp>
#include <relinker/analysis/UnusedNidFilter/EhFrameReader.hpp>
#include <elfpatcher/windows/WindowsPeFormat.hpp>
#include <algorithm>
#include <cstdio>
#include <limits>

namespace Relinker {

namespace {

struct GuardRange {
    std::uint64_t Begin;
    std::uint64_t End;
    std::string Reason;
};

bool Padding(std::uint8_t byte) {
    return byte == 0xcc || byte == 0x90 || byte == 0x00;
}

void AddUncovered(const std::vector<std::uint8_t>& bytes, const Domain::ProgramHeader& segment, const std::vector<Codegen::RedZoneFunction>& functions, std::vector<GuardRange>& ranges) {
    const auto segmentEnd = segment.MappedAddress + segment.FileSize;
    const auto addGap = [&](std::uint64_t begin, std::uint64_t end) {
        if (end <= begin) return;
        const auto first = bytes.begin() + static_cast<std::ptrdiff_t>(segment.Offset + (begin - segment.MappedAddress));
        if (std::all_of(first, first + static_cast<std::ptrdiff_t>(end - begin), Padding)) return;
        ranges.push_back({begin, end, "no FDE"});
    };
    auto cursor = segment.MappedAddress;
    for (const auto& function : functions) {
        if (function.End <= segment.MappedAddress || function.Begin >= segmentEnd) continue;
        addGap(cursor, std::min(function.Begin, segmentEnd));
        cursor = std::max(cursor, std::min(function.End, segmentEnd));
    }
    addGap(cursor, segmentEnd);
}

}

std::string RedZoneGuardTable(const std::vector<std::uint8_t>& bytes, const std::vector<Domain::ProgramHeader>& headers, const std::vector<Codegen::TrampolineSite>& existing, const Codegen::RedZoneFrameReport* framed) {
    std::vector<Domain::ProgramHeader> codeSegments;
    std::uint64_t firstAddress = std::numeric_limits<std::uint64_t>::max();
    for (const auto& header : headers) {
        if (header.Type != 1) continue;
        firstAddress = std::min(firstAddress, header.MappedAddress);
        if ((header.Flags & 1) != 0 && header.Offset <= bytes.size() && header.FileSize <= bytes.size() - header.Offset) codeSegments.push_back(header);
    }
    if (codeSegments.empty()) return {};
    firstAddress &= ~static_cast<std::uint64_t>(Elfpatcher::Windows::SectionAlignment - 1);

    std::vector<Codegen::RedZoneFunction> functions;
    try {
        for (const auto& region : UnusedNidFilter::ReadExceptionFunctions(bytes, headers, {}, {})) functions.push_back({region.Begin, region.End});
    } catch (const Domain::RelinkerException&) {
        functions.clear();
    }
    std::sort(functions.begin(), functions.end(), [](const auto& left, const auto& right) { return left.Begin < right.Begin; });

    std::vector<GuardRange> ranges;
    Codegen::RedZoneFrameReport scanned;
    if (framed == nullptr) {
        std::vector<Codegen::RedZoneDisplacement> displacements;
        Codegen::FrameRedZoneFunctions(bytes, codeSegments, functions, existing, scanned, displacements);
    }
    for (const auto& leaf : (framed != nullptr ? *framed : scanned).Leaves) {
        if (framed == nullptr) ranges.push_back({leaf.Begin, leaf.End, "module not framed"});
        else if (!leaf.Reason.empty()) ranges.push_back({leaf.Begin, leaf.End, leaf.Reason});
    }
    for (const auto& segment : codeSegments) AddUncovered(bytes, segment, functions, ranges);

    std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) { return left.Begin < right.Begin; });
    std::vector<GuardRange> merged;
    for (auto& range : ranges) {
        if (merged.empty() || range.Begin >= merged.back().End) {
            merged.push_back(std::move(range));
            continue;
        }
        merged.back().End = std::max(merged.back().End, range.End);
        if (merged.back().Reason == "no FDE") merged.back().Reason = std::move(range.Reason);
    }
    std::string table;
    for (const auto& range : merged) {
        char line[64];
        std::snprintf(line, sizeof(line), "%llx %llx ", static_cast<unsigned long long>(Elfpatcher::Windows::LoadRva + (range.Begin - firstAddress)), static_cast<unsigned long long>(Elfpatcher::Windows::LoadRva + (range.End - firstAddress)));
        table += line;
        table += range.Reason;
        table += '\n';
    }
    return table;
}

}
