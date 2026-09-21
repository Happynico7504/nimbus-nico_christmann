#pragma once

// Installs a CIA through the AM service (used to update Nimbus itself). 3DS only.

#include <cstdint>
#include <string>
#include <vector>

namespace CiaInstall {

bool install(const std::vector<uint8_t>& cia, std::string& err);

} // namespace CiaInstall
