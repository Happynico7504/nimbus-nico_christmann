#include "CtrHttp.hpp"

#include <cstdio>
#include <cstring>

namespace Ctr {

Http::Http() {
	initResult_ = httpcInit(0x100000);
}

Http::~Http() {
	if (R_SUCCEEDED(initResult_)) httpcExit();
}

static std::string hex(Result rc) {
	char b[32];
	std::snprintf(b, sizeof b, "0x%08lX", (unsigned long)rc);
	return b;
}

// "https://host/path" -> "https://host"
static std::string originOf(const std::string& url) {
	size_t s = url.find("://");
	if (s == std::string::npos) return "";
	size_t e = url.find('/', s + 3);
	return e == std::string::npos ? url : url.substr(0, e);
}

bool Http::get(const std::string& url0, std::vector<uint8_t>& out, std::string& err, size_t maxBytes) {
	lastResult = 0;
	out.clear();
	if (!ok()) { lastResult = initResult_; err = "Network service unavailable " + hex(initResult_); return false; }

	std::string url = url0;
	for (int hop = 0; hop < 6; hop++) {
		httpcContext ctx;
		Result rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url.c_str(), 1);
		if (R_FAILED(rc)) { lastResult = rc; err = "Could not open connection " + hex(rc); return false; }

		httpcSetSSLOpt(&ctx, verifyTls ? 0 : SSLCOPT_DisableVerify);
		httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
		httpcAddRequestHeaderField(&ctx, "User-Agent", "Nimbus-Updater");
		httpcAddRequestHeaderField(&ctx, "Accept", "application/vnd.github+json, */*");

		rc = httpcBeginRequest(&ctx);
		if (R_FAILED(rc)) { httpcCloseContext(&ctx); lastResult = rc; err = "Request failed " + hex(rc); return false; }

		u32 status = 0;
		rc = httpcGetResponseStatusCode(&ctx, &status);
		if (R_FAILED(rc)) { httpcCloseContext(&ctx); lastResult = rc; err = "No response " + hex(rc); return false; }

		if (status >= 301 && status <= 308 && status != 304) {
			char loc[1024] = {0};
			rc = httpcGetResponseHeader(&ctx, "Location", loc, sizeof loc);
			httpcCloseContext(&ctx);
			if (R_FAILED(rc) || !loc[0]) { lastResult = rc; err = "Redirect without a location"; return false; }
			url = loc[0] == '/' ? originOf(url) + loc : std::string(loc);
			continue;
		}
		if (status != 200) {
			httpcCloseContext(&ctx);
			err = "Server answered HTTP " + std::to_string(status);
			return false;
		}

		size_t used = 0;
		out.resize(0x8000);
		do {
			u32 got = 0;
			rc = httpcDownloadData(&ctx, out.data() + used, (u32)(out.size() - used), &got);
			used += got;
			if (used > maxBytes) { httpcCloseContext(&ctx); out.clear(); err = "Download is larger than expected"; return false; }
			if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING && used == out.size()) out.resize(out.size() * 2);
		} while (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
		httpcCloseContext(&ctx);

		if (R_FAILED(rc)) { out.clear(); lastResult = rc; err = "Download failed " + hex(rc); return false; }
		out.resize(used);
		return true;
	}
	err = "Too many redirects";
	return false;
}

} // namespace Ctr
