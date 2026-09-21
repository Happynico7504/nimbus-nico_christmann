#pragma once

#include <set>
#include <string>

// What the app remembers between runs, stored as simple key=value lines in /3ds/nimbus/state.txt.
struct State {
	std::string network;          // id of the network whose patches are installed ("" = none yet)
	std::string installedVersion; // version/tag of those patches
	std::set<std::string> trusted; // ids of third-party networks the user chose to trust

	static State parse(const std::string& text);
	std::string serialize() const;
};
