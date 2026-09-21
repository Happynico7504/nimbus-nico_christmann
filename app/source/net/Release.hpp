#pragma once

#include <string>

namespace Release {

struct Asset {
	std::string name;   // file name, e.g. "3dsx.2.1.1.zip"
	std::string url;    // browser_download_url
	std::string sha256; // hex digest if GitHub reports one ("" otherwise)
};

// Find the first release asset in a GitHub "releases/latest" JSON document whose file name
// starts with `prefix` and ends with `suffix`. Deliberately not a full JSON parser: it only
// reads the "browser_download_url" and "digest" string values of the assets array.
bool findAsset(const std::string& json, const std::string& prefix, const std::string& suffix, Asset& out);

// Pick, from a GitHub "releases" list (JSON array), the release with the highest version whose
// tag starts with `tagPrefix` (e.g. "patches-v") and that has an asset matching prefix/suffix.
// Pre-releases are ignored. `tag` receives the full tag name ("patches-v1.2.0").
bool findLatestByTagPrefix(const std::string& releasesJson, const std::string& tagPrefix, const std::string& assetPrefix,
                           const std::string& assetSuffix, std::string& tag, Asset& out);

// Compare dotted numeric versions after stripping `prefix` from both ("patches-v1.10.0" > "patches-v1.9.0").
// Returns <0, 0 or >0. Non-numeric parts compare as 0.
int compareVersions(const std::string& a, const std::string& b, const std::string& prefix);

} // namespace Release
