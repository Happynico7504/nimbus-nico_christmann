#include "NetworkSwitch.hpp"

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
#include "../net/Sources.hpp"
#include "../net/State.hpp"

namespace {

constexpr const char* kStatePath = "/3ds/nimbus/state.txt";
constexpr const char* kCacheRoot = "/3ds/nimbus/cache";

enum class Stage { Closed, List, Trust, Working, Message };
enum class Job { None, Install, SelfUpdate };

Stage stage = Stage::Closed;
Job job = Job::None;
Job retryJob = Job::None;                    // what to run again if the user allows an unverified retry
const Sources::Network* retryNet = nullptr;
int selected = 0;
int workFrames = 0;
bool allowUnverifiedRetry = false; // offered after a certificate failure
bool unverified = false;           // user agreed to skip certificate verification for this run
std::string message;
State state;
const Sources::Network* pending = nullptr;

int rowCount() { return (int)Sources::all().size() + 1; } // networks + "update Nimbus"
bool isAppRow(int i) { return i == (int)Sources::all().size(); }

void loadState() {
	std::string text;
	state = Fs::readFile(kStatePath, text) ? State::parse(text) : State();
}

void saveState() {
	Fs::mkdirs("/3ds/nimbus");
	Fs::writeText(kStatePath, state.serialize());
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

void startWork(Job j, const Sources::Network* net) {
	job = j;
	pending = net;
	workFrames = 0;
	stage = Stage::Working;
}

std::string describe(const Sources::Network& n) {
	if (state.network == n.id) return std::format("Installed: {}", state.installedVersion.empty() ? "unknown version" : state.installedVersion);
	if (n.owned) return "Our network";
	return state.trusted.count(n.id) ? "Third party (trusted)" : "Third party - asks first";
}

// ---------------------------------------------------------------- the actual work (blocking)

void runInstall(MainStruct* ms, const Sources::Network& net) {
	Ctr::Http http;
	http.verifyTls = !unverified;
	if (!http.ok()) { showMessage("Cannot use the network service. Is Wi-Fi connected?"); return; }

	Manager::Prepared prepared;
	std::string err;
	if (!Manager::prepare(net, http, kCacheRoot, prepared, err)) {
		// A failed TLS handshake is the one case where retrying without verification makes sense - but only if the user says so.
		bool tlsLike = http.lastResult != 0; // a libctru-level failure (e.g. certificate check); HTTP status errors are not offered a retry
		showMessage(std::format("Could not get the patches:\n{}", err), tlsLike && !unverified);
		return;
	}

	// Same safety as before: do not touch the patches if the account migration failed.
	ms->errorString[0] = 0;
	MainUI::migrateAccount(ms);
	if (ms->errorString[0] != 0) {
		std::string e = ms->errorString;
		showMessage(std::format("Not installed: account migration failed.\n{}", e));
		return;
	}

	Installer::Result r = Installer::install(prepared.cacheDir);
	if (!r.ok) { showMessage(std::format("Install failed: {}", r.error)); return; }

	state.network = net.id;
	state.installedVersion = prepared.version;
	saveState();

	std::string done = std::format("Installed {} ({}).\n{}\n\n{} patches installed, {} old patches removed.",
	                               net.name, prepared.version, prepared.usedCache ? "Already the newest version (used the saved copy)." : "Downloaded the newest version.",
	                               r.installed, r.removed);
	ms->errorString[0] = 0;
	LOGF_NIMBUS_ERROR(ms, "%s", done.c_str());
	aptSetHomeAllowed(false);
	ms->needsReboot = true;
	stage = Stage::Closed; // the top screen now shows the message and "Press START to reboot"
}

void runSelfUpdate(MainStruct* ms) {
	Ctr::Http http;
	http.verifyTls = !unverified;
	if (!http.ok()) { showMessage("Cannot use the network service. Is Wi-Fi connected?"); return; }

	AppUpdate::Info info;
	std::string err;
	if (!AppUpdate::check(http, VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO, info, err)) {
		bool tlsLike = http.lastResult != 0; // a libctru-level failure (e.g. certificate check); HTTP status errors are not offered a retry
		showMessage(std::format("Could not check for app updates:\n{}", err), tlsLike && !unverified);
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
	retryNet = pending;
	if (job == Job::Install && pending) runInstall(ms, *pending);
	else if (job == Job::SelfUpdate) runSelfUpdate(ms);
	job = Job::None;
	if (stage == Stage::Working) stage = Stage::List; // safety net
}

// ---------------------------------------------------------------- drawing

void drawList() {
	text("Patch networks", 8, 6, 0.6f, kWhite);
	const auto& nets = Sources::all();
	float y = 30;
	for (int i = 0; i < rowCount(); i++) {
		const float h = 34;
		if (i == selected) C2D_DrawRectSolid(4, y - 2, 0.3f, 312, h, C2D_Color32(50, 60, 90, 255));
		if (isAppRow(i)) {
			text("Update the Nimbus app", 10, y, 0.5f, kWhite);
			text(std::format("Running {}.{}.{} - checks for a newer app-v release", VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO), 10, y + 15, 0.4f, kGrey);
		} else {
			const auto& n = nets[i];
			text(n.name, 10, y, 0.5f, state.network == n.id ? kAccent : kWhite);
			text(describe(n), 10, y + 15, 0.4f, kGrey);
		}
		y += h + 2;
	}
	text("UP/DOWN: choose    A: install / update    B: back", 8, 222, 0.4f, kGrey);
}

void drawTrust() {
	if (!pending) return;
	text(std::format("Trust \"{}\"?", pending->name), 8, 6, 0.6f, kWarn);
	text(std::format("Published by:\n{}\n\nNimbus cannot verify this source. Its files are patches that Luma applies to system modules (nim, act, http, ...) at boot. Only continue if you trust the publisher.\n\nYour choice is remembered for this network.", pending->publisher),
	     8, 34, 0.45f, kWhite, 304.0f);
	text("A: trust and install    B: cancel", 8, 222, 0.45f, kWhite);
}

void drawWorking() {
	std::string what = job == Job::SelfUpdate ? "Checking for a Nimbus update..." : (pending ? std::format("Getting patches for {}...", pending->name) : "Working...");
	text(what, 8, 90, 0.55f, kWhite, 304.0f);
	text("Please wait, this can take a few seconds.\nDo not close the lid.", 8, 130, 0.45f, kGrey, 304.0f);
}

void drawMessage() {
	text(message, 8, 8, 0.45f, kWhite, 304.0f);
	std::string hint = allowUnverifiedRetry ? "Y: retry without certificate check    B: back" : "B: back";
	text(hint, 8, 222, 0.42f, allowUnverifiedRetry ? kWarn : kGrey);
}

} // namespace

namespace NetworkSwitch {

bool isActive() { return stage != Stage::Closed; }

void open(MainStruct* ms) {
	(void)ms;
	loadState();
	unverified = false;
	selected = 0;
	for (int i = 0; i < (int)Sources::all().size(); i++)
		if (Sources::all()[i].id == state.network) selected = i;
	stage = Stage::List;
}

void onStartup(MainStruct* ms) {
	loadState();
	if (state.network.empty()) {
		LOG_NIMBUS_ERROR(ms, "No patch network is installed yet.\nPress SELECT to choose one.");
	}
}

void update(MainStruct* ms, C3D_RenderTarget* top, C3D_RenderTarget* bottom, u32 kDown, touchPosition touch) {
	// ---- input
	switch (stage) {
		case Stage::List: {
			if (kDown & KEY_DOWN) selected = (selected + 1) % rowCount();
			if (kDown & KEY_UP) selected = (selected + rowCount() - 1) % rowCount();
			if (kDown & KEY_TOUCH) {
				int row = ((int)touch.py - 28) / 36;
				if (touch.py >= 28 && row >= 0 && row < rowCount()) selected = row;
			}
			if (kDown & KEY_B) { stage = Stage::Closed; return; }
			if (kDown & KEY_A) {
				if (isAppRow(selected)) {
					startWork(Job::SelfUpdate, nullptr);
				} else {
					const Sources::Network& n = Sources::all()[selected];
					pending = &n;
					if (!n.owned && !state.trusted.count(n.id)) stage = Stage::Trust;
					else startWork(Job::Install, &n);
				}
			}
			break;
		}
		case Stage::Trust: {
			if (kDown & KEY_B) stage = Stage::List;
			if ((kDown & KEY_A) && pending) {
				state.trusted.insert(pending->id);
				saveState();
				startWork(Job::Install, pending);
			}
			break;
		}
		case Stage::Message: {
			if (kDown & KEY_B) stage = Stage::List;
			if ((kDown & KEY_Y) && allowUnverifiedRetry) {
				unverified = true;
				startWork(retryJob, retryNet);
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
		case Stage::List: drawList(); break;
		case Stage::Trust: drawTrust(); break;
		case Stage::Working: drawWorking(); break;
		case Stage::Message: drawMessage(); break;
		default: break;
	}

	// ---- the blocking work runs only after the "Working" screen has been shown for two frames
	if (stage == Stage::Working && ++workFrames >= 3) runJob(ms);
}

} // namespace NetworkSwitch
