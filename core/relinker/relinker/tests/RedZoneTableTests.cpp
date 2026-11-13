#include <relinker/analysis/RedZoneTable.hpp>
#include <io/BufferUtils.hpp>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint64_t Base = 0x2000;

std::vector<Domain::ProgramHeader> segments(std::size_t bytes) {
    return {{1, 5, 0, Base, Base, bytes, bytes, 16}, {1, 6, bytes, Base + 0x10000, Base + 0x10000, 0, 0x100, 16}};
}

void listsUnframedLeavesAndUncoveredCode() {
    std::vector<std::uint8_t> bytes(0x80, 0xcc);
    for (std::size_t i = 0x10; i < 0x18; ++i) bytes[i] = 0x48;
    for (std::size_t i = 0x40; i < 0x48; ++i) bytes[i] = 0x48;
    Codegen::RedZoneFrameReport report;
    report.Leaves.push_back({Base + 0x40, Base + 0x50, "exit shorter than a jump"});
    report.Leaves.push_back({Base + 0x60, Base + 0x70, ""});
    const auto table = Relinker::RedZoneGuardTable(bytes, segments(bytes.size()), {}, &report);
    require(table == "10000 10080 exit shorter than a jump\n", "an unframed leaf inside code no FDE covers must keep its reason, at RVAs from LoadRva; got:\n" + table);
}

void listsOnlyUncoveredCodeOutsidePadding() {
    std::vector<std::uint8_t> bytes(0x80, 0xcc);
    for (std::size_t i = 0x20; i < 0x24; ++i) bytes[i] = 0x90;
    Codegen::RedZoneFrameReport report;
    require(Relinker::RedZoneGuardTable(bytes, segments(bytes.size()), {}, &report).empty(), "padding alone is not code no FDE covers");
    bytes[0x30] = 0xc3;
    require(Relinker::RedZoneGuardTable(bytes, segments(bytes.size()), {}, &report) == "10000 10080 no FDE\n", "code outside every FDE was not listed");
}

void passesTheFramePointerSwitchesToModulesItScans() {
    std::vector<std::uint8_t> bytes(128, 0);
    bytes[0] = 1; bytes[1] = 0; bytes[2] = 3; bytes[3] = 0;
    Io::WriteU64(bytes, 4, 0x1040); Io::WriteU32(bytes, 12, 1);
    Io::WriteU64(bytes, 16, 0x2000); Io::WriteU64(bytes, 24, 0x1060);
    Io::WriteU32(bytes, 64, 9); bytes[72] = 1; bytes[73] = 0; bytes[74] = 1; bytes[75] = 0x78; bytes[76] = 16;
    Io::WriteU32(bytes, 96, 20); Io::WriteU32(bytes, 100, 36);
    Io::WriteU64(bytes, 104, 0x2000); Io::WriteU64(bytes, 112, 0x10);
    const std::vector<std::uint8_t> movesFramePointer{0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xed, 0x40, 0x48, 0x89, 0x45, 0x10, 0x5d, 0xc3, 0xcc, 0xcc};
    bytes.insert(bytes.end(), movesFramePointer.begin(), movesFramePointer.end());
    Domain::ProgramHeader load{}; load.Type = 1; load.Flags = 4; load.MappedAddress = 0x1000; load.FileSize = 128;
    Domain::ProgramHeader code{}; code.Type = 1; code.Flags = 5; code.Offset = 128; code.MappedAddress = 0x2000; code.FileSize = 16;
    auto eh = load; eh.Type = 0x6474E550; eh.FileSize = 32;
    const auto quiet = Relinker::RedZoneGuardTable(bytes, {load, code, eh}, {}, nullptr);
    require(quiet.empty(), "a scanned module listed a frame-pointer leaf that moves rbp, or code its FDE covers, without --suspect-frame-pointer-leaves; got:\n" + quiet);
    const auto suspected = Relinker::RedZoneGuardTable(bytes, {load, code, eh}, {}, nullptr, false, nullptr, false, true);
    require(suspected.find("module not framed") != std::string::npos, "--suspect-frame-pointer-leaves did not reach the table of a scanned module; got:\n" + suspected);
}

}

int main() {
    try {
        listsUnframedLeavesAndUncoveredCode();
        listsOnlyUncoveredCodeOutsidePadding();
        passesTheFramePointerSwitchesToModulesItScans();
        std::cout << "red-zone table tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "red-zone table tests failed: " << error.what() << '\n';
        return 1;
    }
}
