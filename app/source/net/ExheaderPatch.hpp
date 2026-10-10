#pragma once

// Portable (no libctru) helpers for the RvtA boot hook: RvtA is started at boot by listing it as a
// dependency of a sysmodule NS starts, through a Luma exheader override (/luma/titles/<tid>/exheader.bin).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ExheaderPatch {

constexpr size_t kInfoSize = 0x400;        // ExHeader_Info, the size Luma accepts for an override
constexpr size_t kDepsOffset = 0x40;       // u64 dependencies[48] inside it
constexpr size_t kDepsCount = 48;

std::vector<uint64_t> dependencies(const uint8_t* info);
bool hasDependency(const uint8_t* info, uint64_t titleId);

// Puts titleId into the first free dependency slot. Returns false when every slot is taken.
// Already listed counts as success.
bool addDependency(uint8_t* info, uint64_t titleId);

// "/luma/titles/0004013000003502/exheader.bin"
std::string overridePath(uint64_t titleId);

// The first candidate that the boot module (NS) starts directly, or 0.
uint64_t chooseHook(const uint8_t* nsInfo, const std::vector<uint64_t>& candidates);

} // namespace ExheaderPatch
