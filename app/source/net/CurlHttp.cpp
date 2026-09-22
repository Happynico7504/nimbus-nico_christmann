// newlib hides fd_set (needed by curl/multi.h) under strict -std=c++20 unless the default POSIX names are enabled
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "CurlHttp.hpp"

#include <sys/select.h>
#include <sys/types.h>

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>

#ifdef __3DS__
#include <3ds.h>
#include <malloc.h>
#include "cacert_bin.h" // the CA bundle from app/data/cacert.bin, linked into the executable
#else
#include <chrono>
#include <thread>
#endif

namespace Net {

namespace {

struct Sink {
	std::vector<uint8_t>* out;
	size_t max;
	bool overflow = false;
};

size_t onData(char* p, size_t size, size_t n, void* user) {
	Sink* s = (Sink*)user;
	size_t bytes = size * n;
	if (s->out->size() + bytes > s->max) { s->overflow = true; return 0; } // abort: larger than expected
	s->out->insert(s->out->end(), (uint8_t*)p, (uint8_t*)p + bytes);
	return bytes;
}

#ifdef __3DS__
u32* socBuffer = nullptr;
#endif
int users = 0;

bool acquire() {
	if (users == 0) {
#ifdef __3DS__
		socBuffer = (u32*)memalign(0x1000, 0x100000);
		if (!socBuffer || R_FAILED(socInit(socBuffer, 0x100000))) { free(socBuffer); socBuffer = nullptr; return false; }
#endif
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
	}
	users++;
	return true;
}

void release() {
	if (--users == 0) {
		curl_global_cleanup();
#ifdef __3DS__
		socExit();
		free(socBuffer);
		socBuffer = nullptr;
#endif
	}
}

std::string num(long v) { return std::to_string(v); }

// Transient-looking failures only: a transport-level error (timeout, connection reset, DNS
// hiccup - exactly the "network flakiness" a retry can paper over) or a 5xx from the server.
// A 4xx or "download is larger than expected" won't be fixed by asking again, so those return
// straight from get() without spending the retries.
bool isTransient(bool curlFailed, long status) {
	return curlFailed || (status >= 500 && status < 600);
}

} // namespace

CurlHttp::CurlHttp() { ok_ = acquire(); }

CurlHttp::~CurlHttp() {
	if (ok_) release();
}

bool CurlHttp::certificateProblem() const {
	// 60 peer certificate cannot be authenticated, 51/58/59/77/83 other certificate problems
	return lastCurlCode == CURLE_PEER_FAILED_VERIFICATION || lastCurlCode == CURLE_SSL_CERTPROBLEM || lastCurlCode == CURLE_SSL_CACERT_BADFILE ||
	       lastCurlCode == 59 || lastCurlCode == 83;
}

bool CurlHttp::get(const std::string& url, std::vector<uint8_t>& out, std::string& err, size_t maxBytes) {
	constexpr int kAttempts = 3;
	for (int attempt = 1; attempt <= kAttempts; attempt++) {
		bool ok = getOnce(url, out, err, maxBytes);
		if (ok || attempt == kAttempts || !isTransient(lastCurlCode != 0, lastStatus_)) return ok;
#ifdef __3DS__
		svcSleepThread(500'000'000LL * attempt); // 0.5s, 1s - back off a little more each retry
#else
		std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
#endif
	}
	return false; // unreachable, kept for clarity
}

bool CurlHttp::getOnce(const std::string& url, std::vector<uint8_t>& out, std::string& err, size_t maxBytes) {
	lastCurlCode = 0;
	lastStatus_ = 0;
	out.clear();
	if (!ok_) { err = "Network could not be started"; return false; }

	CURL* c = curl_easy_init();
	if (!c) { err = "Network could not be started"; return false; }

	Sink sink{&out, maxBytes};
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_USERAGENT, "Nimbus-Updater");
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, verifyTls ? 1L : 0L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, verifyTls ? 2L : 0L);
#ifdef __3DS__
	if (verifyTls) {
		struct curl_blob blob;
		blob.data = (void*)cacert_bin;
		blob.len = cacert_bin_size;
		blob.flags = CURL_BLOB_COPY;
		curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
	}
#endif
	struct curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json, */*");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

	CURLcode rc = curl_easy_perform(c);
	long status = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
	lastStatus_ = status;
	curl_slist_free_all(headers);
	curl_easy_cleanup(c);

	if (sink.overflow) { out.clear(); err = "Download is larger than expected"; return false; }
	if (rc != CURLE_OK) {
		lastCurlCode = (int)rc;
		out.clear();
		err = std::string(curl_easy_strerror(rc)) + " (" + num(rc) + ")";
		return false;
	}
	if (status != 200) {
		out.clear();
		err = "Server answered HTTP " + num(status);
		return false;
	}
	return true;
}

} // namespace Net
