#include "Sources.hpp"

namespace Sources {

static std::vector<Network> build() {
	std::vector<Network> v;

	{
		Network n;
		n.id = "pretendo";
		n.name = "Pretendo (stock)";
		n.publisher = "github.com/PretendoNetwork/nimbus";
		n.owned = false;
		n.kind = Kind::GithubReleaseZip;
		n.releasesUrl = "https://api.github.com/repos/PretendoNetwork/nimbus/releases?per_page=10";
		n.tagPrefix = "v";
		n.assetPrefix = "3dsx.";
		n.assetSuffix = ".zip";
		v.push_back(n);
	}
	{
		Network n;
		n.id = "revivetendo";
		n.name = "Revivetendo (our network)";
		n.publisher = "github.com/Happynico7504/nimbus-nico_christmann";
		n.owned = true;
		n.kind = Kind::GithubReleaseZip;
		// Patches live in their own releases, tagged "patches-v*", so the app can have its own "app-v*" tags.
		n.releasesUrl = "https://api.github.com/repos/Happynico7504/nimbus-nico_christmann/releases?per_page=30";
		n.tagPrefix = "patches-v";
		n.assetPrefix = "nimbus-patches";
		n.assetSuffix = ".zip";
		v.push_back(n);
	}

	// Roseverse publishes no releases, only files in the repository. Its regional Miiverse patches are
	// named america/europe/japan; they map onto the USA/EUR/JPN Miiverse applet ids the installer uses.
	const std::vector<RawFile> roseFiles = {
		{"0004013000002902.ips", "0004013000002902.ips"},
		{"0004013000003802.ips", "0004013000003802.ips"},
		{"america.ips", "000400300000BD02.ips"},
		{"europe.ips", "000400300000BE02.ips"},
		{"japan.ips", "000400300000BC02.ips"},
		{"juxt-prod.pem", "juxt-prod.pem"},
	};
	{
		Network n;
		n.id = "roseverse";
		n.name = "Roseverse";
		n.publisher = "github.com/VirtuallyExisting/Roseverse-Patches";
		n.owned = false;
		n.kind = Kind::RawFiles;
		n.rawBase = "https://raw.githubusercontent.com/VirtuallyExisting/Roseverse-Patches/main/";
		n.versionUrl = "https://raw.githubusercontent.com/VirtuallyExisting/Roseverse-Patches/main/latest_version.txt";
		n.rawFiles = roseFiles;
		v.push_back(n);
	}
	{
		Network n;
		n.id = "roseverse-juxt";
		n.name = "Roseverse (Juxt)";
		n.publisher = "github.com/VirtuallyExisting/Roseverse-Patches (juxt/)";
		n.owned = false;
		n.kind = Kind::RawFiles;
		n.rawBase = "https://raw.githubusercontent.com/VirtuallyExisting/Roseverse-Patches/main/juxt/";
		n.versionUrl = "https://raw.githubusercontent.com/VirtuallyExisting/Roseverse-Patches/main/latest_version.txt";
		n.rawFiles = roseFiles;
		v.push_back(n);
	}
	return v;
}

const std::vector<Network>& all() {
	static const std::vector<Network> v = build();
	return v;
}

const Network* findById(const std::string& id) {
	for (const auto& n : all())
		if (n.id == id) return &n;
	return nullptr;
}

} // namespace Sources
