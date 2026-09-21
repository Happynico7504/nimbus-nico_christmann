#include "NetworkSwitch.hpp"

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
bool allowUnverifiedRetry = false; // offered only after a certificate problem
bool unverified = false;           // the user agreed to skip certificate verification for this run
std::string message;
std::string trustPending;          // provider id waiting for a trust decision

State state;                       // what is installed / remembered
Services::Selection sel;           // what the chosen preset will install

int presetCount() { return (int)Services::presets().size(); }
int rowCount() { return presetCount() + 1; } // presets + "update the app"
bool isAppRow(int i) { return i == presetCount(); }

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

	// 1) get every provider that is actually used (checks for a newer tag, reuses the saved copy if current)
	std::map<std::string, Manager::Prepared> prepared;
	for (const auto& p : Services::providersUsed(sel)) {
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
	text("Choose your network", 8, 6, 0.6f, kWhite);

	float y = 36;
	const float rowH = 40;
	for (int i = 0; i < rowCount(); i++) {
		if (i == selected) C2D_DrawRectSolid(4, y - 3, 0.3f, 312, rowH - 4, C2D_Color32(50, 60, 90, 255));
		if (isAppRow(i)) {
			text("Update the Nimbus app", 12, y, 0.55f, kWhite);
			text(std::format("Running {}.{}.{}", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO), 12, y + 18, 0.4f, kGrey);
		} else {
			const Services::Preset& p = Services::presets()[i];
			std::string inst = installedLine(p);
			text(p.label, 12, y, 0.55f, inst.empty() ? kWhite : kAccent);
			text(inst.empty() ? p.blurb : inst, 12, y + 18, 0.4f, inst.empty() ? kGrey : kAccent);
		}
		y += rowH;
	}
	text("UP/DOWN: choose    A: install    B: back", 8, 220, 0.42f, kWhite);
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
	if (!state.installed()) {
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
				int row = ((int)touch.py - 33) / 40;
				if (touch.py >= 33 && row >= 0 && row < rowCount()) selected = row;
			}
			if (kDown & KEY_B) { stage = Stage::Closed; return; }
			if (kDown & KEY_A) {
				if (isAppRow(selected)) {
					startWork(Job::SelfUpdate);
				} else {
					sel = Services::selectionForPreset(Services::presets()[selected].id);
					if (!askTrustIfNeeded()) startWork(Job::Apply);
				}
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
