#ifndef RELINKER_ANALYSIS_REDZONETABLE_HPP
#define RELINKER_ANALYSIS_REDZONETABLE_HPP

#include <codegen/CodegenTypes.hpp>
#include <codegen/x86/RedZoneFramer.hpp>
#include <domain/Types.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Relinker {

struct RedZoneGuardStats {
    std::size_t Leaves = 0;
    std::size_t LeavesWithDeadCode = 0;
    std::uint64_t LeafBytes = 0;
    std::uint64_t DeadBytes = 0;
};

std::string RedZoneGuardTable(const std::vector<std::uint8_t>& bytes, const std::vector<Domain::ProgramHeader>& headers, const std::vector<Codegen::TrampolineSite>& existing, const Codegen::RedZoneFrameReport* framed, bool precise = false, RedZoneGuardStats* stats = nullptr, bool countFramePointerAllocation = false, bool suspectFramePointerLeaves = false, bool stubJumpsStayInBody = false);

}

#endif
