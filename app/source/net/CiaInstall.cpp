#include "CiaInstall.hpp"

#include <3ds.h>

#include <algorithm>
#include <cstdio>

namespace CiaInstall {

static std::string hex(Result rc) {
	char b[32];
	std::snprintf(b, sizeof b, "0x%08lX", (unsigned long)rc);
	return b;
}

bool install(const std::vector<uint8_t>& cia, std::string& err) {
	Result rc = amInit();
	if (R_FAILED(rc)) { err = "AM service unavailable " + hex(rc); return false; }

	Handle handle = 0;
	rc = AM_StartCiaInstall(MEDIATYPE_SD, &handle);
	if (R_FAILED(rc)) { amExit(); err = "Could not start install " + hex(rc); return false; }

	u64 offset = 0;
	const u32 chunk = 0x20000;
	while (offset < cia.size()) {
		u32 want = (u32)std::min<u64>(chunk, cia.size() - offset);
		u32 written = 0;
		rc = FSFILE_Write(handle, &written, offset, cia.data() + offset, want, FS_WRITE_FLUSH);
		if (R_FAILED(rc) || written != want) {
			AM_CancelCIAInstall(handle);
			amExit();
			err = "Write failed " + hex(rc);
			return false;
		}
		offset += written;
	}

	rc = AM_FinishCiaInstall(handle);
	amExit();
	if (R_FAILED(rc)) { err = "Install failed " + hex(rc); return false; }
	return true;
}

} // namespace CiaInstall
