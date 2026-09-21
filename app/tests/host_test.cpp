// Host-side tests for the portable network-switcher modules (no libctru needed).
// Build & run:  make -C app/tests   (needs g++ and zlib; see app/tests/Makefile)
// Optional real-data inputs are passed on the command line; each check is skipped if its file is absent.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "../source/net/Archive.hpp"
#include "../source/net/Fs.hpp"
#include "../source/net/Installer.hpp"
#include "../source/net/Manager.hpp"
#include "../source/net/Services.hpp"
#include "../source/net/State.hpp"
#include "../source/net/Release.hpp"
#include "../source/net/Sources.hpp"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); failures++; } else std::printf("ok:   %s\n", msg); } while (0)

static bool readFile(const std::string& p, std::vector<uint8_t>& out) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}
static std::string readText(const std::string& p) { std::vector<uint8_t> v; readFile(p, v); return std::string(v.begin(), v.end()); }

struct MapSink : Archive::FileSink {
	std::map<std::string, std::vector<uint8_t>> files;
	bool write(const std::string& n, const uint8_t* d, size_t s) override { files[n] = std::vector<uint8_t>(d, d + s); return true; }
};


struct FakeHttp : Manager::HttpClient {
	std::map<std::string, std::vector<uint8_t>> routes;
	std::vector<std::string> requested;
	bool get(const std::string& url, std::vector<uint8_t>& out, std::string& err, size_t maxBytes) override {
		requested.push_back(url);
		auto it = routes.find(url);
		if (it == routes.end()) { err = "404 " + url; return false; }
		if (it->second.size() > maxBytes) { err = "too large"; return false; }
		out = it->second;
		return true;
	}
	int count(const std::string& sub) const { int n = 0; for (auto& u : requested) if (u.find(sub) != std::string::npos) n++; return n; }
};
static std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
static std::string releasesFor(const std::string& tag, const std::string& asset, const std::string& url, const std::string& sha) {
	return "[{\"tag_name\":\"" + tag + "\",\"prerelease\":false,\"assets\":[{\"name\":\"" + asset + "\",\"digest\":\"sha256:" + sha + "\",\"browser_download_url\":\"" + url + "\"}]}]";
}

int main(int argc, char** argv) {
	std::string dir = argc > 1 ? argv[1] : ".";

	// ---- pure logic
	CHECK(Archive::isSafeFileName("0004013000003802.ips"), "safe name accepted");
	CHECK(!Archive::isSafeFileName("../x.ips") && !Archive::isSafeFileName(".hidden") && !Archive::isSafeFileName("a/b") && !Archive::isSafeFileName(""), "unsafe names rejected");
	{
		const char* abc = "abc";
		CHECK(Archive::sha256Hex((const uint8_t*)abc, 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(abc)");
		CHECK(Archive::sha256Hex((const uint8_t*)"", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(empty)");
	}
	CHECK(Release::compareVersions("patches-v1.10.0", "patches-v1.9.0", "patches-v") > 0, "1.10.0 > 1.9.0");
	CHECK(Release::compareVersions("patches-v2", "patches-v2.0.0", "patches-v") == 0, "2 == 2.0.0");
	CHECK(Release::compareVersions("app-v1.0.1", "app-v1.0.2", "app-v") < 0, "1.0.1 < 1.0.2");

	// ---- release list with mixed tags (synthetic, GitHub-shaped)
	std::string json = R"([
	 {"url":"u","tag_name":"app-v1.4.0","prerelease":false,"assets":[{"name":"nimbus.cia","digest":"sha256:aa","browser_download_url":"https://x/app-v1.4.0/nimbus.cia"}]},
	 {"url":"u","tag_name":"patches-v1.9.0","prerelease":false,"assets":[{"name":"nimbus-patches.zip","digest":"sha256:1111","browser_download_url":"https://x/patches-v1.9.0/nimbus-patches.zip"}],"body":"text with \"tag_name\" inside"},
	 {"url":"u","tag_name":"patches-v1.10.0","prerelease":false,"assets":[{"name":"other.txt","browser_download_url":"https://x/o.txt"},{"name":"nimbus-patches.zip","digest":"sha256:2222","browser_download_url":"https://x/patches-v1.10.0/nimbus-patches.zip"}]},
	 {"url":"u","tag_name":"patches-v2.0.0","prerelease":true,"assets":[{"name":"nimbus-patches.zip","digest":"sha256:3333","browser_download_url":"https://x/patches-v2.0.0/nimbus-patches.zip"}]}
	])";
	{
		std::string tag; Release::Asset a;
		bool ok = Release::findLatestByTagPrefix(json, "patches-v", "nimbus-patches", ".zip", tag, a);
		CHECK(ok && tag == "patches-v1.10.0", "picks highest patches-v tag (ignores pre-release and app-v)");
		CHECK(a.url == "https://x/patches-v1.10.0/nimbus-patches.zip" && a.sha256 == "2222", "asset url + digest of that release");
		std::string t2; Release::Asset a2;
		CHECK(Release::findLatestByTagPrefix(json, "app-v", "nimbus", ".cia", t2, a2) && t2 == "app-v1.4.0" && a2.sha256 == "aa", "picks app-v release");
		CHECK(!Release::findLatestByTagPrefix(json, "nothing-v", "x", "", t2, a2), "no match -> false");
	}

	// ---- real data (optional)
	std::vector<uint8_t> zip, tgz;
	if (readFile(dir + "/3dsx.2.1.1.zip", zip)) {
		MapSink s; std::string err;
		bool ok = Archive::extractZip(zip.data(), zip.size(), Sources::kArchiveMarker, s, err);
		CHECK(ok, ("stock zip extracts: " + err).c_str());
		CHECK(s.files.size() == 11 && s.files.count("nimbus.3gx") && s.files.count("0004013000003802.ips") && s.files["juxt-prod.pem"].size() == 1260, "stock zip: 11 files, expected names/sizes");
	}
	if (readFile(dir + "/sdfiles.tar.gz", tgz)) {
		MapSink s; std::string err;
		bool ok = Archive::extractTarGz(tgz.data(), tgz.size(), Sources::kArchiveMarker, s, err);
		CHECK(ok, ("our tar.gz extracts: " + err).c_str());
		CHECK(s.files.count("0004013000002C02.ips") && s.files.count("000400300000D602.ips") && s.files.count("nimbus.3gx"), "tar.gz: nim + mint patches present");
		CHECK(Archive::sha256Hex(tgz.data(), tgz.size()) == "c9268dc26dc13723ff8837ee3bb910ee4d2a0e5f239901efbc442cd2fcb17d5f", "sha256 equals GitHub's asset digest");
		// corruption must be detected
		std::vector<uint8_t> bad = tgz; bad[bad.size() / 2] ^= 0xFF;
		MapSink s2; std::string e2;
		CHECK(!Archive::extractTarGz(bad.data(), bad.size(), Sources::kArchiveMarker, s2, e2), "corrupted tar.gz is rejected");
	}
	if (!zip.empty()) {
		// corrupt a byte inside the data of nimbus.3gx (an entry we DO extract): CRC check must catch it
		std::string needle = std::string(Sources::kArchiveMarker) + "nimbus.3gx";
		size_t at = std::string((const char*)zip.data(), zip.size()).find(needle);
		std::vector<uint8_t> bad = zip;
		if (at != std::string::npos) bad[at + needle.size() + 5000] ^= 0xFF;
		MapSink s; std::string e;
		CHECK(at != std::string::npos && !Archive::extractZip(bad.data(), bad.size(), Sources::kArchiveMarker, s, e), "corrupted zip entry is rejected (inflate/CRC)");
	}
	{
		std::string ghjson = readText(dir + "/latest.json");
		if (!ghjson.empty()) {
			Release::Asset a;
			CHECK(Release::findAsset(ghjson, "3dsx.", ".zip", a) && a.name == "3dsx.2.1.1.zip" && !a.url.empty(), "real GitHub JSON: stock 3dsx asset found");
			CHECK(!a.sha256.empty(), "real GitHub JSON: digest present");
		}
	}

	// ---- Manager: full flow against a fake server (real stock zip)
	if (!zip.empty()) {
		char tmpl[] = "/tmp/nimbus-mgr-XXXXXX";
		std::string root = mkdtemp(tmpl);
		std::string cacheRoot = root + "/cache";
		const Sources::Network* stock = Sources::findById("pretendo");
		std::string assetUrl = "https://example/v2.1.1/3dsx.2.1.1.zip";
		FakeHttp http;
		http.routes[stock->releasesUrl] = bytes(releasesFor("v2.1.1", "3dsx.2.1.1.zip", assetUrl, Archive::sha256Hex(zip.data(), zip.size())));
		http.routes[assetUrl] = zip;

		Manager::Prepared p; std::string err;
		CHECK(Manager::prepare(*stock, http, cacheRoot, p, err) && !p.usedCache && p.version == "v2.1.1", ("first prepare downloads: " + err).c_str());
		CHECK(Fs::exists(cacheRoot + "/pretendo/nimbus.3gx") && Fs::exists(cacheRoot + "/pretendo/version.txt") && !Fs::exists(cacheRoot + "/pretendo.new"), "cache populated, no leftover .new folder");
		int assetGets = http.count("3dsx.2.1.1.zip");
		CHECK(Manager::prepare(*stock, http, cacheRoot, p, err) && p.usedCache && http.count("3dsx.2.1.1.zip") == assetGets, "second prepare reuses the cache (asset not downloaded again)");

		// a newer tag appears -> must download instead of using the cache
		std::string url2 = "https://example/v2.2.0/3dsx.2.2.0.zip";
		http.routes[stock->releasesUrl] = bytes(releasesFor("v2.2.0", "3dsx.2.2.0.zip", url2, Archive::sha256Hex(zip.data(), zip.size())));
		http.routes[url2] = zip;
		std::string latest;
		CHECK(Manager::latestVersion(*stock, http, latest, err) && latest == "v2.2.0" && Manager::isNewer(*stock, "v2.1.1", latest) && !Manager::isNewer(*stock, "v2.2.0", latest), "newer tag detected");
		CHECK(Manager::prepare(*stock, http, cacheRoot, p, err) && !p.usedCache && p.version == "v2.2.0" && http.count("3dsx.2.2.0.zip") == 1, "newer tag is downloaded, not served from cache");
		std::string vt; Fs::readFile(cacheRoot + "/pretendo/version.txt", vt);
		CHECK(vt == "v2.2.0", "cache records the new version");

		// bad checksum -> refuse, keep previous cache
		std::string url3 = "https://example/v2.3.0/3dsx.2.3.0.zip";
		http.routes[stock->releasesUrl] = bytes(releasesFor("v2.3.0", "3dsx.2.3.0.zip", url3, std::string(64, '0')));
		http.routes[url3] = zip;
		CHECK(!Manager::prepare(*stock, http, cacheRoot, p, err) && err.find("checksum") != std::string::npos, "checksum mismatch is refused");
		Fs::readFile(cacheRoot + "/pretendo/version.txt", vt);
		CHECK(vt == "v2.2.0" && Fs::exists(cacheRoot + "/pretendo/nimbus.3gx") && !Fs::exists(cacheRoot + "/pretendo.new"), "failed download leaves the previous cache untouched and cleans up");

		// server error -> failure, cache untouched
		http.routes.erase(stock->releasesUrl);
		CHECK(!Manager::prepare(*stock, http, cacheRoot, p, err), "unreachable server -> error");
		CHECK(Fs::exists(cacheRoot + "/pretendo/nimbus.3gx"), "cache intact after network failure");

		// raw-files source (Roseverse): names mapped, version from latest_version.txt
		const Sources::Network* rose = Sources::findById("roseverse");
		FakeHttp h2;
		h2.routes[rose->versionUrl] = bytes("v1.0.1\n");
		for (const auto& f : rose->rawFiles) h2.routes[rose->rawBase + f.remote] = bytes("data:" + f.remote);
		CHECK(Manager::prepare(*rose, h2, cacheRoot, p, err) && p.version == "v1.0.1", ("raw source prepares: " + err).c_str());
		CHECK(Fs::exists(cacheRoot + "/roseverse/000400300000BD02.ips") && Fs::exists(cacheRoot + "/roseverse/0004013000002902.ips"), "america.ips stored as the USA Miiverse patch name");
		int before = (int)h2.requested.size();
		CHECK(Manager::prepare(*rose, h2, cacheRoot, p, err) && p.usedCache && (int)h2.requested.size() == before + 1, "raw source: unchanged version -> only the version file is fetched");
		h2.routes.erase(rose->rawBase + "juxt-prod.pem");
		h2.routes[rose->versionUrl] = bytes("v1.0.2");
		CHECK(!Manager::prepare(*rose, h2, cacheRoot, p, err), "raw source: one missing file fails the whole update");
		Fs::readFile(cacheRoot + "/roseverse/version.txt", vt);
		CHECK(vt == "v1.0.1", "raw source: old cache kept after partial failure");

		// install straight from the prepared cache into a fake SD root
		std::string sd = root + "/sd";
		Fs::mkdirs(sd);
		Installer::Result ir = Installer::install(cacheRoot + "/pretendo", sd);
		CHECK(ir.ok && ir.installed == 10 && Fs::exists(sd + "/luma/sysmodules/0004013000003802.ips") && Fs::exists(sd + "/luma/plugins/nimbus.3gx") && Fs::exists(sd + "/3ds/juxt-prod.pem"), "stock patches install into /luma and /3ds");

		// ---- Services: assemble files from two providers into a stage and install them
		{
			// pretendo cache = the real stock files; roseverse cache = the fake raw files prepared above
			Services::Selection want = Services::selectionFor("pretendo");
			Services::setProvider(want, "miiverse", "roseverse");
			std::string stage = root + "/stage";
			// the stock cache was overwritten by the failure tests above but still holds the v2.2.0 files
			CHECK(Services::buildStage(want, cacheRoot, stage, err), ("buildStage: " + err).c_str());
			std::string ipsHttp, ipsFriends;
			Fs::readFile(stage + "/0004013000002902.ips", ipsHttp);
			Fs::readFile(stage + "/0004013000003202.ips", ipsFriends);
			CHECK(ipsHttp == "data:0004013000002902.ips", "stage: http comes from Roseverse");
			CHECK(ipsFriends.size() == 29, "stage: friends comes from Pretendo (real 29-byte stock patch)");
			CHECK(!Fs::exists(stage + "/0004013000002C02.ips") && !Fs::exists(stage + "/000400300000CE02.ips"), "stage: eShop Off -> no nim/mint files");
			std::string sd2 = root + "/sd2";
			Fs::mkdirs(sd2 + "/luma/sysmodules");
			Fs::writeText(sd2 + "/luma/sysmodules/0004013000002C02.ips", "OLD-NIM");
			Installer::Result ir2 = Installer::install(stage, sd2);
			std::string got;
			Fs::readFile(sd2 + "/luma/sysmodules/0004013000002902.ips", got);
			CHECK(ir2.ok && got == "data:0004013000002902.ips" && !Fs::exists(sd2 + "/luma/sysmodules/0004013000002C02.ips"), "install: Roseverse http in place, old nim patch removed (eShop Off)");
			// a provider whose cache lacks a needed file is refused, and the stage is cleaned up
			Services::Selection bad = Services::selectionFor("revivetendo");
			CHECK(!Services::buildStage(bad, cacheRoot, root + "/stage2", err) && !Fs::exists(root + "/stage2"), "stage: missing provider files are refused");
		}

		// State round trip
		State st; st.selection["miiverse"] = "roseverse"; st.selection["friends"] = "pretendo"; st.versions["roseverse"] = "v1.0.1"; st.trusted.insert("roseverse"); st.trusted.insert("pretendo");
		State st2 = State::parse(st.serialize());
		CHECK(st2.selection["miiverse"] == "roseverse" && st2.versions["roseverse"] == "v1.0.1" && st2.trusted.size() == 2 && st2.installed(), "state file round-trips");
		CHECK(!State::parse("garbage\n=\nselect.=x\nselect.a=b\r\n").selection.empty() && State::parse("garbage\n=\n").selection.empty(), "state parser tolerates garbage / CRLF");

		std::string cmd = "rm -rf " + root;
		(void)!system(cmd.c_str());
	}

	// ---- installer against a temp root
	{
		char tmpl[] = "/tmp/nimbus-test-XXXXXX";
		std::string root = mkdtemp(tmpl);
		std::string cache = root + "/cache";
		mkdir(cache.c_str(), 0777);
		auto put = [&](const std::string& p, const std::string& c) { std::ofstream f(p, std::ios::binary); f << c; };
		auto get = [&](const std::string& p) { return readText(p); };
		// pre-existing "old network" files
		for (const char* d : {"/luma", "/luma/sysmodules", "/luma/titles", "/luma/titles/000400300000CE02", "/luma/plugins", "/3ds"}) mkdir((root + d).c_str(), 0777);
		put(root + "/luma/sysmodules/0004013000002C02.ips", "OLD-NIM");
		put(root + "/luma/titles/000400300000CE02/code.ips", "OLD-MINT");
		put(root + "/luma/sysmodules/0004013000003802.ips", "OLD-ACT");
		// new network ships act + plugin only
		put(cache + "/0004013000003802.ips", "NEW-ACT");
		put(cache + "/nimbus.3gx", "NEW-PLUGIN");
		put(cache + "/notes.txt", "ignored");
		Installer::Result r = Installer::install(cache, root);
		CHECK(r.ok && r.installed == 2, "install ok, 2 files installed");
		CHECK(get(root + "/luma/sysmodules/0004013000003802.ips") == "NEW-ACT" && get(root + "/luma/plugins/nimbus.3gx") == "NEW-PLUGIN", "new files in place");
		CHECK(get(cache + "/nimbus.3gx") == "NEW-PLUGIN", "cache survives (copy, not move)");
		struct stat st;
		CHECK(stat((root + "/luma/sysmodules/0004013000002C02.ips").c_str(), &st) != 0 && stat((root + "/luma/titles/000400300000CE02/code.ips").c_str(), &st) != 0 && r.removed == 2, "stale patches from previous network removed");
		CHECK(stat((root + "/luma/sysmodules/0004013000003802.ips.tmp").c_str(), &st) != 0, "no temp files left behind");
		// safety: empty/unknown download must not wipe anything
		std::string empty = root + "/empty";
		mkdir(empty.c_str(), 0777);
		put(empty + "/random.txt", "x");
		Installer::Result r2 = Installer::install(empty, root);
		CHECK(!r2.ok && get(root + "/luma/sysmodules/0004013000003802.ips") == "NEW-ACT", "download with no known files installs nothing and removes nothing");
		std::string cmd = "rm -rf " + root;
		(void)!system(cmd.c_str());
	}

	// ---- service selection rules
	{
		Services::Selection d = Services::selectionFor("pretendo");
		CHECK(d.size() == 8 && d["account"] == "pretendo" && d["plugin"] == "pretendo" && d["eshop"] == "off", "Pretendo network: base for everything it ships, eShop off");
		CHECK(Services::networks() == std::vector<std::string>({"pretendo", "revivetendo"}), "two networks: Pretendo (base) and ours");
		CHECK(Services::networkOf(d) == "pretendo", "default selection is on Pretendo");

		Services::Selection ours = Services::selectionFor("revivetendo");
		bool all = true;
		for (const auto& kv : ours) all = all && kv.second == "revivetendo";
		CHECK(ours.size() == 8 && all && Services::networkOf(ours) == "revivetendo", "our network provides every service incl. eShop");
		CHECK(Services::providersFor("miiverse", "revivetendo") == std::vector<std::string>({"revivetendo"}), "our network: services are locked to it");

		auto opts = [](const char* id) { return Services::providersFor(id, "pretendo"); };
		CHECK(opts("eshop") == std::vector<std::string>({"off"}), "on Pretendo the eShop service is unavailable (ours only)");
		CHECK(opts("friends") == std::vector<std::string>({"pretendo"}), "on Pretendo, friends has no overlay");
		CHECK(opts("miiverse") == std::vector<std::string>({"pretendo", "roseverse"}), "on Pretendo, Miiverse offers the Roseverse overlay");

		Services::Selection s1 = d;
		Services::setProvider(s1, "miiverse", "roseverse");
		CHECK(s1["account"] == "roseverse" && s1["http"] == "roseverse" && s1["miiverse"] == "roseverse" && s1["friends"] == "pretendo", "Roseverse is a bundle: account + http + miiverse move together");
		Services::setProvider(s1, "http", "pretendo");
		CHECK(s1["account"] == "pretendo" && s1["http"] == "pretendo" && s1["miiverse"] == "pretendo", "moving one member away moves the whole bundle back to Pretendo");
		Services::setProvider(s1, "friends", "roseverse");
		CHECK(s1["friends"] == "pretendo", "an overlay that does not ship a service cannot be chosen for it");
		Services::Selection o2 = ours;
		Services::setProvider(o2, "miiverse", "roseverse");
		CHECK(o2 == ours, "our full network cannot be mixed with overlays");

		Services::Selection junk = {{"friends", "roseverse"}, {"eshop", "pretendo"}, {"nonsense", "x"}, {"ssl", "pretendo"}};
		Services::Selection n = Services::normalized(junk);
		CHECK(n["friends"] == "pretendo" && n["eshop"] == "off" && !n.count("nonsense"), "normalized: invalid entries fall back to defaults");
		Services::Selection mixed = d;
		mixed["ssl"] = "revivetendo";
		CHECK(Services::normalized(mixed) == ours, "normalized: any use of the full network makes the whole selection ours");

		Services::Selection u = d;
		Services::setProvider(u, "miiverse", "roseverse");
		CHECK(Services::providersUsed(u) == std::vector<std::string>({"roseverse", "pretendo"}), "providersUsed: unique, in service order (Account is first), no off");
		CHECK(Services::providersUsed(ours) == std::vector<std::string>({"revivetendo"}), "providersUsed: full network needs only itself");
	}

	std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
