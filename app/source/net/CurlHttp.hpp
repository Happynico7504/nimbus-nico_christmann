#pragma once

// HTTPS client built on libcurl + mbedtls, used for every download the app makes.
//
// The 3DS system HTTP service only speaks TLS 1.0, which GitHub (TLS 1.2+) rejects with
// 0xD8A0A03C, so the app carries its own TLS stack. The same code builds on the host for tests.

#include <string>
#include <vector>

#include "Manager.hpp"

namespace Net {

class CurlHttp : public Manager::HttpClient {
public:
	CurlHttp();
	~CurlHttp() override;

	bool ok() const { return ok_; }

	// false = do not verify the server certificate. Only ever set after the user agreed
	// (for example when the console clock is wrong and the certificate looks "not yet valid").
	bool verifyTls = true;

	// libcurl error of the last failed request (0 if it was an HTTP status or another problem).
	int lastCurlCode = 0;
	// true when the last failure was about the server certificate, so retrying without verification could help
	bool certificateProblem() const;

	bool get(const std::string& url, std::vector<uint8_t>& out, std::string& err, size_t maxBytes) override;

private:
	bool ok_ = false;
	long lastStatus_ = 0; // HTTP status of the last attempt (0 if it never got one)

	// One HTTP attempt, no retry - the previous body of get().
	bool getOnce(const std::string& url, std::vector<uint8_t>& out, std::string& err, size_t maxBytes);
};

} // namespace Net
