#pragma once

// Small POSIX file helpers (no libctru), shared by the manager and the host tests.

#include <string>
#include <vector>
#include <cstdint>

namespace Fs {

bool exists(const std::string& path);
bool mkdirs(const std::string& path);                 // like mkdir -p
bool removeTree(const std::string& path);             // like rm -rf (files and one level of subfolders is enough here)
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const uint8_t* data, size_t size);
bool writeText(const std::string& path, const std::string& text);
std::vector<std::string> listNames(const std::string& dir); // names only, no . or ..

} // namespace Fs
