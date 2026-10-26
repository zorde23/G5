#include <relinker/analysis/RedZoneTable.hpp>
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

}

int main() {
    try {
        listsUnframedLeavesAndUncoveredCode();
        listsOnlyUncoveredCodeOutsidePadding();
        std::cout << "red-zone table tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "red-zone table tests failed: " << error.what() << '\n';
        return 1;
    }
}
