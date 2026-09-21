#pragma once

// The download / cache / update-check logic, independent of libctru: all network access goes through
// HttpClient, so the complete flow is tested on the host with a fake server (see app/tests/).

#include <cstdint>
#include <string>
#include <vector>

#include "Sources.hpp"

namespace Manager {

struct HttpClient {
	virtual ~HttpClient() = default;
	// Fetch `url` fully into `out` (following redirects). `maxBytes` bounds memory use.
	virtual bool get(const std::string& url, std::vector<uint8_t>& out, std::string& err, size_t maxBytes) = 0;
};

struct Prepared {
	std::string version;   // the version now in the cache
	bool usedCache = false;
	std::string cacheDir;  // folder holding the patch files
};

// Newest version the source offers (a release tag, or the contents of latest_version.txt).
bool latestVersion(const Sources::Network& net, HttpClient& http, std::string& version, std::string& err);

// Make sure `<cacheRoot>/<net.id>/` holds the newest patches: reuse it if its recorded version is already
// current, otherwise download into a fresh folder, verify, and only then swap it in. A failure at any point
// leaves the previous cache untouched.
bool prepare(const Sources::Network& net, HttpClient& http, const std::string& cacheRoot, Prepared& out, std::string& err);

// True when `latest` is a newer version than `installed` for this source.
bool isNewer(const Sources::Network& net, const std::string& installed, const std::string& latest);

} // namespace Manager
