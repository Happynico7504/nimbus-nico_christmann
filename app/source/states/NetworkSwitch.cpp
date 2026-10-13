#include "NetworkSwitch.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "MainUI.hpp"
#include "../net/AppUpdate.hpp"
#include "../net/CiaInstall.hpp"
#include "../net/CurlHttp.hpp"
#include "../net/Fs.hpp"
#include "../net/Installer.hpp"
#include "../net/Manager.hpp"
#include "../net/RvtaHook.hpp"
#include "../net/Services.hpp"
#include "../net/Sources.hpp"
#include "../net/State.hpp"

namespace {

constexpr const char* kStatePath = "/3ds/nimbus/state.txt";
constexpr const char* kCacheRoot = "/3ds/nimbus/cache";
constexpr const char* kStageDir = "/3ds/nimbus/stage";

enum class Stage { Closed, Page, Trust, Working, Message, Consent };
enum class Job { None, Apply, SelfUpdate };

Stage stage = Stage::Closed;
Job job = Job::None;
Job retryJob = Job::None;          // what to run again if the user allows an unverified retry
int selected = 0;
int workFrames = 0;
bool allowUnverifiedRetry = false; // offered only after a certificate problem
bool unverified = false;           // the user agreed to skip certificate verification for this run
std::string message;
std::string trustPending;          // provider id waiting for a trust decision

State state;                       // what is installed / remembered
Services::Selection sel;           // what the chosen preset will install

int presetCount() { return (int)Services::presets().size(); }
int rowCount() { return presetCount() + 2; } // presets + analytics + "update the app"
bool isAnalyticsRow(int i) { return i == presetCount(); }
bool isAppRow(int i) { return i == presetCount() + 1; }
constexpr float kRowH = 36;

// RvtA (analytics) is opt-in and independent of the network: its file comes from our patches release.
constexpr const char* kRvtaFile = "0004013000F7A002.cxi";
constexpr const char* kRvtaProvider = "revivetendo";
// NS patch (lets installed update titles run for any unique id, so updates from the Revivetendo eShop work):
// harmless on every network, so it is added from our release whatever network is chosen, like RvtA but always.
constexpr const char* kNsPatchFile = "0004013000008002.ips";

bool analyticsOn() { return state.analytics == "on"; }

// Turning analytics off needs no download: remove RvtA and its boot hook (it stops at the next reboot).
void removeAnalytics() {
	std::remove(RvtaHook::kCxiPath);
	RvtaHook::sync();
}

void loadState() {
	std::string text;
	state = Fs::readFile(kStatePath, text) ? State::parse(text) : State();
}

void saveState() {
	Fs::mkdirs("/3ds/nimbus");
	Fs::writeText(kStatePath, state.serialize());
}

std::string label(const std::string& providerId) {
	const Sources::Network* n = Sources::findById(providerId);
	return n ? n->label : providerId;
}

void text(const std::string& s, float x, float y, float size, u32 color, float wrapWidth = 0.0f, int flags = 0) {
	C2D_Text t;
	C2D_TextBufClear(textBuf);
	C2D_TextFontParse(&t, font, textBuf, s.c_str());
	C2D_TextOptimize(&t);
	if (wrapWidth > 0.0f) C2D_DrawText(&t, C2D_WithColor | C2D_WordWrap | flags, x, y, 0.5f, size, size, color, wrapWidth);
	else C2D_DrawText(&t, C2D_WithColor | flags, x, y, 0.5f, size, size, color);
}

constexpr u32 kWhite = C2D_Color32(255, 255, 255, 255);
constexpr u32 kGrey = C2D_Color32(170, 170, 180, 255);
constexpr u32 kAccent = C2D_Color32(120, 190, 255, 255);
constexpr u32 kWarn = C2D_Color32(255, 200, 90, 255);

void showMessage(const std::string& m, bool offerUnverifiedRetry = false) {
	message = m;
	allowUnverifiedRetry = offerUnverifiedRetry;
	stage = Stage::Message;
}

void startWork(Job j) {
	job = j;
	workFrames = 0;
	stage = Stage::Working;
}

// Third-party providers must be trusted by the user once before anything is downloaded from them.
// Returns true when a trust screen was opened.
bool askTrustIfNeeded() {
	for (const auto& p : Services::providersUsed(sel)) {
		const Sources::Network* n = Sources::findById(p);
		if (n && !n->owned && !state.trusted.count(p)) {
			trustPending = p;
			stage = Stage::Trust;
			return true;
		}
	}
	return false;
}

std::string installedLine(const Services::Preset& p) {
	if (!state.installed() || Services::presetOf(state.selection) != p.id) return "";
	std::string v;
	for (const auto& id : Services::providersUsed(state.selection)) {
		auto it = state.versions.find(id);
		if (it != state.versions.end()) v += (v.empty() ? "" : ", ") + label(id) + " " + it->second;
	}
	return v.empty() ? "Installed" : "Installed: " + v;
}

// ---------------------------------------------------------------- the actual work (blocking)

void runApply(MainStruct* ms) {
	Net::CurlHttp http;
	http.verifyTls = !unverified;
	if (!http.ok()) { showMessage("Could not start the network. Is Wi-Fi connected?"); return; }

	// 1) get every provider that is actually used (checks for a newer tag, reuses the saved copy if current);
	//    with analytics on, our release is needed for RvtA whatever the network is
	std::map<std::string, Manager::Prepared> prepared;
	std::vector<std::string> providers = Services::providersUsed(sel);
	//    our release is always needed too: it carries the NS patch (and RvtA when analytics is on)
	if (std::find(providers.begin(), providers.end(), kRvtaProvider) == providers.end())
		providers.push_back(kRvtaProvider);
	for (const auto& p : providers) {
		const Sources::Network* n = Sources::findById(p);
		if (!n) continue;
		Manager::Prepared pr;
		std::string err;
		if (!Manager::prepare(*n, http, kCacheRoot, pr, err)) {
			showMessage(std::format("Could not get the {} patches:\n{}", n->label, err), http.certificateProblem() && !unverified);
			return;
		}
		prepared[p] = pr;
	}

	// 2) same safety as before: do not touch the patches if the account migration failed
	ms->errorString[0] = 0;
	MainUI::migrateAccount(ms);
	if (ms->errorString[0] != 0) {
		std::string e = ms->errorString;
		showMessage(std::format("Not installed: account migration failed.\n{}", e));
		return;
	}

	// 3) assemble the chosen files and install them; anything not selected is removed
	std::string err;
	if (!Services::buildStage(sel, kCacheRoot, kStageDir, err)) { showMessage(std::format("Could not assemble the patches:\n{}", err)); return; }
	if (analyticsOn()) {
		std::string rvta;
		if (!Fs::readFile(std::format("{}/{}/{}", kCacheRoot, kRvtaProvider, kRvtaFile), rvta) ||
		    !Fs::writeFile(std::format("{}/{}", kStageDir, kRvtaFile), (const uint8_t*)rvta.data(), rvta.size())) {
			Fs::removeTree(kStageDir);
			showMessage("Could not add analytics (RvtA) to the patches.");
			return;
		}
	}
	{
		// NS patch from our release, on every network; an older release without it is fine (nothing to add)
		std::string ns;
		if (Fs::readFile(std::format("{}/{}/{}", kCacheRoot, kRvtaProvider, kNsPatchFile), ns) &&
		    !Fs::writeFile(std::format("{}/{}", kStageDir, kNsPatchFile), (const uint8_t*)ns.data(), ns.size())) {
			Fs::removeTree(kStageDir);
			showMessage("Could not add the NS patch to the patches.");
			return;
		}
	}
	Installer::Result r = Installer::install(kStageDir);
	Fs::removeTree(kStageDir);
	if (!r.ok) { showMessage(std::format("Install failed: {}", r.error)); return; }

	state.selection = sel;
	state.versions.clear();
	std::string versions;
	for (const auto& kv : prepared) {
		state.versions[kv.first] = kv.second.version;
		versions += std::format("\n{}: {}{}", label(kv.first), kv.second.version, kv.second.usedCache ? " (saved copy)" : " (downloaded)");
	}
	saveState();

	// RvtA (analytics, if turned on): start it at boot, or undo that when it was removed.
	std::string analytics = RvtaHook::sync();
	// Emulators (Azahar = 2) cannot run RvtA anyway (no /luma/sysmodules loading, no pxi:pm): do not show the
	// expected "not started" line there. rvta-hook.txt still records why.
	s64 emulator = 0;
	svcGetSystemInfo(&emulator, 0x20000, 0);
	if (emulator != 0) analytics.clear();

	ms->errorString[0] = 0;
	std::string done = std::format("Patches applied.{}\n\n{} installed, {} removed.{}{}", versions, r.installed, r.removed,
	                               analytics.empty() ? "" : "\n", analytics);
	LOGF_NIMBUS_ERROR(ms, "%s", done.c_str());
	aptSetHomeAllowed(false);
	ms->needsReboot = true;
	stage = Stage::Closed; // the top screen now shows the result and "Press START to reboot"
}

void runSelfUpdate(MainStruct* ms) {
	Net::CurlHttp http;
	http.verifyTls = !unverified;
	if (!http.ok()) { showMessage("Could not start the network. Is Wi-Fi connected?"); return; }

	AppUpdate::Info info;
	std::string err;
	if (!AppUpdate::check(http, VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO, info, err)) {
		showMessage(std::format("Could not check for app updates:\n{}", err), http.certificateProblem() && !unverified);
		return;
	}
	if (!info.newer) {
		showMessage(std::format("Nimbus is up to date.\nInstalled: {}.{}.{}\nNewest: {}", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO, info.tag));
		return;
	}

	std::vector<uint8_t> cia;
	if (!AppUpdate::fetch(http, info, cia, err)) { showMessage(std::format("Could not download the update:\n{}", err)); return; }
	if (!CiaInstall::install(cia, err)) { showMessage(std::format("Could not install the update:\n{}\n\n(Saved to /3ds/nimbus/update-error.txt)", err)); return; }

	ms->errorString[0] = 0;
	LOGF_NIMBUS_ERROR(ms, "Nimbus updated to %s.\nPress START, then open Nimbus again.", info.tag.c_str());
	stage = Stage::Closed;
}

void runJob(MainStruct* ms) {
	retryJob = job;
	if (job == Job::Apply) runApply(ms);
	else if (job == Job::SelfUpdate) runSelfUpdate(ms);
	job = Job::None;
	if (stage == Stage::Working) stage = Stage::Page; // safety net
}

// ---------------------------------------------------------------- drawing

void drawPage() {
	text("Choose your network", 8, 6, 0.6f, kWhite);

	float y = 32;
	const float rowH = kRowH;
	for (int i = 0; i < rowCount(); i++) {
		if (i == selected) C2D_DrawRectSolid(4, y - 3, 0.3f, 312, rowH - 4, C2D_Color32(50, 60, 90, 255));
		if (isAnalyticsRow(i)) {
			text(std::format("Analytics (RvtA): {}", analyticsOn() ? "On" : "Off"), 12, y, 0.55f, analyticsOn() ? kAccent : kWhite);
			text(analyticsOn() ? "Sends battery, Wi-Fi and error data to Revivetendo" : "Helps us find the cause of crashes", 12, y + 17, 0.4f, kGrey);
		} else if (isAppRow(i)) {
			text("Update the Nimbus app", 12, y, 0.55f, kWhite);
			text(std::format("Running {}.{}.{}", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO), 12, y + 17, 0.4f, kGrey);
		} else {
			const Services::Preset& p = Services::presets()[i];
			std::string inst = installedLine(p);
			text(p.label, 12, y, 0.55f, inst.empty() ? kWhite : kAccent);
			text(inst.empty() ? p.blurb : inst, 12, y + 17, 0.4f, inst.empty() ? kGrey : kAccent);
		}
		y += rowH;
	}
	text("UP/DOWN: choose    A: select    B: back", 8, 222, 0.42f, kWhite);
}

void drawConsent() {
	text("Help us fix problems?", 8, 6, 0.6f, kWhite);
	text("With analytics (RvtA) on, your console records battery level and temperature, Wi-Fi state, the running game "
	     "or app and error codes about once a minute, together with your friend code, and uploads them to Revivetendo "
	     "every 15 minutes. The newest Luma crash dump is sent once; a memory dump only when you press "
	     "L + START + SELECT.\n\nDetails: revivetendo.nicochristmann.net/privacy.html\n"
	     "You can change this any time with SELECT > Analytics.",
	     8, 30, 0.42f, kGrey, 304.0f);
	text("A: allow    B: don't allow", 8, 222, 0.45f, kWhite);
}

void drawTrust() {
	const Sources::Network* n = Sources::findById(trustPending);
	if (!n) return;
	text(std::format("Trust \"{}\"?", n->name), 8, 6, 0.6f, kWarn);
	text(std::format("Published by:\n{}\n\nNimbus cannot verify this source. Its files are patches that Luma applies to system modules at boot. Only continue if you trust the publisher.\n\nYour choice is remembered for this source.", n->publisher),
	     8, 32, 0.45f, kWhite, 304.0f);
	text("A: trust and continue    B: cancel", 8, 222, 0.45f, kWhite);
}

void drawWorking() {
	std::string what = job == Job::SelfUpdate ? "Checking for a Nimbus update..." : "Getting and installing the patches...";
	text(what, 8, 90, 0.55f, kWhite, 304.0f);
	text("Please wait, this can take a few seconds.\nDo not close the lid.", 8, 130, 0.45f, kGrey, 304.0f);
}

void drawMessage() {
	text(message, 8, 8, 0.45f, kWhite, 304.0f);
	text(allowUnverifiedRetry ? "Y: retry without certificate check    B: back" : "B: back", 8, 222, 0.42f, allowUnverifiedRetry ? kWarn : kGrey);
}

// Saves the choice and applies it: on = reinstall the current network with RvtA (downloads it if needed),
// off = remove RvtA right away. Both take effect at the next reboot.
void setAnalytics(MainStruct* ms, bool on) {
	state.analytics = on ? "on" : "off";
	saveState();
	if (!on) {
		bool had = Fs::exists(RvtaHook::kCxiPath);
		removeAnalytics();
		if (had) {
			ms->needsReboot = true;
			showMessage("Analytics is off. RvtA was removed and stops after the next restart.");
		} else {
			stage = stage == Stage::Consent ? Stage::Closed : Stage::Page;
		}
		return;
	}
	if (state.installed()) {
		sel = Services::normalized(state.selection);
		startWork(Job::Apply);
	} else {
		showMessage("Analytics is on. RvtA is installed together with the patches (SELECT, then choose a network).");
	}
}

} // namespace

namespace NetworkSwitch {

bool isActive() { return stage != Stage::Closed; }

void open(MainStruct* ms) {
	(void)ms;
	loadState();
	unverified = false;
	selected = 0;
	const std::string current = state.installed() ? Services::presetOf(state.selection) : "";
	for (int i = 0; i < presetCount(); i++)
		if (Services::presets()[i].id == current) selected = i;
	stage = Stage::Page;
}

void onStartup(MainStruct* ms) {
	loadState();
	// Analytics is opt-in: never keep RvtA when the player said no (older versions installed it with our network).
	if (state.analytics == "off" && Fs::exists(RvtaHook::kCxiPath)) removeAnalytics();
	// Keep the RvtA boot hook in step with whether RvtA is installed (also covers files copied by hand).
	// It only takes effect at the next boot.
	RvtaHook::sync();
	if (state.analytics.empty()) stage = Stage::Consent; // ask once
	// Only complain when no patch is on the SD card at all: patches copied by hand (the usual way on
	// Azahar, which applies the same IPS files) have no state.txt but work fine.
	if (!state.installed() && !Installer::anyInstalled()) {
		LOG_NIMBUS_ERROR(ms, "No patches are installed yet.\nPress SELECT to choose a network.");
	}
}

void update(MainStruct* ms, C3D_RenderTarget* top, C3D_RenderTarget* bottom, u32 kDown, touchPosition touch) {
	// ---- input
	switch (stage) {
		case Stage::Page: {
			if (kDown & KEY_DOWN) selected = (selected + 1) % rowCount();
			if (kDown & KEY_UP) selected = (selected + rowCount() - 1) % rowCount();
			if (kDown & KEY_TOUCH) {
				int row = ((int)touch.py - 29) / (int)kRowH;
				if (touch.py >= 29 && row >= 0 && row < rowCount()) selected = row;
			}
			if (kDown & KEY_B) { stage = Stage::Closed; return; }
			if (kDown & KEY_A) {
				if (isAppRow(selected)) {
					startWork(Job::SelfUpdate);
				} else if (isAnalyticsRow(selected)) {
					setAnalytics(ms, !analyticsOn());
				} else {
					sel = Services::selectionForPreset(Services::presets()[selected].id);
					if (!askTrustIfNeeded()) startWork(Job::Apply);
				}
			}
			break;
		}
		case Stage::Consent: {
			if (kDown & KEY_A) setAnalytics(ms, true);
			else if (kDown & KEY_B) setAnalytics(ms, false);
			break;
		}
		case Stage::Trust: {
			if (kDown & KEY_B) stage = Stage::Page;
			if (kDown & KEY_A) {
				state.trusted.insert(trustPending);
				saveState();
				if (!askTrustIfNeeded()) startWork(Job::Apply);
			}
			break;
		}
		case Stage::Message: {
			if (kDown & KEY_B) stage = Stage::Page;
			if ((kDown & KEY_Y) && allowUnverifiedRetry) {
				unverified = true;
				startWork(retryJob);
			}
			break;
		}
		default: break;
	}

	// ---- draw
	C2D_SceneBegin(top);
	DrawScamWarning();
	DrawVersionString();
	C2D_DrawSprite(&ms->top);

	C2D_SceneBegin(bottom);
	switch (stage) {
		case Stage::Page: drawPage(); break;
		case Stage::Trust: drawTrust(); break;
		case Stage::Working: drawWorking(); break;
		case Stage::Message: drawMessage(); break;
		case Stage::Consent: drawConsent(); break;
		default: break;
	}

	// ---- the blocking work runs only after the "Working" screen has been shown for a few frames
	if (stage == Stage::Working && ++workFrames >= 3) runJob(ms);
}

} // namespace NetworkSwitch
