// Live end-to-end test: the real HTTP client against the real GitHub endpoints.
// Runs the complete flow the app runs on the console (check the newest tag, download, verify the digest,
// extract, stage, install into a fake SD card) for all three choices.
//   make -C app/tests live      (needs g++, zlib and libcurl headers, and internet access)
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../source/net/AppUpdate.hpp"
#include "../source/net/CurlHttp.hpp"
#include "../source/net/Fs.hpp"
#include "../source/net/Installer.hpp"
#include "../source/net/Manager.hpp"
#include "../source/net/Services.hpp"
#include "../source/net/Sources.hpp"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s  (line %d)\n", msg, __LINE__); failures++; } else std::printf("ok:   %s\n", msg); } while (0)

int main() {
	char tmpl[] = "/tmp/nimbus-live-XXXXXX";
	std::string root = mkdtemp(tmpl);
	std::string cacheRoot = root + "/cache";

	Net::CurlHttp http;
	CHECK(http.ok(), "HTTP client starts");

	// newest app release, compared with two running versions
	AppUpdate::Info info; std::string err;
	CHECK(AppUpdate::check(http, 2, 0, 0, info, err) && info.newer && info.tag.rfind("app-v", 0) == 0, ("app update check finds " + info.tag + (err.empty() ? "" : " / " + err)).c_str());
	AppUpdate::Info same;
	CHECK(AppUpdate::check(http, 99, 0, 0, same, err) && !same.newer, "an app newer than the release is not offered an update");

	// each provider: newest version, real download, digest check, extraction
	for (const char* id : {"revivetendo", "pretendo", "roseverse"}) {
		const Sources::Network* n = Sources::findById(id);
		Manager::Prepared p;
		bool ok = Manager::prepare(*n, http, cacheRoot, p, err);
		CHECK(ok, (std::string("prepare ") + id + ": " + (ok ? p.version : err)).c_str());
		Manager::Prepared again;
		CHECK(Manager::prepare(*n, http, cacheRoot, again, err) && again.usedCache && again.version == p.version, (std::string(id) + ": second prepare reuses the saved copy").c_str());
	}

	// the three choices, installed into a fake SD card
	for (const char* preset : {"revivetendo", "pretendo", "roseverse"}) {
		std::string sd = root + std::string("/sd-") + preset;
		Fs::mkdirs(sd);
		Services::Selection sel = Services::selectionForPreset(preset);
		std::string stage = root + "/stage";
		bool staged = Services::buildStage(sel, cacheRoot, stage, err);
		CHECK(staged, (std::string("stage ") + preset + (staged ? "" : ": " + err)).c_str());
		Installer::Result r = Installer::install(stage, sd);
		std::printf("      %s: %d installed, %d removed\n", preset, r.installed, r.removed);
		CHECK(r.ok, (std::string("install ") + preset).c_str());
		bool nim = Fs::exists(sd + "/luma/sysmodules/0004013000002C02.ips");
		bool friends = Fs::exists(sd + "/luma/sysmodules/0004013000003202.ips");
		CHECK(std::string(preset) == "revivetendo" ? (nim && r.installed == 14) : (!nim && friends), (std::string(preset) + ": expected set of patches (nim/mint only on ours)").c_str());
	}
	// switching from ours to Pretendo removes ours-only patches
	{
		std::string sd = root + "/sd-switch";
		Fs::mkdirs(sd);
		Services::buildStage(Services::selectionForPreset("revivetendo"), cacheRoot, root + "/s1", err);
		Installer::install(root + "/s1", sd);
		Services::buildStage(Services::selectionForPreset("pretendo"), cacheRoot, root + "/s2", err);
		Installer::Result r = Installer::install(root + "/s2", sd);
		CHECK(r.ok && r.removed == 4 && !Fs::exists(sd + "/luma/sysmodules/0004013000002C02.ips") && !Fs::exists(sd + "/luma/titles/000400300000CE02/code.ips"), "switching Ours -> Pretendo removes the 4 nim/mint patches");
	}

	std::string cmd = "rm -rf " + root;
	(void)!system(cmd.c_str());
	std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
