#include "Services.hpp"

#include <algorithm>
#include <cstdio>

#include "Fs.hpp"
#include "Sources.hpp"

namespace Services {

const std::vector<Service>& all() {
	static const std::vector<Service> v = {
		{"account", "Account (act)", {"0004013000003802.ips"}},
		{"friends", "Friends", {"0004013000003202.ips"}},
		{"http", "HTTP redirects", {"0004013000002902.ips"}},
		{"socket", "Socket", {"0004013000002E02.ips"}},
		{"ssl", "SSL", {"0004013000002F02.ips"}},
		{"miiverse", "Miiverse (applets + cert)", {"000400300000BC02.ips", "000400300000BD02.ips", "000400300000BE02.ips", "juxt-prod.pem"}},
		{"eshop", "eShop / Badge Arcade", {"0004013000002C02.ips", "000400300000CE02.ips", "000400300000D602.ips", "000400300000C602.ips"}},
		{"plugin", "Plugin", {"nimbus.3gx"}},
	};
	return v;
}

const Service* findById(const std::string& id) {
	for (const auto& s : all())
		if (s.id == id) return &s;
	return nullptr;
}

static bool ships(const Sources::Network& n, const std::string& svc) {
	return std::find(n.services.begin(), n.services.end(), svc) != n.services.end();
}

std::vector<std::string> networks() {
	std::vector<std::string> out = {kBase};
	for (const auto& n : Sources::all())
		if (n.fullNetwork) out.push_back(n.id);
	return out;
}

std::string networkOf(const Selection& sel) {
	for (const auto& kv : sel) {
		const Sources::Network* n = Sources::findById(kv.second);
		if (n && n->fullNetwork) return n->id;
	}
	return kBase;
}

Selection selectionFor(const std::string& networkId) {
	Selection sel;
	const Sources::Network* net = Sources::findById(networkId);
	if (net && net->fullNetwork) {
		for (const auto& s : all()) sel[s.id] = ships(*net, s.id) ? net->id : std::string(kOff);
		return sel;
	}
	const Sources::Network* base = Sources::findById(kBase);
	for (const auto& s : all()) sel[s.id] = (base && ships(*base, s.id)) ? std::string(kBase) : std::string(kOff);
	return sel;
}

std::vector<std::string> providersFor(const std::string& serviceId, const std::string& networkId) {
	std::vector<std::string> out;
	const Sources::Network* net = Sources::findById(networkId);
	if (net && net->fullNetwork) {
		out.push_back(ships(*net, serviceId) ? net->id : std::string(kOff));
		return out;
	}
	const Sources::Network* base = Sources::findById(kBase);
	if (base && ships(*base, serviceId)) out.push_back(kBase);
	else out.push_back(kOff); // only a full network ships this service (eShop)
	for (const auto& n : Sources::all())
		if (n.overlay && ships(n, serviceId)) out.push_back(n.id);
	return out;
}

void setProvider(Selection& sel, const std::string& serviceId, const std::string& providerId) {
	if (networkOf(sel) != kBase) return; // full networks cannot be mixed
	const auto opts = providersFor(serviceId, kBase);
	if (std::find(opts.begin(), opts.end(), providerId) == opts.end()) return;

	std::string old = sel.count(serviceId) ? sel[serviceId] : std::string(kBase);
	const Sources::Network* oldNet = Sources::findById(old);
	const Sources::Network* newNet = Sources::findById(providerId);

	// leaving a bundle: everything that was taken from it moves back to the base
	if (oldNet && oldNet->bundled && providerId != old) {
		for (const auto& sid : oldNet->services)
			if (sel[sid] == old) sel[sid] = kBase;
	}
	sel[serviceId] = providerId;
	// entering a bundle: take the whole set
	if (newNet && newNet->bundled) {
		for (const auto& sid : newNet->services) sel[sid] = providerId;
	}
}

Selection normalized(const Selection& in) {
	const std::string net = networkOf(in);
	Selection out = selectionFor(net);
	for (const auto& s : all()) {
		auto it = in.find(s.id);
		if (it == in.end()) continue;
		const auto opts = providersFor(s.id, net);
		if (std::find(opts.begin(), opts.end(), it->second) != opts.end()) out[s.id] = it->second;
	}
	return out;
}

std::vector<std::string> providersUsed(const Selection& sel) {
	std::vector<std::string> out;
	for (const auto& s : all()) {
		auto it = sel.find(s.id);
		if (it == sel.end() || it->second == kOff) continue;
		if (std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
	}
	return out;
}

bool buildStage(const Selection& sel, const std::string& cacheRoot, const std::string& stageDir, std::string& err) {
	Fs::removeTree(stageDir);
	if (!Fs::mkdirs(stageDir)) { err = "could not create the staging folder"; return false; }

	for (const auto& s : all()) {
		auto it = sel.find(s.id);
		if (it == sel.end() || it->second == kOff) continue;
		const std::string cache = cacheRoot + "/" + it->second;
		for (const auto& f : s.files) {
			std::string data;
			if (!Fs::readFile(cache + "/" + f, data)) {
				Fs::removeTree(stageDir);
				err = "the " + it->second + " download has no " + f + " (needed for " + s.name + ")";
				return false;
			}
			if (!Fs::writeFile(stageDir + "/" + f, (const uint8_t*)data.data(), data.size())) {
				Fs::removeTree(stageDir);
				err = "could not stage " + f;
				return false;
			}
		}
	}
	return true;
}

} // namespace Services
