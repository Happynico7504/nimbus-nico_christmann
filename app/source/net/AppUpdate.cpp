#include "AppUpdate.hpp"

#include "Archive.hpp"

namespace AppUpdate {

bool check(Manager::HttpClient& http, int major, int minor, int micro, Info& out, std::string& err) {
	std::vector<uint8_t> buf;
	if (!http.get(kReleasesUrl, buf, err, 512 * 1024)) return false;
	if (!Release::findLatestByTagPrefix(std::string(buf.begin(), buf.end()), kTagPrefix, kAssetPrefix, kAssetSuffix, out.tag, out.asset)) {
		err = "no app release found";
		return false;
	}
	std::string cur = std::string(kTagPrefix) + std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(micro);
	out.newer = Release::compareVersions(out.tag, cur, kTagPrefix) > 0;
	return true;
}

bool fetch(Manager::HttpClient& http, const Info& info, std::vector<uint8_t>& cia, std::string& err) {
	if (!http.get(info.asset.url, cia, err, 8 * 1024 * 1024)) return false;
	if (cia.size() < 0x2020) { err = "downloaded file is too small to be a CIA"; return false; }
	if (!info.asset.sha256.empty() && Archive::sha256Hex(cia.data(), cia.size()) != info.asset.sha256) {
		err = "download does not match the release checksum";
		return false;
	}
	return true;
}

} // namespace AppUpdate
