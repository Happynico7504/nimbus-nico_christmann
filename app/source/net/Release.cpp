#include "Release.hpp"

#include <vector>

namespace Release {

// Reads the JSON string value that follows the key whose opening quote is at `keyPos`
// (`"key"` : `"value"`). On success `endPos` is just past the closing quote of the value.
static bool readValueAfterKey(const std::string& s, size_t keyPos, const std::string& key, std::string& val, size_t& endPos) {
	size_t p = keyPos + key.size() + 2; // skip "key"
	while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
	if (p >= s.size() || s[p] != ':') return false;
	p++;
	while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
	if (p >= s.size() || s[p] != '"') return false;
	p++;
	val.clear();
	while (p < s.size() && s[p] != '"') {
		if (s[p] == '\\' && p + 1 < s.size()) {
			char c = s[p + 1];
			if (c == '/' || c == '\\' || c == '"') { val += c; p += 2; continue; }
			return false; // \uXXXX etc. never appear in URLs we care about
		}
		val += s[p++];
	}
	if (p >= s.size()) return false;
	endPos = p + 1;
	return true;
}

static bool hasPrefix(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
static bool hasSuffix(const std::string& s, const std::string& x) { return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; }

bool findAsset(const std::string& json, const std::string& prefix, const std::string& suffix, Asset& out) {
	size_t assets = json.find("\"assets\"");
	if (assets == std::string::npos) return false;

	const std::string urlKey = "browser_download_url";
	const std::string digestKey = "digest";
	size_t segStart = assets;
	size_t pos = assets;

	while (true) {
		size_t kp = json.find("\"" + urlKey + "\"", pos);
		if (kp == std::string::npos) return false;
		std::string url;
		size_t end = 0;
		if (!readValueAfterKey(json, kp + 0, urlKey, url, end)) { pos = kp + 1; continue; }

		size_t slash = url.rfind('/');
		std::string name = slash == std::string::npos ? url : url.substr(slash + 1);

		if (hasPrefix(name, prefix) && hasSuffix(name, suffix)) {
			// The digest field of the same asset object sits between the previous asset's URL and this one.
			std::string digest;
			size_t dp = segStart;
			size_t lastDigest = std::string::npos;
			while ((dp = json.find("\"" + digestKey + "\"", dp)) != std::string::npos && dp < kp) { lastDigest = dp; dp++; }
			if (lastDigest != std::string::npos) {
				std::string v;
				size_t e2 = 0;
				if (readValueAfterKey(json, lastDigest, digestKey, v, e2) && hasPrefix(v, "sha256:")) digest = v.substr(7);
			}
			out.name = name;
			out.url = url;
			out.sha256 = digest;
			return true;
		}
		segStart = end;
		pos = end;
	}
}

static std::vector<int> parseVersion(const std::string& s, const std::string& prefix) {
	std::string t = hasPrefix(s, prefix) ? s.substr(prefix.size()) : s;
	std::vector<int> v;
	int cur = 0;
	bool any = false;
	for (char ch : t) {
		if (ch >= '0' && ch <= '9') { cur = cur * 10 + (ch - '0'); any = true; }
		else if (ch == '.') { v.push_back(any ? cur : 0); cur = 0; any = false; }
		else break;
	}
	v.push_back(any ? cur : 0);
	return v;
}

int compareVersions(const std::string& a, const std::string& b, const std::string& prefix) {
	std::vector<int> x = parseVersion(a, prefix), y = parseVersion(b, prefix);
	size_t n = x.size() > y.size() ? x.size() : y.size();
	for (size_t i = 0; i < n; i++) {
		int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
		if (p != q) return p < q ? -1 : 1;
	}
	return 0;
}

bool findLatestByTagPrefix(const std::string& json, const std::string& tagPrefix, const std::string& assetPrefix,
                           const std::string& assetSuffix, std::string& tag, Asset& out) {
	// Each release object starts at its "tag_name" and runs to the next release's "tag_name"
	// (GitHub emits tag_name before the assets array; a JSON string can never contain an
	// unescaped "tag_name" key, so this split is safe for these documents).
	const std::string key = "tag_name";
	std::vector<size_t> starts;
	for (size_t p = json.find("\"" + key + "\""); p != std::string::npos; p = json.find("\"" + key + "\"", p + 1)) starts.push_back(p);

	bool found = false;
	for (size_t i = 0; i < starts.size(); i++) {
		size_t end = i + 1 < starts.size() ? starts[i + 1] : json.size();
		std::string seg = json.substr(starts[i], end - starts[i]);

		std::string t;
		size_t e = 0;
		if (!readValueAfterKey(seg, 0, key, t, e)) continue;
		if (!hasPrefix(t, tagPrefix)) continue;
		size_t pr = seg.find("\"prerelease\"");
		if (pr != std::string::npos) {
			size_t q = seg.find(':', pr);
			if (q != std::string::npos) {
				size_t v = seg.find_first_not_of(" \t\r\n", q + 1);
				if (v != std::string::npos && seg.compare(v, 4, "true") == 0) continue;
			}
		}
		Asset a;
		if (!findAsset(seg, assetPrefix, assetSuffix, a)) continue;
		if (!found || compareVersions(t, tag, tagPrefix) > 0) { tag = t; out = a; found = true; }
	}
	return found;
}

} // namespace Release
