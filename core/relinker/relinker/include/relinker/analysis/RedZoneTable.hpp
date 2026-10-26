#ifndef RELINKER_ANALYSIS_REDZONETABLE_HPP
#define RELINKER_ANALYSIS_REDZONETABLE_HPP

#include <codegen/CodegenTypes.hpp>
#include <codegen/x86/RedZoneFramer.hpp>
#include <domain/Types.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Relinker {

std::string RedZoneGuardTable(const std::vector<std::uint8_t>& bytes, const std::vector<Domain::ProgramHeader>& headers, const std::vector<Codegen::TrampolineSite>& existing, const Codegen::RedZoneFrameReport* framed);

}

#endif
