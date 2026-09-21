#include "Archive.hpp"

#include <zlib.h>

#include <cstring>
#include <vector>

namespace Archive {

bool isSafeFileName(const std::string& name) {
	if (name.empty() || name.size() > 64 || name[0] == '.') return false;
	for (char c : name) {
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		if (!ok) return false;
	}
	return name.find("..") == std::string::npos;
}

// If `path` contains `marker` at a folder boundary, return the remainder (which must be a safe bare file name).
static bool fileNameUnderMarker(const std::string& path, const std::string& marker, std::string& out) {
	size_t p = path.find(marker);
	while (p != std::string::npos) {
		if (p == 0 || path[p - 1] == '/') {
			std::string rest = path.substr(p + marker.size());
			if (rest.empty() || rest.find('/') != std::string::npos || !isSafeFileName(rest)) return false;
			out = rest;
			return true;
		}
		p = path.find(marker, p + 1);
	}
	return false;
}

// ---------------------------------------------------------------- zip

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static constexpr size_t kMaxEntrySize = 8u * 1024u * 1024u;

bool extractZip(const uint8_t* data, size_t size, const std::string& marker, FileSink& sink, std::string& err) {
	if (size < 22) { err = "zip too small"; return false; }

	// End Of Central Directory record: scan backwards (a comment may follow it).
	size_t eocd = std::string::npos;
	size_t lowest = size > 22 + 65535 ? size - 22 - 65535 : 0;
	for (size_t i = size - 22 + 1; i-- > lowest;) {
		if (rd32(data + i) == 0x06054b50) { eocd = i; break; }
	}
	if (eocd == std::string::npos) { err = "zip: no end-of-central-directory"; return false; }

	uint16_t count = rd16(data + eocd + 10);
	uint32_t cdSize = rd32(data + eocd + 12);
	uint32_t cdOff = rd32(data + eocd + 16);
	if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) { err = "zip64 not supported"; return false; }
	if ((uint64_t)cdOff + cdSize > size) { err = "zip: bad central directory"; return false; }

	size_t p = cdOff;
	for (uint16_t n = 0; n < count; n++) {
		if (p + 46 > size || rd32(data + p) != 0x02014b50) { err = "zip: bad central entry"; return false; }
		uint16_t flags = rd16(data + p + 8);
		uint16_t method = rd16(data + p + 10);
		uint32_t crc = rd32(data + p + 16);
		uint32_t csize = rd32(data + p + 20);
		uint32_t usize = rd32(data + p + 24);
		uint16_t nlen = rd16(data + p + 28);
		uint16_t xlen = rd16(data + p + 30);
		uint16_t clen = rd16(data + p + 32);
		uint32_t lho = rd32(data + p + 42);
		if (p + 46 + nlen > size) { err = "zip: truncated name"; return false; }
		std::string name((const char*)data + p + 46, nlen);
		p += 46 + (size_t)nlen + xlen + clen;

		std::string bare;
		if (!fileNameUnderMarker(name, marker, bare)) continue;

		if (flags & 1) { err = "zip: encrypted entry " + bare; return false; }
		if (usize > kMaxEntrySize || csize > kMaxEntrySize) { err = "zip: entry too large " + bare; return false; }
		if ((uint64_t)lho + 30 > size || rd32(data + lho) != 0x04034b50) { err = "zip: bad local header " + bare; return false; }
		size_t dataOff = (size_t)lho + 30 + rd16(data + lho + 26) + rd16(data + lho + 28);
		if ((uint64_t)dataOff + csize > size) { err = "zip: truncated data " + bare; return false; }

		std::vector<uint8_t> out(usize);
		if (method == 0) {
			if (csize != usize) { err = "zip: stored size mismatch " + bare; return false; }
			if (usize) memcpy(out.data(), data + dataOff, usize);
		} else if (method == 8) {
			z_stream zs;
			memset(&zs, 0, sizeof zs);
			if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { err = "zip: inflate init"; return false; }
			zs.next_in = (Bytef*)(data + dataOff);
			zs.avail_in = csize;
			zs.next_out = out.data();
			zs.avail_out = usize;
			int rc = usize ? inflate(&zs, Z_FINISH) : Z_STREAM_END;
			uLong produced = zs.total_out;
			inflateEnd(&zs);
			if (rc != Z_STREAM_END || produced != usize) { err = "zip: inflate failed for " + bare; return false; }
		} else {
			err = "zip: unsupported compression for " + bare;
			return false;
		}

		if (crc32(0L, out.data(), (uInt)out.size()) != crc) { err = "zip: CRC mismatch for " + bare; return false; }
		if (!sink.write(bare, out.data(), out.size())) { err = "write failed for " + bare; return false; }
	}
	return true;
}

// ---------------------------------------------------------------- tar.gz

static bool gunzip(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t maxOut, std::string& err) {
	z_stream zs;
	memset(&zs, 0, sizeof zs);
	if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) { err = "gzip: init failed"; return false; }
	zs.next_in = (Bytef*)src;
	zs.avail_in = (uInt)n;
	std::vector<uint8_t> buf(16384);
	int rc;
	do {
		zs.next_out = buf.data();
		zs.avail_out = (uInt)buf.size();
		rc = inflate(&zs, Z_NO_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&zs); err = "gzip: corrupt data"; return false; }
		out.insert(out.end(), buf.begin(), buf.begin() + (buf.size() - zs.avail_out));
		if (out.size() > maxOut) { inflateEnd(&zs); err = "gzip: unpacked data too large"; return false; }
		if (rc == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) { inflateEnd(&zs); err = "gzip: truncated"; return false; }
	} while (rc != Z_STREAM_END);
	inflateEnd(&zs);
	return true;
}

// strnlen is not available in strict C++20 on newlib, so use a tiny local version.
static size_t boundedLen(const char* p, size_t max) {
	size_t n = 0;
	while (n < max && p[n]) n++;
	return n;
}

static bool tarOctal(const uint8_t* p, size_t len, uint64_t& v) {
	size_t i = 0;
	while (i < len && (p[i] == ' ' || p[i] == 0)) i++;
	v = 0;
	bool any = false;
	for (; i < len && p[i] >= '0' && p[i] <= '7'; i++) { v = (v << 3) | (uint64_t)(p[i] - '0'); any = true; }
	return any;
}

bool extractTarGz(const uint8_t* data, size_t size, const std::string& marker, FileSink& sink, std::string& err, size_t maxUnpacked) {
	std::vector<uint8_t> tar;
	if (!gunzip(data, size, tar, maxUnpacked, err)) return false;

	size_t pos = 0;
	while (pos + 512 <= tar.size()) {
		const uint8_t* h = tar.data() + pos;
		bool zero = true;
		for (int i = 0; i < 512; i++) if (h[i]) { zero = false; break; }
		if (zero) break;

		uint64_t chk = 0;
		if (!tarOctal(h + 148, 8, chk)) { err = "tar: bad checksum field"; return false; }
		uint64_t sum = 0;
		for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
		if (sum != chk) { err = "tar: header checksum mismatch"; return false; }

		uint64_t fsize = 0;
		if (!tarOctal(h + 124, 12, fsize)) { err = "tar: bad size field"; return false; }
		char type = (char)h[156];

		std::string name((const char*)h, boundedLen((const char*)h, 100));
		if (memcmp(h + 257, "ustar", 5) == 0) {
			std::string prefix((const char*)h + 345, boundedLen((const char*)h + 345, 155));
			if (!prefix.empty()) name = prefix + "/" + name;
		}

		size_t dataOff = pos + 512;
		if (fsize > tar.size() || dataOff + fsize > tar.size()) { err = "tar: truncated file " + name; return false; }

		if (type == '0' || type == 0) {
			std::string bare;
			if (fileNameUnderMarker(name, marker, bare) && fsize <= kMaxEntrySize) {
				if (!sink.write(bare, tar.data() + dataOff, (size_t)fsize)) { err = "write failed for " + bare; return false; }
			}
		}
		pos = dataOff + (size_t)((fsize + 511) / 512) * 512;
	}
	return true;
}

// ---------------------------------------------------------------- sha256

namespace {
struct Sha256 {
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint8_t buf[64];
	size_t buflen = 0;
	uint64_t total = 0;

	static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

	void block(const uint8_t* p) {
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		uint32_t w[64];
		for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = hh + S1 + ch + k[i] + w[i];
			uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = S0 + mj;
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}

	void update(const uint8_t* p, size_t n) {
		total += n;
		while (n) {
			size_t take = 64 - buflen < n ? 64 - buflen : n;
			memcpy(buf + buflen, p, take);
			buflen += take; p += take; n -= take;
			if (buflen == 64) { block(buf); buflen = 0; }
		}
	}

	std::string finish() {
		uint64_t bits = total * 8;
		uint8_t pad = 0x80;
		update(&pad, 1);
		uint8_t z = 0;
		while (buflen != 56) update(&z, 1);
		uint8_t len[8];
		for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
		update(len, 8);
		static const char* hex = "0123456789abcdef";
		std::string s;
		for (int i = 0; i < 8; i++)
			for (int j = 3; j >= 0; j--) { uint8_t b = (uint8_t)(h[i] >> (j * 8)); s += hex[b >> 4]; s += hex[b & 15]; }
		return s;
	}
};
} // namespace

std::string sha256Hex(const uint8_t* data, size_t size) {
	Sha256 s;
	s.update(data, size);
	return s.finish();
}

} // namespace Archive
