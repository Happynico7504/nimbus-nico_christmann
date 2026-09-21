#include "NetworkSwitch.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "MainUI.hpp"
#include "../net/AppUpdate.hpp"
#include "../net/CiaInstall.hpp"
#include "../net/CtrHttp.hpp"
#include "../net/Fs.hpp"
#include "../net/Installer.hpp"
#include "../net/Manager.hpp"
#include "../net/Services.hpp"
#include "../net/Sources.hpp"
#include "../net/State.hpp"

namespace {

constexpr const char* kStatePath = "/3ds/nimbus/state.txt";
constexpr const char* kCacheRoot = "/3ds/nimbus/cache";
constexpr const char* kStageDir = "/3ds/nimbus/stage";

enum class Stage { Closed, Page, Trust, Working, Message };
enum class Job { None, Apply, SelfUpdate };

Stage stage = Stage::Closed;
Job job = Job::None;
Job retryJob = Job::None;          // what to run again if the user allows an unverified retry
int selected = 0;
int workFrames = 0;
bool allowUnverifiedRetry = false; // offered after a network-level failure
bool unverified = false;           // the user agreed to skip certificate verification for this run
std::string message;
std::string trustPending;          // provider id waiting for a trust decision

State state;                       // what is installed / remembered
Services::Selection sel;           // what the page currently shows (may differ from what is installed)

int serviceCount() { return (int)Services::all().size(); }
int rowCount() { return serviceCount() + 2; } // network + services + "update the app"
bool isNetworkRow(int i) { return i == 0; }
bool isAppRow(int i) { return i == serviceCount() + 1; }
int serviceIndex(int row) { return row - 1; }

void loadState() {
	std::string text;
	state = Fs::readFile(kStatePath, text) ? State::parse(text) : State();
}

void saveState() {
	Fs::mkdirs("/3ds/nimbus");
	Fs::writeText(kStatePath, state.serialize());
}

std::string label(const std::string& providerId) {
	if (providerId == Services::kOff) return "Off";
	const Sources::Network* n = Sources::findById(providerId);
	return n ? n->label : providerId;
}

bool differsFromInstalled(const std::string& serviceId) {
	if (!state.installed()) return true;
	auto it = state.selection.find(serviceId);
	std::string installed = it == state.selection.end() ? std::string(Services::kOff) : it->second;
	return sel[serviceId] != installed;
}

bool anyPending() {
	for (const auto& s : Services::all())
		if (differsFromInstalled(s.id)) return true;
	return false;
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

// Switching network resets the page to that network's defaults (a full network cannot be mixed).
void cycleNetwork(int dir) {
	const auto nets = Services::networks();
	auto it = std::find(nets.begin(), nets.end(), Services::networkOf(sel));
	int idx = it == nets.end() ? 0 : (int)(it - nets.begin());
	idx = (idx + dir + (int)nets.size()) % (int)nets.size();
	sel = Services::selectionFor(nets[idx]);
}

void cycleProvider(int serviceIdx, int dir) {
	const Services::Service& s = Services::all()[serviceIdx];
	const auto opts = Services::providersFor(s.id, Services::networkOf(sel));
	if (opts.size() < 2) return;
	auto it = std::find(opts.begin(), opts.end(), sel[s.id]);
	int idx = it == opts.end() ? 0 : (int)(it - opts.begin());
	idx = (idx + dir + (int)opts.size()) % (int)opts.size();
	Services::setProvider(sel, s.id, opts[idx]);
}

// ---------------------------------------------------------------- the actual work (blocking)

void runApply(MainStruct* ms) {
	Ctr::Http http;
	http.verifyTls = !unverified;
	if (!http.ok()) { showMessage("Cannot use the network service. Is Wi-Fi connected?"); return; }

	// 1) get every provider that is actually used (checks for a newer tag, reuses the saved copy if current)
	std::map<std::string, Manager::Prepared> prepared;
	for (const auto& p : Services::providersUsed(sel)) {
		const Sources::Network* n = Sources::findById(p);
		if (!n) continue;
		Manager::Prepared pr;
		std::string err;
		if (!Manager::prepare(*n, http, kCacheRoot, pr, err)) {
			showMessage(std::format("Could not get the {} patches:\n{}", n->label, err), http.lastResult != 0 && !unverified);
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

	ms->errorString[0] = 0;
	std::string done = std::format("Patches applied.{}\n\n{} installed, {} removed.", versions, r.installed, r.removed);
	LOGF_NIMBUS_ERROR(ms, "%s", done.c_str());
	aptSetHomeAllowed(false);
	ms->needsReboot = true;
	stage = Stage::Closed; // the top screen now shows the result and "Press START to reboot"
}

void runSelfUpdate(MainStruct* ms) {
	Ctr::Http http;
	http.verifyTls = !unverified;
	if (!http.ok()) { showMessage("Cannot use the network service. Is Wi-Fi connected?"); return; }

	AppUpdate::Info info;
	std::string err;
	if (!AppUpdate::check(http, VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO, info, err)) {
		showMessage(std::format("Could not check for app updates:\n{}", err), http.lastResult != 0 && !unverified);
		return;
	}
	if (!info.newer) {
		showMessage(std::format("Nimbus is up to date.\nInstalled: {}.{}.{}\nNewest: {}", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO, info.tag));
		return;
	}

	std::vector<uint8_t> cia;
	if (!AppUpdate::fetch(http, info, cia, err)) { showMessage(std::format("Could not download the update:\n{}", err)); return; }
	if (!CiaInstall::install(cia, err)) { showMessage(std::format("Could not install the update:\n{}", err)); return; }

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
	text("Patch services", 8, 4, 0.55f, kWhite);

	const std::string net = Services::networkOf(sel);
	float y = 26;
	const float rowH = 19;
	for (int i = 0; i < rowCount(); i++) {
		if (i == selected) C2D_DrawRectSolid(4, y - 2, 0.3f, 312, rowH, C2D_Color32(50, 60, 90, 255));
		if (isNetworkRow(i)) {
			text("Network", 10, y, 0.5f, kWhite);
			text(label(net), 312, y, 0.5f, kAccent, 0.0f, C2D_AlignRight);
		} else if (isAppRow(i)) {
			text("Update the Nimbus app", 10, y, 0.48f, kWhite);
			text(std::format("{}.{}.{}", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO), 312, y, 0.45f, kGrey, 0.0f, C2D_AlignRight);
		} else {
			const Services::Service& s = Services::all()[serviceIndex(i)];
			const std::string& p = sel[s.id];
			const bool locked = Services::providersFor(s.id, net).size() < 2;
			bool pending = differsFromInstalled(s.id);
			text(std::format("{}{}", pending ? "* " : "", s.name), 10, y, 0.46f, pending ? kWarn : kWhite);
			std::string shown = p == Services::kOff ? (net == Services::kBase ? "Ours only" : "Off") : label(p);
			u32 color = locked ? kGrey : (p == Services::kBase ? kWhite : kAccent);
			text(shown, 312, y, 0.46f, color, 0.0f, C2D_AlignRight);
		}
		y += rowH;
	}

	text(anyPending() ? "* = change not applied yet" : "Everything shown is installed. X re-checks for newer versions.", 8, 204, 0.4f, anyPending() ? kWarn : kGrey);
	text("A/LEFT/RIGHT: change   X: apply   B: back", 8, 220, 0.42f, kWhite);
}

void drawTrust() {
	const Sources::Network* n = Sources::findById(trustPending);
	if (!n) return;
	std::string services;
	for (const auto& sid : n->services) {
		const Services::Service* s = Services::findById(sid);
		if (!s) continue;
		bool used = sel[sid] == n->id;
		if (used) services += (services.empty() ? "" : ", ") + s->name;
	}
	text(std::format("Trust \"{}\"?", n->name), 8, 6, 0.6f, kWarn);
	text(std::format("Published by:\n{}\n\nFor: {}\n\nNimbus cannot verify this source. Its files are patches that Luma applies to system modules at boot. Only continue if you trust the publisher.\n\nYour choice is remembered for this source.", n->publisher, services.empty() ? "-" : services),
	     8, 32, 0.44f, kWhite, 304.0f);
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

} // namespace

namespace NetworkSwitch {

bool isActive() { return stage != Stage::Closed; }

void open(MainStruct* ms) {
	(void)ms;
	loadState();
	unverified = false;
	selected = 0;
	sel = Services::normalized(state.installed() ? state.selection : Services::selectionFor(Services::kBase));
	stage = Stage::Page;
}

void onStartup(MainStruct* ms) {
	loadState();
	if (!state.installed()) {
		LOG_NIMBUS_ERROR(ms, "No patches are installed yet.\nPress SELECT to set them up.");
	}
}

void update(MainStruct* ms, C3D_RenderTarget* top, C3D_RenderTarget* bottom, u32 kDown, touchPosition touch) {
	// ---- input
	switch (stage) {
		case Stage::Page: {
			if (kDown & KEY_DOWN) selected = (selected + 1) % rowCount();
			if (kDown & KEY_UP) selected = (selected + rowCount() - 1) % rowCount();
			if (kDown & KEY_TOUCH) {
				int row = ((int)touch.py - 24) / 19;
				if (touch.py >= 24 && row >= 0 && row < rowCount()) selected = row;
			}
			if (kDown & KEY_B) { stage = Stage::Closed; return; }
			if (isAppRow(selected)) {
				if (kDown & KEY_A) startWork(Job::SelfUpdate);
			} else if (isNetworkRow(selected)) {
				if (kDown & (KEY_A | KEY_RIGHT)) cycleNetwork(+1);
				if (kDown & KEY_LEFT) cycleNetwork(-1);
			} else {
				if (kDown & (KEY_A | KEY_RIGHT)) cycleProvider(serviceIndex(selected), +1);
				if (kDown & KEY_LEFT) cycleProvider(serviceIndex(selected), -1);
			}
			if (kDown & KEY_X) {
				if (!askTrustIfNeeded()) startWork(Job::Apply);
			}
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
		default: break;
	}

	// ---- the blocking work runs only after the "Working" screen has been shown for a few frames
	if (stage == Stage::Working && ++workFrames >= 3) runJob(ms);
}

} // namespace NetworkSwitch
