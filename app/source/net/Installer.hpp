#pragma once

#include <string>
#include <vector>

namespace Installer {

// Copies the patch files found in `srcDir` into their places on the SD card (the same set of
// destinations the app has always installed), replacing what was there. For every known
// destination this network does not ship, the old file is removed, so a network that does not ship a given patch
// leaves that patch uninstalled: switching networks therefore also cleans up the previous one.
//
// Refuses to touch anything unless `srcDir` holds at least one known patch file, so a failed
// download can never wipe a working install.
//
// `root` is prepended to every absolute path ("" on the console, a temp dir in tests).
struct Result {
	bool ok = false;
	std::string error;
	int installed = 0; // files moved into place
	int removed = 0;   // stale files removed because the new network does not ship them
};

Result install(const std::string& srcDir, const std::string& root = "");

// Names install() understands inside srcDir (used to validate downloads).
const std::vector<std::string>& knownFiles();

} // namespace Installer
