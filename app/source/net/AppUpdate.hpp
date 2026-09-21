#pragma once

// Self-update of the Nimbus app from "app-v*" releases (CIA only).

#include <string>
#include <vector>

#include "Manager.hpp"
#include "Release.hpp"

namespace AppUpdate {

constexpr const char* kReleasesUrl = "https://api.github.com/repos/Happynico7504/nimbus-nico_christmann/releases?per_page=30";
constexpr const char* kTagPrefix = "app-v";
constexpr const char* kAssetPrefix = "nimbus";
constexpr const char* kAssetSuffix = ".cia";

struct Info {
	std::string tag;        // e.g. "app-v2.2.0"
	Release::Asset asset;   // the .cia
	bool newer = false;     // newer than the running version
};

// Look up the newest app release and compare it with the running version.
bool check(Manager::HttpClient& http, int major, int minor, int micro, Info& out, std::string& err);

// Download the .cia and verify its SHA-256 against the release (when GitHub reports one).
bool fetch(Manager::HttpClient& http, const Info& info, std::vector<uint8_t>& cia, std::string& err);

} // namespace AppUpdate
