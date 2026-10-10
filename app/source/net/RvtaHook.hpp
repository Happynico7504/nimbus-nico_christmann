#pragma once

#include <string>

// Starts RvtA (the Revivetendo analytics sysmodule, /luma/sysmodules/0004013000F7A002.cxi) at boot.
// Luma cannot add new boot modules, so RvtA is listed as a dependency of a sysmodule that NS starts at
// boot, through a Luma exheader override. That module's exheader is read from the console itself
// (fs:REG), so it always matches the installed firmware and no Nintendo data is shipped.
namespace RvtaHook {

constexpr const char* kCxiPath = "/luma/sysmodules/0004013000F7A002.cxi";

// Installs the hook when RvtA is installed, removes it otherwise. Returns a line for the result screen.
std::string sync();

} // namespace RvtaHook
