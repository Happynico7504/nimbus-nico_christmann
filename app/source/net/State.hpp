#pragma once

#include <map>
#include <set>
#include <string>

// What the app remembers between runs, stored as simple key=value lines in /3ds/nimbus/state.txt.
struct State {
	std::map<std::string, std::string> selection; // service id -> provider id ("off" = not installed); what is installed
	std::map<std::string, std::string> versions;  // provider id -> version of the copy that was installed
	std::set<std::string> trusted;                // ids of third-party providers the user chose to trust

	bool installed() const { return !selection.empty(); }

	static State parse(const std::string& text);
	std::string serialize() const;
};
