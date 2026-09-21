#pragma once

// Portable (no libctru) archive helpers used by the network switcher.
// Everything here is unit-tested on the host (see app/tests/).

#include <cstddef>
#include <cstdint>
#include <string>

namespace Archive {

// Receives each extracted file. Return false to abort extraction.
struct FileSink {
	virtual ~FileSink() = default;
	virtual bool write(const std::string& name, const uint8_t* data, size_t size) = 0;
};

// A file name is "safe" if it is a plain name we would be willing to write into
// the cache folder: [A-Za-z0-9._-], not starting with '.', at most 64 chars.
bool isSafeFileName(const std::string& name);

// Extract regular files that live directly inside a folder called `marker`
// (e.g. "3ds/nimbus/update/") anywhere in the archive. `marker` must end in '/'.
// Only the bare file name (after the marker) is handed to the sink; entries
// with anything else after the marker, or unsafe names, are skipped.
bool extractZip(const uint8_t* data, size_t size, const std::string& marker, FileSink& sink, std::string& err);
bool extractTarGz(const uint8_t* data, size_t size, const std::string& marker, FileSink& sink, std::string& err,
                  size_t maxUnpacked = 32u * 1024u * 1024u);

// Lower-case hex SHA-256.
std::string sha256Hex(const uint8_t* data, size_t size);

} // namespace Archive
