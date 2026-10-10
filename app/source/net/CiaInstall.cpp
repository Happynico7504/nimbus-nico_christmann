#include "CiaInstall.hpp"

#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <format>

namespace CiaInstall {

static std::string hex(Result rc) {
	char b[32];
	std::snprintf(b, sizeof b, "0x%08lX", (unsigned long)rc);
	return b;
}

// Keep the last failure on the SD card too, so the error code survives a reboot.
static void saveError(const std::string& err) {
	if (FILE* f = std::fopen("/3ds/nimbus/update-error.txt", "w")) {
		std::fprintf(f, "%s\n", err.c_str());
		std::fclose(f);
	}
}

// One full attempt. `overwrite` uses AM's mode for replacing a title with the same title ID,
// which is what a self-update is.
static bool attempt(const std::vector<uint8_t>& cia, bool overwrite, std::string& err) {
	Handle handle = 0;
	Result rc = overwrite ? AM_StartCiaInstallOverwrite(&handle, MEDIATYPE_SD) : AM_StartCiaInstall(MEDIATYPE_SD, &handle);
	if (R_FAILED(rc)) { err = "Could not start install " + hex(rc); return false; }

	u64 offset = 0;
	const u32 chunk = 0x20000;
	while (offset < cia.size()) {
		u32 want = (u32)std::min<u64>(chunk, cia.size() - offset);
		u32 written = 0;
		rc = FSFILE_Write(handle, &written, offset, cia.data() + offset, want, 0);
		if (R_FAILED(rc) || written != want) {
			AM_CancelCIAInstall(handle);
			err = std::format("Write failed {} at {}/{} (wrote {} of {})", hex(rc), offset, cia.size(), written, want);
			return false;
		}
		offset += written;
	}

	rc = AM_FinishCiaInstall(handle);
	if (R_FAILED(rc)) { err = "Install failed " + hex(rc); return false; }
	return true;
}

bool install(const std::vector<uint8_t>& cia, std::string& err) {
	Result rc = amInit();
	if (R_FAILED(rc)) { err = "AM service unavailable " + hex(rc); saveError(err); return false; }

	// Some consoles have no SD title database yet; AM then refuses the install.
	bool available = false;
	if (R_SUCCEEDED(AM_QueryAvailableExternalTitleDatabase(&available)) && !available)
		AM_InitializeExternalTitleDatabase(false);

	// Overwrite mode first, then the plain install once more if that did not work.
	std::string first;
	bool ok = attempt(cia, true, first);
	if (!ok) {
		std::string second;
		ok = attempt(cia, false, second);
		if (!ok) err = first + "\n" + second;
	}
	amExit();
	if (!ok) saveError(err);
	return ok;
}

} // namespace CiaInstall
