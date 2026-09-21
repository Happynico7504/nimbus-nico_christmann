#include "Manager.hpp"

#include <cstdio>

#include "Archive.hpp"
#include "Fs.hpp"
#include "Installer.hpp"
#include "Release.hpp"

namespace Manager {

static constexpr size_t kMaxJson = 512 * 1024;
static constexpr size_t kMaxArchive = 8 * 1024 * 1024;
static constexpr size_t kMaxRawFile = 2 * 1024 * 1024;

static std::string trim(std::string s) {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
	size_t i = 0;
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
	return s.substr(i);
}

bool latestVersion(const Sources::Network& net, HttpClient& http, std::string& version, std::string& err) {
	std::vector<uint8_t> buf;
	if (net.kind == Sources::Kind::RawFiles) {
		if (!http.get(net.versionUrl, buf, err, 4096)) return false;
		version = trim(std::string(buf.begin(), buf.end()));
		if (version.empty() || version.size() > 32) { err = "version file is empty or invalid"; return false; }
		return true;
	}
	if (!http.get(net.releasesUrl, buf, err, kMaxJson)) return false;
	std::string tag;
	Release::Asset a;
	if (!Release::findLatestByTagPrefix(std::string(buf.begin(), buf.end()), net.tagPrefix, net.assetPrefix, net.assetSuffix, tag, a)) {
		err = "no matching release found";
		return false;
	}
	version = tag;
	return true;
}

bool isNewer(const Sources::Network& net, const std::string& installed, const std::string& latest) {
	if (installed.empty()) return true;
	if (net.kind == Sources::Kind::RawFiles) return installed != latest;
	return Release::compareVersions(latest, installed, net.tagPrefix) > 0;
}

namespace {
struct DirSink : Archive::FileSink {
	std::string dir;
	int count = 0;
	bool write(const std::string& name, const uint8_t* data, size_t size) override {
		count++;
		return Fs::writeFile(dir + "/" + name, data, size);
	}
};

bool cacheIsCurrent(const std::string& dir, const std::string& version) {
	std::string v;
	if (!Fs::readFile(dir + "/version.txt", v) || trim(v) != version) return false;
	for (const auto& k : Installer::knownFiles())
		if (Fs::exists(dir + "/" + k)) return true;
	return false;
}
} // namespace

bool prepare(const Sources::Network& net, HttpClient& http, const std::string& cacheRoot, Prepared& out, std::string& err) {
	std::string version;
	std::vector<uint8_t> buf;
	Release::Asset asset;

	// 1) what is the newest version?
	if (net.kind == Sources::Kind::RawFiles) {
		if (!latestVersion(net, http, version, err)) return false;
	} else {
		if (!http.get(net.releasesUrl, buf, err, kMaxJson)) return false;
		if (!Release::findLatestByTagPrefix(std::string(buf.begin(), buf.end()), net.tagPrefix, net.assetPrefix, net.assetSuffix, version, asset)) {
			err = "no matching release found";
			return false;
		}
	}

	std::string cache = cacheRoot + "/" + net.id;
	out.version = version;
	out.cacheDir = cache;

	// 2) cache still current? then nothing to download
	if (cacheIsCurrent(cache, version)) {
		out.usedCache = true;
		return true;
	}

	// 3) download into a fresh folder next to the cache
	std::string fresh = cache + ".new";
	Fs::removeTree(fresh);
	if (!Fs::mkdirs(fresh)) { err = "could not create cache folder"; return false; }

	DirSink sink;
	sink.dir = fresh;

	if (net.kind == Sources::Kind::RawFiles) {
		for (const auto& f : net.rawFiles) {
			std::vector<uint8_t> file;
			if (!http.get(net.rawBase + f.remote, file, err, kMaxRawFile)) { Fs::removeTree(fresh); err = f.remote + ": " + err; return false; }
			if (!Archive::isSafeFileName(f.local) || !Fs::writeFile(fresh + "/" + f.local, file.data(), file.size())) {
				Fs::removeTree(fresh);
				err = "could not store " + f.local;
				return false;
			}
			sink.count++;
		}
	} else {
		std::vector<uint8_t> archive;
		if (!http.get(asset.url, archive, err, kMaxArchive)) { Fs::removeTree(fresh); return false; }
		if (!asset.sha256.empty() && Archive::sha256Hex(archive.data(), archive.size()) != asset.sha256) {
			Fs::removeTree(fresh);
			err = "download does not match the release checksum";
			return false;
		}
		bool ok = net.kind == Sources::Kind::GithubReleaseZip
		              ? Archive::extractZip(archive.data(), archive.size(), Sources::kArchiveMarker, sink, err)
		              : Archive::extractTarGz(archive.data(), archive.size(), Sources::kArchiveMarker, sink, err);
		if (!ok) { Fs::removeTree(fresh); return false; }
	}

	bool any = false;
	for (const auto& k : Installer::knownFiles())
		if (Fs::exists(fresh + "/" + k)) { any = true; break; }
	if (!any) { Fs::removeTree(fresh); err = "the download contained no patch files"; return false; }

	if (!Fs::writeText(fresh + "/version.txt", version)) { Fs::removeTree(fresh); err = "could not write version"; return false; }

	// 4) swap in
	Fs::removeTree(cache);
	if (std::rename(fresh.c_str(), cache.c_str()) != 0) { err = "could not activate the download"; return false; }
	out.usedCache = false;
	return true;
}

} // namespace Manager
