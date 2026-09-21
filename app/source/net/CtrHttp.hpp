#pragma once

// libctru httpc implementation of Manager::HttpClient. 3DS only.

#include <3ds.h>

#include "Manager.hpp"

namespace Ctr {

class Http : public Manager::HttpClient {
public:
	Http();
	~Http() override;

	bool ok() const { return R_SUCCEEDED(initResult_); }

	// false = verify the server certificate (default). Only ever set to false after the user agreed.
	bool verifyTls = true;

	// Result of the last failed operation (0 if it was not a libctru error, e.g. an HTTP status).
	Result lastResult = 0;

	bool get(const std::string& url, std::vector<uint8_t>& out, std::string& err, size_t maxBytes) override;

private:
	Result initResult_;
};

} // namespace Ctr
