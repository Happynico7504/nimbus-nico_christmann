// RvtA - Revivetendo analytics sysmodule.
//
// Logs battery, battery temperature, Wi-Fi and shell/sleep state to the SD card so that
// crash reports can be checked against the console's power and thermal state, and uploads
// the log to Revivetendo every 15 minutes while the console is online. A log file is only
// deleted after the server confirmed it.
//
//   /3ds/revivetendo-analytics/id.txt        random console id (32 hex), made once; never a serial or MAC
//   /3ds/revivetendo-analytics/boot.txt      boot counter (row sequence numbers = boot << 24 | row)
//   /3ds/revivetendo-analytics/<fc>.csv      samples written under that friend code (0 = no friend account)
//   /3ds/revivetendo-analytics/<fc>.up       a log being uploaded (kept and retried if the upload fails)
//
// CSV line: seq,time,event,battery_pct,battery_mv,charging,adapter,temp_c,net_state,wifi_connected,
//           wifi_strength,wireless_mode,shell_open,title_id,ac_error,frd_result,act_result
// Unknown MCU values are written as battery_mv 0 and temp_c -128. title_id is the running application
// (0 = none, e.g. HOME Menu). ac_error / frd_result / act_result are the last error codes the Wi-Fi
// connection (AC), Friends and account (ACT) services recorded; an "error" row is written whenever one
// changes and a "title" row whenever another application starts.
//
// Internal problems (SD card not usable, a file could not be written, the server rejecting an upload)
// pulse the notification LED purple a few times, at most once every 10 minutes.
//
// No heap is used: everything lives in static buffers.

#include <3ds.h>
#include <string.h>

#ifndef RVTA_VERSION
#define RVTA_VERSION "0.0.0"
#endif

#define DIR_PATH        "/3ds/revivetendo-analytics"
#define UPLOAD_URL      "https://rvta.nicoch.net/v1/upload"
#define POLL_SECONDS    5
#define SAMPLE_SECONDS  60
#define SLOW_SECONDS    15 // title, error codes and friend code (each needs a session opened and closed)
#define UPLOAD_SECONDS  (15 * 60)
#define MAX_LOG_SIZE    (2 * 1024 * 1024) // stop logging past this while offline for a very long time
#define CHUNK_SIZE      0x8000
#define UNIX_FROM_1900  2208988800LL

// ---------------------------------------------------------------- process setup (no libctru app init)

void __wrap_exit(int rc)
{
    (void)rc;
    svcExitProcess();
}

void __ctru_exit(int rc) { (void)rc; }

Result __sync_init(void);

void initSystem(void)
{
    __sync_init();
    // srv may not be up yet this early in boot
    while (R_FAILED(srvInit()))
        svcSleepThread(500 * 1000 * 1000LL);
    while (R_FAILED(fsInit()))
        svcSleepThread(500 * 1000 * 1000LL);
}

// ---------------------------------------------------------------- small helpers

static void sleepSeconds(u32 s) { svcSleepThread((s64)s * 1000 * 1000 * 1000); }

static s64 unixNow(void) { return (s64)(osGetTime() / 1000) - UNIX_FROM_1900; }

// Appends the decimal form of v to buf at *pos (buffer is always large enough for one line).
static void putInt(char *buf, u32 *pos, s64 v)
{
    char tmp[24];
    int n = 0;
    bool neg = v < 0;
    u64 u = neg ? (u64)(-v) : (u64)v;
    do {
        tmp[n++] = '0' + (u % 10);
        u /= 10;
    } while (u);
    if (neg) buf[(*pos)++] = '-';
    while (n) buf[(*pos)++] = tmp[--n];
}

static void putStr(char *buf, u32 *pos, const char *s)
{
    while (*s) buf[(*pos)++] = *s++;
}

// 12-digit, zero-padded friend code (used for file names)
static void fcDigits(char out[13], u64 fc)
{
    for (int i = 11; i >= 0; i--) {
        out[i] = '0' + (fc % 10);
        fc /= 10;
    }
    out[12] = 0;
}

// ---------------------------------------------------------------- problem signal (notification LED)

#define PROBLEM_LED_COOLDOWN (10 * 60)

static s64 lastProblemLed = -PROBLEM_LED_COOLDOWN;

static void signalProblem(void)
{
    s64 now = unixNow();
    if (now - lastProblemLed < PROBLEM_LED_COOLDOWN) return;
    lastProblemLed = now;

    // 4 purple pulses over 4 seconds, played once, ending dark
    static InfoLedPattern p;
    memset(&p, 0, sizeof p);
    p.delay = 0x02;      // 1/8 s per step
    p.smoothing = 0x40;
    p.loopDelay = 0xFF;  // play once
    for (int i = 0; i < 32; i++) {
        bool on = (i % 8) < 4;
        p.redPattern[i] = on ? 0x80 : 0;
        p.bluePattern[i] = on ? 0xFF : 0;
    }
    p.redPattern[31] = p.bluePattern[31] = 0;
    if (R_SUCCEEDED(mcuHwcInit())) {
        MCUHWC_SetInfoLedPattern(&p);
        mcuHwcExit();
    }
}

// ---------------------------------------------------------------- SD card

static FS_Archive sdmc;
static bool sdReady;

static bool sdOpen(void)
{
    if (sdReady) return true;
    if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) return false;
    FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, "/3ds"), 0);
    FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, DIR_PATH), 0);
    sdReady = true;
    return true;
}

static s64 fileSize(const char *path)
{
    Handle f;
    u64 size = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, sdmc, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0))) return -1;
    FSFILE_GetSize(f, &size);
    FSFILE_Close(f);
    return (s64)size;
}

static bool fileAppend(const char *path, const void *data, u32 len)
{
    Handle f;
    u64 size = 0;
    u32 written = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, sdmc, fsMakePath(PATH_ASCII, path), FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) return false;
    Result rc = FSFILE_GetSize(f, &size);
    if (R_SUCCEEDED(rc)) rc = FSFILE_Write(f, &written, size, data, len, FS_WRITE_FLUSH);
    FSFILE_Close(f);
    return R_SUCCEEDED(rc) && written == len;
}

static bool fileReplace(const char *path, const void *data, u32 len)
{
    Handle f;
    u32 written = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, sdmc, fsMakePath(PATH_ASCII, path), FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) return false;
    Result rc = FSFILE_SetSize(f, 0);
    if (R_SUCCEEDED(rc)) rc = FSFILE_Write(f, &written, 0, data, len, FS_WRITE_FLUSH);
    FSFILE_Close(f);
    return R_SUCCEEDED(rc) && written == len;
}

static u32 fileRead(const char *path, u64 offset, void *buf, u32 max)
{
    Handle f;
    u32 got = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, sdmc, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0))) return 0;
    if (R_FAILED(FSFILE_Read(f, &got, offset, buf, max))) got = 0;
    FSFILE_Close(f);
    return got;
}

// ---------------------------------------------------------------- identity

static char consoleId[33];
static s64 bootCount;
static s64 rowInBoot;

static bool isHex32(const char *s)
{
    for (int i = 0; i < 32; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

static void loadIdentity(void)
{
    char buf[40] = {0};
    if (fileRead(DIR_PATH "/id.txt", 0, buf, 32) == 32 && isHex32(buf)) {
        memcpy(consoleId, buf, 32);
    } else {
        u8 rnd[16];
        bool ok = R_SUCCEEDED(psInit()) && R_SUCCEEDED(PS_GenerateRandomBytes(rnd, sizeof rnd));
        psExit();
        if (!ok) {
            // ps unavailable: fall back to the tick counter and clock, still not tied to the hardware
            u64 a = svcGetSystemTick(), b = osGetTime() ^ (a << 21);
            memcpy(rnd, &a, 8);
            memcpy(rnd + 8, &b, 8);
        }
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            consoleId[2 * i] = hex[rnd[i] >> 4];
            consoleId[2 * i + 1] = hex[rnd[i] & 15];
        }
        consoleId[32] = '\n';
        if (!fileReplace(DIR_PATH "/id.txt", consoleId, 33)) signalProblem();
    }
    consoleId[32] = 0;

    // boot counter: makes row sequence numbers unique across reboots without a write per row
    memset(buf, 0, sizeof buf);
    fileRead(DIR_PATH "/boot.txt", 0, buf, 20);
    s64 n = 0;
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++) n = n * 10 + (buf[i] - '0');
    bootCount = n + 1;
    u32 pos = 0;
    putInt(buf, &pos, bootCount);
    buf[pos++] = '\n';
    if (!fileReplace(DIR_PATH "/boot.txt", buf, pos)) signalProblem();
}

// ---------------------------------------------------------------- slow fields

typedef struct {
    u64 friendCode, titleId;
    u32 acError, frdResult, actResult;
} Slow;

// Luma's pm:dbg command 0x100 (GetCurrentAppInfo): the running application, if any.
static u64 currentTitle(void)
{
    Handle h;
    u64 tid = 0;
    // pm:dbg allows only two sessions, so it is only held for this one request
    if (R_FAILED(srvGetServiceHandle(&h, "pm:dbg"))) return 0;
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x100, 0, 0);
    if (R_SUCCEEDED(svcSendSyncRequest(h)) && R_SUCCEEDED((Result)cmdbuf[1]))
        memcpy(&tid, &cmdbuf[2], sizeof tid);
    svcCloseHandle(h);
    return tid;
}

static bool acReady;

static void readSlow(Slow *s)
{
    memset(s, 0, sizeof *s);
    s->titleId = currentTitle();

    if (R_SUCCEEDED(frdInit(true))) {
        FriendKey key;
        if (R_SUCCEEDED(FRD_GetMyFriendKey(&key)) && key.principalId != 0 &&
            R_FAILED(FRD_PrincipalIdToFriendCode(key.principalId, &s->friendCode)))
            s->friendCode = 0;
        s->frdResult = (u32)FRD_GetLastResponseResult();
        frdExit();
    }
    if (R_SUCCEEDED(actInit(true))) {
        s->actResult = (u32)ACT_GetLastResponseResult();
        actExit();
    }
    if (!acReady) acReady = R_SUCCEEDED(acInit());
    if (acReady) ACU_GetLastErrorCode(&s->acError);
}

// ---------------------------------------------------------------- sampling

typedef struct {
    s64 time;
    int batteryPct, batteryMv, charging, adapter, tempC;
    int netState, wifiConnected, wifiStrength, wirelessMode, shellOpen;
} Sample;

static bool ptmReady, ndmReady;

static void readSample(Sample *s)
{
    memset(s, 0, sizeof *s);
    s->time = unixNow();
    s->tempC = -128;

    if (!ptmReady) ptmReady = R_SUCCEEDED(ptmuInit());
    if (!acReady) acReady = R_SUCCEEDED(acInit());
    if (!ndmReady) ndmReady = R_SUCCEEDED(ndmuInit());

    // MCU registers 0x0A..0x0D: temperature (C), percent, percent fraction, voltage (20 mV units).
    // mcu::HWC allows few sessions, so it is only held while reading (like Rosalina).
    bool mcuOk = false;
    if (R_SUCCEEDED(mcuHwcInit())) {
        u8 d[4];
        if (R_SUCCEEDED(MCUHWC_ReadRegister(0x0A, d, sizeof d))) {
            s->tempC = (s8)d[0];
            s->batteryPct = d[1] > 100 ? 100 : d[1];
            s->batteryMv = d[3] * 20;
            mcuOk = true;
        }
        mcuHwcExit();
    }

    if (ptmReady) {
        u8 v = 0;
        bool b = false;
        if (!mcuOk && R_SUCCEEDED(PTMU_GetBatteryLevel(&v))) s->batteryPct = v > 5 ? 100 : v * 20; // coarse 0-5 scale
        if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&v))) s->charging = v ? 1 : 0;
        if (R_SUCCEEDED(PTMU_GetAdapterState(&b))) s->adapter = b ? 1 : 0;
        if (R_SUCCEEDED(PTMU_GetShellState(&v))) s->shellOpen = v ? 1 : 0;
    }

    s->netState = OS_SharedConfig->network_state;
    s->wifiStrength = osGetWifiStrength() & 3;

    if (acReady) {
        u32 st = 0;
        if (R_SUCCEEDED(ACU_GetStatus(&st))) s->wifiConnected = st == 3 ? 1 : 0;
    }
    if (ndmReady) {
        ndmExclusiveState ex = NDM_EXCLUSIVE_STATE_NONE;
        if (R_SUCCEEDED(NDMU_GetExclusiveState(&ex))) s->wirelessMode = (int)ex & 0xFF;
    }
}

static char logPath[64];
static u64 logFc = (u64)-1;

static void selectLog(u64 fc)
{
    if (fc == logFc) return;
    char d[13];
    fcDigits(d, fc);
    u32 pos = 0;
    putStr(logPath, &pos, DIR_PATH "/");
    putStr(logPath, &pos, d);
    putStr(logPath, &pos, ".csv");
    logPath[pos] = 0;
    logFc = fc;
}

static Slow slow;

static void writeRow(const char *event, const Sample *s)
{
    if (!sdOpen()) {
        signalProblem();
        return;
    }
    if (fileSize(logPath) > MAX_LOG_SIZE) return;

    char line[256];
    u32 p = 0;
    putInt(line, &p, (bootCount << 24) | (rowInBoot++ & 0xFFFFFF)); line[p++] = ',';
    putInt(line, &p, s->time);           line[p++] = ',';
    putStr(line, &p, event);             line[p++] = ',';
    putInt(line, &p, s->batteryPct);     line[p++] = ',';
    putInt(line, &p, s->batteryMv);      line[p++] = ',';
    putInt(line, &p, s->charging);       line[p++] = ',';
    putInt(line, &p, s->adapter);        line[p++] = ',';
    putInt(line, &p, s->tempC);          line[p++] = ',';
    putInt(line, &p, s->netState);       line[p++] = ',';
    putInt(line, &p, s->wifiConnected);  line[p++] = ',';
    putInt(line, &p, s->wifiStrength);   line[p++] = ',';
    putInt(line, &p, s->wirelessMode);   line[p++] = ',';
    putInt(line, &p, s->shellOpen);      line[p++] = ',';
    putInt(line, &p, (s64)slow.titleId); line[p++] = ',';
    putInt(line, &p, slow.acError);      line[p++] = ',';
    putInt(line, &p, slow.frdResult);    line[p++] = ',';
    putInt(line, &p, slow.actResult);    line[p++] = '\n';
    if (!fileAppend(logPath, line, p)) signalProblem();
}

// ---------------------------------------------------------------- upload

static u8 chunk[CHUNK_SIZE] __attribute__((aligned(4)));
static bool httpReady;

static bool post(const u8 *data, u32 len, const char *fcHeader)
{
    httpcContext ctx;
    if (R_FAILED(httpcOpenContext(&ctx, HTTPC_METHOD_POST, UPLOAD_URL, 1))) return false;
    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "RvtA/" RVTA_VERSION);
    httpcAddRequestHeaderField(&ctx, "Content-Type", "text/csv");
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Id", consoleId);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Version", RVTA_VERSION);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-FC", fcHeader);

    bool ok = false;
    u32 status = 0;
    if (R_SUCCEEDED(httpcAddPostDataRaw(&ctx, (u32 *)data, len)) && R_SUCCEEDED(httpcBeginRequest(&ctx)) &&
        R_SUCCEEDED(httpcGetResponseStatusCode(&ctx, &status)) && status == 200) {
        char resp[16] = {0};
        u32 got = 0;
        httpcDownloadData(&ctx, (u8 *)resp, sizeof resp - 1, &got);
        ok = got >= 3 && memcmp(resp, "ok ", 3) == 0; // the server's confirmation
    }
    // 4xx = the server refused our data or id (being offline or a server outage is not our problem)
    if (status >= 400 && status < 500) signalProblem();
    httpcCloseContext(&ctx);
    return ok;
}

// Uploads one ".up" file in line-aligned chunks and deletes it once every chunk was confirmed.
static bool uploadFile(const char *path, const char *fcHeader)
{
    u64 offset = 0;
    for (;;) {
        u32 got = fileRead(path, offset, chunk, sizeof chunk);
        if (got == 0) break;
        u32 len = got;
        if (got == sizeof chunk) {
            while (len > 0 && chunk[len - 1] != '\n') len--;
            if (len == 0) len = got; // a single over-long line: send it anyway, the server skips it
        }
        if (!post(chunk, len, fcHeader)) return false;
        offset += len;
    }
    FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, path));
    return true;
}

static bool online(void)
{
    if (!acReady) acReady = R_SUCCEEDED(acInit());
    u32 st = 0;
    return acReady && R_SUCCEEDED(ACU_GetStatus(&st)) && st == 3;
}

// File names are "<12 digits>.csv" / "<12 digits>.up"; returns false for anything else.
static bool parseLogName(const u16 *name, char digits[13], bool *isUp)
{
    int i = 0;
    for (; i < 12; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
        digits[i] = (char)name[i];
    }
    digits[12] = 0;
    if (name[12] != '.') return false;
    if (name[13] == 'c' && name[14] == 's' && name[15] == 'v' && name[16] == 0) { *isUp = false; return true; }
    if (name[13] == 'u' && name[14] == 'p' && name[15] == 0) { *isUp = true; return true; }
    return false;
}

static void uploadAll(void)
{
    if (!sdOpen() || !online()) return;
    if (!httpReady) httpReady = R_SUCCEEDED(httpcInit(0));
    if (!httpReady) return;

    // Collect names first: renaming/deleting while the directory is open is not safe.
    static char names[16][13];
    static bool ups[16];
    int count = 0;
    Handle dir;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, sdmc, fsMakePath(PATH_ASCII, DIR_PATH)))) return;
    FS_DirectoryEntry entry;
    u32 n = 0;
    while (count < 16 && R_SUCCEEDED(FSDIR_Read(dir, &n, 1, &entry)) && n == 1) {
        if (!(entry.attributes & FS_ATTRIBUTE_DIRECTORY) && parseLogName(entry.name, names[count], &ups[count])) count++;
    }
    FSDIR_Close(dir);

    for (int i = 0; i < count; i++) {
        char csv[64], up[64];
        u32 a = 0, b = 0;
        putStr(csv, &a, DIR_PATH "/"); putStr(csv, &a, names[i]); putStr(csv, &a, ".csv"); csv[a] = 0;
        putStr(up, &b, DIR_PATH "/");  putStr(up, &b, names[i]);  putStr(up, &b, ".up");   up[b] = 0;

        if (!ups[i]) {
            // a pending ".up" for this friend code is uploaded on its own entry; never overwrite it
            if (fileSize(up) >= 0) continue;
            if (R_FAILED(FSUSER_RenameFile(sdmc, fsMakePath(PATH_ASCII, csv), sdmc, fsMakePath(PATH_ASCII, up)))) continue;
        }

        // the server takes "0" for no friend account
        const char *fc = names[i];
        while (*fc == '0' && fc[1]) fc++;
        if (!uploadFile(up, fc)) return; // offline again or server trouble: retry next time
    }
}

// ---------------------------------------------------------------- main loop

int main(void)
{
    // let the rest of boot settle; the services we read are started as our dependencies
    sleepSeconds(20);
    while (!sdOpen()) {
        signalProblem();
        sleepSeconds(5);
    }
    loadIdentity();

    Sample cur, last;
    readSlow(&slow);
    selectLog(slow.friendCode);
    readSample(&cur);
    writeRow("boot", &cur);
    last = cur;

    s64 lastTick = cur.time, lastSample = cur.time, lastSlow = cur.time, lastUpload = cur.time - UPLOAD_SECONDS + 120; // first upload ~2 min after boot

    for (;;) {
        sleepSeconds(POLL_SECONDS);
        readSample(&cur);

        // Threads do not run while the console sleeps: a big gap means it slept.
        if (cur.time - lastTick > POLL_SECONDS + 30) {
            Sample before = last;
            before.time = lastTick;
            writeRow("sleep", &before);
            writeRow("wake", &cur);
            lastSample = cur.time;
        }
        lastTick = cur.time;

        if (cur.time - lastSlow >= SLOW_SECONDS) {
            lastSlow = cur.time;
            Slow prev = slow;
            readSlow(&slow);
            selectLog(slow.friendCode);
            if (slow.titleId != prev.titleId) writeRow("title", &cur);
            if (slow.acError != prev.acError || slow.frdResult != prev.frdResult || slow.actResult != prev.actResult)
                writeRow("error", &cur);
        }

        if (cur.shellOpen != last.shellOpen) {
            writeRow(cur.shellOpen ? "shell_open" : "shell_close", &cur);
            lastSample = cur.time;
        } else if (cur.charging != last.charging || cur.adapter != last.adapter || cur.time - lastSample >= SAMPLE_SECONDS) {
            writeRow("sample", &cur);
            lastSample = cur.time;
        }
        last = cur;

        if (cur.time - lastUpload >= UPLOAD_SECONDS) {
            lastUpload = cur.time;
            uploadAll();
        }
    }
    return 0;
}
