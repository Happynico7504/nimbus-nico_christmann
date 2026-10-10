#include "RvtaHook.hpp"

#include <3ds.h>

#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "ExheaderPatch.hpp"
#include "Fs.hpp"

namespace RvtaHook {

namespace {

constexpr u64 kRvta = 0x0004013000F7A002ULL;
constexpr const char* kStatePath = "/3ds/nimbus/rvta-hook.txt"; // which override Nimbus wrote (+ NS's list, for diagnosis)

// Modules NS starts at boot that nothing important waits on and that are not RvtA's own dependencies.
struct Candidate { u64 tid; const char* name; };
const Candidate kCandidates[] = {
	{0x0004013000003502ULL, "news"},
	{0x0004013000002602ULL, "cecd"},
	{0x0004013000003402ULL, "boss"},
};

const u64 kNs[] = {0x0004013000008002ULL, 0x0004013020008002ULL};

bool readExheader(u64 tid, ExHeader_Info& out) {
	FS_ProgramInfo pi = {};
	pi.programId = tid;
	pi.mediaType = MEDIATYPE_NAND;
	u64 handle = 0;
	if (R_FAILED(FSREG_LoadProgram(&handle, &pi))) return false;
	Result rc = FSREG_GetProgramInfo(&out, 1, handle);
	FSREG_UnloadProgram(handle);
	return R_SUCCEEDED(rc);
}

u64 hookedTitle() {
	std::string s;
	if (!Fs::readFile(kStatePath, s)) return 0;
	unsigned long long v = 0;
	if (std::sscanf(s.c_str(), "hook=%llx", &v) != 1) return 0;
	return v;
}

void removeHook() {
	if (u64 old = hookedTitle()) std::remove(ExheaderPatch::overridePath(old).c_str());
	std::remove(kStatePath);
}

std::string install() {
	if (R_FAILED(fsRegInit())) return "Analytics: not started (fs:REG unavailable)";

	ExHeader_Info ns;
	bool haveNs = false;
	for (u64 tid : kNs)
		if ((haveNs = readExheader(tid, ns))) break;
	if (!haveNs) { fsRegExit(); return "Analytics: not started (could not read NS)"; }
	const uint8_t* nsBytes = reinterpret_cast<const uint8_t*>(&ns);

	std::string list;
	for (u64 d : ExheaderPatch::dependencies(nsBytes)) list += std::format("{:016X}\n", d);

	const u64 current = hookedTitle();
	for (const auto& c : kCandidates) {
		if (!ExheaderPatch::hasDependency(nsBytes, c.tid)) continue;
		const std::string path = ExheaderPatch::overridePath(c.tid);
		if (Fs::exists(path) && c.tid != current) continue; // someone else's override: leave it alone

		ExHeader_Info e;
		if (!readExheader(c.tid, e)) continue;
		uint8_t* bytes = reinterpret_cast<uint8_t*>(&e);
		if (!ExheaderPatch::addDependency(bytes, kRvta)) continue;

		const std::string dir = path.substr(0, path.rfind('/'));
		if (!Fs::mkdirs(dir) || !Fs::writeFile(path, bytes, sizeof e)) continue;
		if (current && current != c.tid) std::remove(ExheaderPatch::overridePath(current).c_str());
		Fs::writeText(kStatePath, std::format("hook={:016X}\nns-dependencies:\n{}", c.tid, list));
		fsRegExit();
		return std::format("Analytics: on (started with {})", c.name);
	}
	fsRegExit();
	removeHook();
	Fs::writeText(kStatePath, "hook=0\nns-dependencies:\n" + list); // keep NS's list for diagnosis
	return "Analytics: not started (no suitable boot module)";
}

} // namespace

std::string sync() {
	if (Fs::exists(kCxiPath)) return install();
	removeHook();
	return "";
}

} // namespace RvtaHook
