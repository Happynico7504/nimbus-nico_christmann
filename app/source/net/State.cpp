#include "State.hpp"

State State::parse(const std::string& text) {
	State s;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		size_t eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string k = line.substr(0, eq), v = line.substr(eq + 1);
		if (k.rfind("select.", 0) == 0 && k.size() > 7 && !v.empty()) s.selection[k.substr(7)] = v;
		else if (k.rfind("version.", 0) == 0 && k.size() > 8 && !v.empty()) s.versions[k.substr(8)] = v;
		else if (k == "trusted" && !v.empty()) s.trusted.insert(v);
	}
	return s;
}

std::string State::serialize() const {
	std::string o;
	for (const auto& kv : selection) o += "select." + kv.first + "=" + kv.second + "\n";
	for (const auto& kv : versions) o += "version." + kv.first + "=" + kv.second + "\n";
	for (const auto& t : trusted) o += "trusted=" + t + "\n";
	return o;
}
