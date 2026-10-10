#include "ExheaderPatch.hpp"

#include <cstdio>
#include <cstring>

namespace ExheaderPatch {

static uint64_t rd64(const uint8_t* p) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
	return v;
}

static void wr64(uint8_t* p, uint64_t v) {
	for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

std::vector<uint64_t> dependencies(const uint8_t* info) {
	std::vector<uint64_t> out;
	for (size_t i = 0; i < kDepsCount; i++) {
		uint64_t d = rd64(info + kDepsOffset + 8 * i);
		if (d) out.push_back(d);
	}
	return out;
}

bool hasDependency(const uint8_t* info, uint64_t titleId) {
	for (size_t i = 0; i < kDepsCount; i++)
		if (rd64(info + kDepsOffset + 8 * i) == titleId) return true;
	return false;
}

bool addDependency(uint8_t* info, uint64_t titleId) {
	if (hasDependency(info, titleId)) return true;
	for (size_t i = 0; i < kDepsCount; i++) {
		uint8_t* slot = info + kDepsOffset + 8 * i;
		if (rd64(slot) == 0) {
			wr64(slot, titleId);
			return true;
		}
	}
	return false;
}

std::string overridePath(uint64_t titleId) {
	char b[64];
	std::snprintf(b, sizeof b, "/luma/titles/%016llX/exheader.bin", (unsigned long long)titleId);
	return b;
}

uint64_t chooseHook(const uint8_t* nsInfo, const std::vector<uint64_t>& candidates) {
	for (uint64_t c : candidates)
		if (hasDependency(nsInfo, c)) return c;
	return 0;
}

} // namespace ExheaderPatch
