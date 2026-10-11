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

// fs:REG normally only has sessions for PM and loader. srv would make us wait for a free one forever
// (which hung the app at startup), so ask without blocking and, when it is full, borrow an existing
// client session through Luma's svcControlService (SERVICEOP_STEAL_CLIENT_SESSION), like Rosalina does
// for mcu::HWC.
bool fsRegBorrowed = false;

Result stealSession(Handle* out, const char* name) {
	register u32 r0 asm("r0") = 0; // SERVICEOP_STEAL_CLIENT_SESSION
	register Handle* r1 asm("r1") = out;
	register const char* r2 asm("r2") = name;
	asm volatile("svc 0xB0" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "r12", "lr", "memory");
	return (Result)r0;
}

Result openFsReg() {
	srvSetBlockingPolicy(true);
	Result rc = fsRegInit();
	srvSetBlockingPolicy(false);
	if (R_SUCCEEDED(rc)) { fsRegBorrowed = false; return 0; }
	Handle h = 0;
	Result rc2 = stealSession(&h, "fs:REG");
	if (R_SUCCEEDED(rc2) && h != 0) {
		*fsRegGetSessionHandle() = h;
		fsRegBorrowed = true;
		return 0;
	}
	return R_FAILED(rc2) ? rc2 : rc;
}

void closeFsReg() {
	if (fsRegBorrowed) {
		svcCloseHandle(*fsRegGetSessionHandle());
		*fsRegGetSessionHandle() = 0;
		fsRegBorrowed = false;
	} else {
		fsRegExit();
	}
}

// Returns 0 or the failing result; `step` says which call failed (for rvta-hook.txt).
Result readExheader(u64 tid, ExHeader_Info& out, const char** step = nullptr) {
	FS_ProgramInfo pi = {};
	pi.programId = tid;
	pi.mediaType = MEDIATYPE_NAND;
	u64 handle = 0;
	Result rc = FSREG_LoadProgram(&handle, &pi);
	if (R_FAILED(rc)) { if (step) *step = "LoadProgram"; return rc; }
	rc = FSREG_GetProgramInfo(&out, 1, handle);
	FSREG_UnloadProgram(handle);
	if (R_FAILED(rc) && step) *step = "GetProgramInfo";
	return rc;
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
	if (Result rc = openFsReg(); R_FAILED(rc)) {
		Fs::writeText(kStatePath, std::format("hook=0\nfs:REG unavailable: {:08X}\n", (u32)rc));
		return std::format("Analytics: not started (fs:REG {:08X})", (u32)rc);
	}

	std::string log = std::format("fs:REG: {}\n", fsRegBorrowed ? "borrowed session" : "own session");
	ExHeader_Info ns;
	bool haveNs = false;
	for (u64 tid : kNs) {
		const char* step = "";
		Result rc = readExheader(tid, ns, &step);
		log += std::format("NS {:016X}: {}\n", tid, R_SUCCEEDED(rc) ? std::string("ok") : std::format("{} {:08X}", step, (u32)rc));
		if ((haveNs = R_SUCCEEDED(rc))) break;
	}
	if (!haveNs) {
		closeFsReg();
		Fs::writeText(kStatePath, "hook=0\n" + log);
		return "Analytics: not started (could not read NS)";
	}
	const uint8_t* nsBytes = reinterpret_cast<const uint8_t*>(&ns);

	std::string list;
	for (u64 d : ExheaderPatch::dependencies(nsBytes)) list += std::format("{:016X}\n", d);

	const u64 current = hookedTitle();
	for (const auto& c : kCandidates) {
		if (!ExheaderPatch::hasDependency(nsBytes, c.tid)) continue;
		const std::string path = ExheaderPatch::overridePath(c.tid);
		if (Fs::exists(path) && c.tid != current) continue; // someone else's override: leave it alone

		ExHeader_Info e;
		const char* step = "";
		if (Result rc = readExheader(c.tid, e, &step); R_FAILED(rc)) {
			log += std::format("{}: {} {:08X}\n", c.name, step, (u32)rc);
			continue;
		}
		uint8_t* bytes = reinterpret_cast<uint8_t*>(&e);
		if (!ExheaderPatch::addDependency(bytes, kRvta)) continue;

		const std::string dir = path.substr(0, path.rfind('/'));
		if (!Fs::mkdirs(dir) || !Fs::writeFile(path, bytes, sizeof e)) {
			log += std::format("{}: could not write {}\n", c.name, path);
			continue;
		}
		if (current && current != c.tid) std::remove(ExheaderPatch::overridePath(current).c_str());
		Fs::writeText(kStatePath, std::format("hook={:016X}\n{}ns-dependencies:\n{}", c.tid, log, list));
		closeFsReg();
		return std::format("Analytics: on (started with {})", c.name);
	}
	closeFsReg();
	removeHook();
	Fs::writeText(kStatePath, "hook=0\n" + log + "ns-dependencies:\n" + list); // keep NS's list for diagnosis
	return "Analytics: not started (no suitable boot module)";
}

} // namespace

std::string sync() {
	if (Fs::exists(kCxiPath)) return install();
	removeHook();
	Fs::writeText(kStatePath, std::format("hook=0\nRvtA is not installed ({} not found)\n", kCxiPath));
	return "";
}

} // namespace RvtaHook
