#pragma once

// The eight "services" shown on the patch page. Each service is a group of patch files.
//
// Two networks can be installed: Pretendo (the base) or our own full network. On Pretendo, overlays such as
// Roseverse may replace individual services; our network always provides every service and cannot be mixed.

#include <map>
#include <string>
#include <vector>

namespace Services {

struct Service {
	std::string id;
	std::string name;                // shown on the page
	std::vector<std::string> files;  // file names in a provider's cache (see Installer::knownFiles)
};

const std::vector<Service>& all();
const Service* findById(const std::string& id);

constexpr const char* kBase = "pretendo"; // the base network
constexpr const char* kOff = "off";       // "not installed" (services the base does not ship)

// service id -> provider id (or kOff)
using Selection = std::map<std::string, std::string>;

// The networks that can be chosen: the base first, then full networks.
std::vector<std::string> networks();

// Which network a selection belongs to (a full network as soon as any service uses it).
std::string networkOf(const Selection& sel);

// Everything from the given network: Pretendo -> Pretendo for what it ships (Off for the rest); a full network -> all ours.
Selection selectionFor(const std::string& networkId);

// Providers offered for a service under a network, in the order the page cycles through them.
std::vector<std::string> providersFor(const std::string& serviceId, const std::string& networkId);

// Choose an overlay (or the base again) for one service. Only meaningful on the base network; overlays that are
// bundled (Roseverse) move together, and moving one member away moves the whole bundle back to the base.
void setProvider(Selection& sel, const std::string& serviceId, const std::string& providerId);

// Drop invalid entries (unknown service/provider, provider not allowed for the network) and fill in defaults.
Selection normalized(const Selection& sel);

// Providers actually needed by a selection (no duplicates, no kOff).
std::vector<std::string> providersUsed(const Selection& sel);

// Assemble the chosen files, taken from `<cacheRoot>/<provider>/`, into a fresh `stageDir`.
bool buildStage(const Selection& sel, const std::string& cacheRoot, const std::string& stageDir, std::string& err);

} // namespace Services
