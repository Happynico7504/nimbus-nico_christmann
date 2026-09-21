#include "Installer.hpp"

#include <sys/stat.h>

#include <cstdio>

namespace Installer {

namespace {
struct Entry {
	const char* src;   // file name inside srcDir
	const char* dir;   // directory that must exist ("" = none needed)
	const char* dest;  // absolute destination path
};

const Entry kEntries[] = {
	// sysmodule patches
	{"0004013000003202.ips", "/luma/sysmodules", "/luma/sysmodules/0004013000003202.ips"},
	{"0004013000003802.ips", "/luma/sysmodules", "/luma/sysmodules/0004013000003802.ips"},
	{"0004013000002902.ips", "/luma/sysmodules", "/luma/sysmodules/0004013000002902.ips"},
	{"0004013000002E02.ips", "/luma/sysmodules", "/luma/sysmodules/0004013000002E02.ips"},
	{"0004013000002F02.ips", "/luma/sysmodules", "/luma/sysmodules/0004013000002F02.ips"},
	{"0004013000002C02.ips", "/luma/sysmodules", "/luma/sysmodules/0004013000002C02.ips"},
	// Miiverse applets (JPN / USA / EUR)
	{"000400300000BC02.ips", "/luma/titles/000400300000BC02", "/luma/titles/000400300000BC02/code.ips"},
	{"000400300000BD02.ips", "/luma/titles/000400300000BD02", "/luma/titles/000400300000BD02/code.ips"},
	{"000400300000BE02.ips", "/luma/titles/000400300000BE02", "/luma/titles/000400300000BE02/code.ips"},
	// eShop applet "mint" (USA CE02 / EUR D602 / JPN C602 per 3dbrew)
	{"000400300000CE02.ips", "/luma/titles/000400300000CE02", "/luma/titles/000400300000CE02/code.ips"},
	{"000400300000D602.ips", "/luma/titles/000400300000D602", "/luma/titles/000400300000D602/code.ips"},
	{"000400300000C602.ips", "/luma/titles/000400300000C602", "/luma/titles/000400300000C602/code.ips"},
	// plugin + certificate
	{"nimbus.3gx", "/luma/plugins", "/luma/plugins/nimbus.3gx"},
	{"juxt-prod.pem", "/3ds", "/3ds/juxt-prod.pem"},
};

bool exists(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

// Copy (not move): the cache must survive so an unchanged network can be reinstalled without downloading.
// Writes to a temporary name first so a failed copy never leaves a half-written patch behind.
bool copyFile(const std::string& src, const std::string& dest) {
	FILE* in = std::fopen(src.c_str(), "rb");
	if (!in) return false;
	std::string tmp = dest + ".tmp";
	FILE* out = std::fopen(tmp.c_str(), "wb");
	if (!out) { std::fclose(in); return false; }
	char buf[8192];
	size_t n;
	bool ok = true;
	while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
		if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
	}
	if (std::ferror(in)) ok = false;
	std::fclose(in);
	if (std::fclose(out) != 0) ok = false;
	if (!ok) { std::remove(tmp.c_str()); return false; }
	std::remove(dest.c_str());
	if (std::rename(tmp.c_str(), dest.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
	return true;
}

void mkdirs(const std::string& root, const std::string& absDir) {
	// create every component of absDir under root
	std::string cur = root;
	size_t i = 0;
	while (i < absDir.size()) {
		size_t j = absDir.find('/', i + 1);
		std::string part = absDir.substr(i, j == std::string::npos ? std::string::npos : j - i);
		cur += part;
		if (!cur.empty()) mkdir(cur.c_str(), 0777);
		if (j == std::string::npos) break;
		i = j;
	}
}
} // namespace

const std::vector<std::string>& knownFiles() {
	static const std::vector<std::string> v = [] {
		std::vector<std::string> r;
		for (const auto& e : kEntries) r.push_back(e.src);
		return r;
	}();
	return v;
}

Result install(const std::string& srcDir, const std::string& root) {
	Result res;

	int present = 0;
	for (const auto& e : kEntries)
		if (exists(srcDir + "/" + e.src)) present++;
	if (present == 0) {
		res.error = "No known patch files were found in the download";
		return res;
	}

	for (const auto& e : kEntries) {
		std::string src = srcDir + "/" + e.src;
		std::string dest = root + e.dest;
		bool have = exists(src);
		bool hadOld = exists(dest);

		if (e.dir[0]) mkdirs(root, e.dir);

		if (have) {
			if (!copyFile(src, dest)) {
				res.error = std::string("Could not install ") + e.src;
				return res;
			}
			res.installed++;
		} else if (hadOld) {
			// this network does not ship the patch: remove the previous network's copy
			std::remove(dest.c_str());
			res.removed++;
		}
	}
	res.ok = true;
	return res;
}

} // namespace Installer
