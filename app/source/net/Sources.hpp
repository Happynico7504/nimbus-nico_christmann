#pragma once

#include <string>
#include <vector>

namespace Sources {

enum class Kind {
	GithubReleaseZip,   // latest release has a .zip whose 3ds/nimbus/update/ folder holds the patches
	GithubReleaseTarGz, // same, but a .tar.gz
	RawFiles            // plain files fetched from raw.githubusercontent.com
};

struct RawFile {
	std::string remote; // path relative to rawBase
	std::string local;  // name used in the install cache (must be a name the installer knows)
};

struct Network {
	std::string id;        // stable id stored on the SD card
	std::string name;      // shown in the menu
	std::string publisher; // shown in the trust screen
	bool owned;            // true only for our own network: everything else needs explicit user trust
	Kind kind;
	std::string releasesUrl;  // GitHub API "list releases" URL (release kinds)
	std::string tagPrefix;    // only releases whose tag starts with this are considered, e.g. "patches-v"
	std::string assetPrefix;  // asset file name prefix (release kinds)
	std::string assetSuffix;  // asset file name suffix (release kinds)
	std::string rawBase;      // base URL ending in '/' (RawFiles)
	std::string versionUrl;   // URL of a small text file holding the current version (RawFiles)
	std::vector<RawFile> rawFiles;
};

const std::vector<Network>& all();
const Network* findById(const std::string& id);

// The folder inside release archives that holds the patch files.
constexpr const char* kArchiveMarker = "3ds/nimbus/update/";

} // namespace Sources
