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
// Internal problems pulse the notification LED purple, at most once every 10 minutes; the number of
// pulses is the problem (1 = SD card not usable, 2 = a file could not be written, 3 = upload refused).
// A confirmed upload shows one short yellow pulse.
//
// Everything lives in static buffers, except a small heap set up at the first upload: http:C keeps POST
// data in a shared memory block the client provides (libctru's httpcInit allocates it on the heap).

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
#define FIRST_UPLOAD_DELAY 15 // seconds after the boot row
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

// Progress and last results, readable in Rosalina's process memory viewer (search for "RvtAstat")
// when nothing reaches the SD card. stage: 1 srv, 2 fs, 3 boot delay over, 4 SD open, 5 id loaded,
// 6 first row written, 7 main loop.
typedef struct {
    char magic[8];
    u32 stage;
    u32 sdResult;     // last FSUSER_OpenArchive result
    u32 writeResult;  // last failed write result
    u32 mcuResult;    // last mcu::HWC open result
    u32 problems;     // problem signals so far
    u32 rows;         // rows written this boot
    u32 uploads;      // chunks confirmed by the server this boot
    u32 srvTries;     // srvInit attempts at startup
    u32 srvResult;    // last srvInit result
    u32 fsTries;      // fsInit attempts at startup
    u32 fsResult;     // last fsInit result
    u32 uploadRounds; // upload rounds started (every 15 min)
    u32 acStatus;     // ACU_GetStatus value at the last round (3 = online)
    u32 httpInit;     // last httpcInit result
    u32 filesFound;   // log files found in the last round
    u32 openResult;   // last httpcOpenContext result
    u32 beginResult;  // last httpcBeginRequest result
    u32 httpStatus;   // last HTTP status code
    u32 addResult;    // last httpcAddPostDataRaw result (first try)
    u32 typeResult;   // SetPostDataType(raw) result, when the first try failed
    u32 addRetry;     // httpcAddPostDataRaw result after SetPostDataType(raw)
} Status;

volatile Status g_status __attribute__((used)) = {{'R', 'v', 't', 'A', 's', 't', 'a', 't'}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

Result __sync_init(void);

#ifdef RVTA_PROBE
static void startWatchdog(void);
static u32 g_fsTries, g_lastFsResult;
#endif

void initSystem(void)
{
    __sync_init();
#ifdef RVTA_PROBE
    startWatchdog();
#endif
    // srv may not be up yet this early in boot
    for (;;) {
        Result rc = srvInit();
        g_status.srvTries++;
        g_status.srvResult = (u32)rc;
        if (R_SUCCEEDED(rc)) break;
        svcSleepThread(500 * 1000 * 1000LL);
    }
    // Never wait for a full service port (mcu::HWC, pm:dbg... have very few sessions): fail and retry later.
    srvSetBlockingPolicy(true);
    g_status.stage = 1;
    for (;;) {
        Result rc = fsInit();
        g_status.fsTries++;
        g_status.fsResult = (u32)rc;
#ifdef RVTA_PROBE
        g_fsTries++;
        g_lastFsResult = (u32)rc;
#endif
        if (R_SUCCEEDED(rc)) break;
        svcSleepThread(500 * 1000 * 1000LL);
    }
    g_status.stage = 2;
}

// Luma's svcControlService(SERVICEOP_STEAL_CLIENT_SESSION): a copy of another process's client session,
// used when a service port is full (Rosalina does the same for mcu::HWC).
static Result stealSession(Handle *out, const char *name)
{
    register u32 r0 asm("r0") = 0;
    register Handle *r1 asm("r1") = out;
    register const char *r2 asm("r2") = name;
    asm volatile("svc 0xB0" : "+r"(r0) : "r"(r1), "r"(r2) : "r3", "r12", "lr", "memory");
    return (Result)r0;
}

static bool mcuBorrowed;

static bool mcuOpen(void)
{
    Result rc = mcuHwcInit();
    if (R_SUCCEEDED(rc)) {
        mcuBorrowed = false;
        return true;
    }
    Handle h = 0;
    Result rc2 = stealSession(&h, "mcu::HWC");
    g_status.mcuResult = R_FAILED(rc2) ? (u32)rc2 : (u32)rc;
    if (R_FAILED(rc2) || h == 0) return false;
    *mcuHwcGetSessionHandle() = h;
    mcuBorrowed = true;
    return true;
}

static void mcuClose(void)
{
    if (mcuBorrowed) {
        svcCloseHandle(*mcuHwcGetSessionHandle());
        *mcuHwcGetSessionHandle() = 0;
        mcuBorrowed = false;
    } else {
        mcuHwcExit();
    }
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

// Problem codes = number of purple pulses.
#define PROBLEM_SD_OPEN   1 // the SD card could not be opened
#define PROBLEM_WRITE     2 // a file could not be written
#define PROBLEM_REJECTED  3 // the server refused an upload (4xx)

// One short yellow pulse (0.25 s) after an upload round in which the server confirmed a log file.
static void signalUploaded(void)
{
    static InfoLedPattern p;
    memset(&p, 0, sizeof p);
    p.delay = 0x02;      // 1/8 s per step: 2 steps lit = 0.25 s
    p.smoothing = 0x00;
    p.loopDelay = 0xFF;  // play once
    p.redPattern[0] = p.redPattern[1] = 0xFF;
    p.greenPattern[0] = p.greenPattern[1] = 0xA0;
    if (mcuOpen()) {
        MCUHWC_SetInfoLedPattern(&p);
        mcuClose();
    }
}

static void logProblem(int code);

static void signalProblem(int code)
{
    g_status.problems++;
    logProblem(code);
    s64 now = unixNow();
    if (now - lastProblemLed < PROBLEM_LED_COOLDOWN) return;
    lastProblemLed = now;

    // `code` purple pulses (1/2 s on, 1/2 s off), played once, ending dark
    static InfoLedPattern p;
    memset(&p, 0, sizeof p);
    p.delay = 0x02;      // 1/8 s per step
    p.smoothing = 0x20;
    p.loopDelay = 0xFF;  // play once
    for (int i = 0; i < 32; i++) {
        bool on = i / 8 < code && (i % 8) < 4;
        p.redPattern[i] = on ? 0x80 : 0;
        p.bluePattern[i] = on ? 0xFF : 0;
    }
    if (mcuOpen()) {
        MCUHWC_SetInfoLedPattern(&p);
        mcuClose();
    }
}

// ---------------------------------------------------------------- SD card

static FS_Archive sdmc;
static bool sdReady;

static bool sdOpen(void)
{
    if (sdReady) return true;
    Result rc = FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    g_status.sdResult = (u32)rc;
    if (R_FAILED(rc)) return false;
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
    Result rc = FSUSER_OpenFile(&f, sdmc, fsMakePath(PATH_ASCII, path), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_FAILED(rc)) { g_status.writeResult = (u32)rc; return false; }
    rc = FSFILE_GetSize(f, &size);
    if (R_SUCCEEDED(rc)) rc = FSFILE_Write(f, &written, size, data, len, FS_WRITE_FLUSH);
    FSFILE_Close(f);
    if (R_FAILED(rc)) g_status.writeResult = (u32)rc;
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

// ---------------------------------------------------------------- debug log (/3ds/revivetendo-analytics/rvta.log)
// Human-readable trace for diagnosing RvtA itself; stays on the SD card, capped at 64 KB (cleared when full).

#define DEBUG_LOG DIR_PATH "/rvta.log"
#define DEBUG_LOG_MAX (64 * 1024)

typedef struct {
    char b[240];
    u32 n;
} Line;

static void lnStr(Line *l, const char *str)
{
    while (*str && l->n < sizeof l->b - 2) l->b[l->n++] = *str++;
}

static void lnInt(Line *l, s64 v)
{
    char t[24];
    u32 p = 0;
    putInt(t, &p, v);
    t[p] = 0;
    lnStr(l, t);
}

static void lnHex(Line *l, u32 v)
{
    static const char h[] = "0123456789ABCDEF";
    char t[9];
    for (int i = 0; i < 8; i++) t[i] = h[(v >> (28 - 4 * i)) & 15];
    t[8] = 0;
    lnStr(l, t);
}

static void lnStart(Line *l)
{
    l->n = 0;
    lnInt(l, unixNow());
    lnStr(l, " ");
}

static void lnWrite(Line *l)
{
    if (!sdReady) return;
    l->b[l->n++] = '\n';
    if (fileSize(DEBUG_LOG) > DEBUG_LOG_MAX) fileReplace(DEBUG_LOG, "", 0);
    fileAppend(DEBUG_LOG, l->b, l->n);
}

static void logProblem(int code)
{
    static const char *const what[] = {"?", "SD card could not be opened", "write failed", "upload refused (4xx)"};
    Line l;
    lnStart(&l);
    lnStr(&l, "problem ");
    lnInt(&l, code);
    lnStr(&l, ": ");
    lnStr(&l, what[code >= 1 && code <= 3 ? code : 0]);
    lnStr(&l, " sd=");
    lnHex(&l, g_status.sdResult);
    lnStr(&l, " write=");
    lnHex(&l, g_status.writeResult);
    lnWrite(&l);
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
        if (!fileReplace(DIR_PATH "/id.txt", consoleId, 33)) signalProblem(PROBLEM_WRITE);
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
    if (!fileReplace(DIR_PATH "/boot.txt", buf, pos)) signalProblem(PROBLEM_WRITE);
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
    if (mcuOpen()) {
        u8 d[4];
        if (R_SUCCEEDED(MCUHWC_ReadRegister(0x0A, d, sizeof d))) {
            s->tempC = (s8)d[0];
            s->batteryPct = d[1] > 100 ? 100 : d[1];
            s->batteryMv = d[3] * 20;
            mcuOk = true;
        }
        mcuClose();
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
        signalProblem(PROBLEM_SD_OPEN);
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
    if (!fileAppend(logPath, line, p)) signalProblem(PROBLEM_WRITE);
    else g_status.rows++;
}

// ---------------------------------------------------------------- upload

static u8 chunk[CHUNK_SIZE] __attribute__((aligned(4)));
static bool httpReady;

// libctru's normal startup (skipped here) allocates the heap the same way.
#define HEAP_SIZE        0x20000 // 128 KB
#define HTTP_POST_BUFFER 0x10000 // 64 KB: one 32 KB chunk plus headers
extern char *fake_heap_start, *fake_heap_end;

static Result ensureHeap(void)
{
    if (fake_heap_end) return 0;
    u32 addr = 0;
    Result rc = svcControlMemory(&addr, OS_HEAP_AREA_BEGIN, 0, HEAP_SIZE, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
    if (R_FAILED(rc)) return rc;
    fake_heap_start = (char *)addr;
    fake_heap_end = fake_heap_start + HEAP_SIZE;
    return 0;
}

// http:C SetPostDataType (0x15): 2 = raw. Not wrapped by libctru.
static Result httpcSetPostDataTypeRaw(httpcContext *context)
{
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x15, 2, 0);
    cmdbuf[1] = context->httphandle;
    cmdbuf[2] = 2;
    Result ret = svcSendSyncRequest(context->servhandle);
    return R_FAILED(ret) ? ret : (Result)cmdbuf[1];
}

static void logPost(const char *fc, u32 len, Result open, const Result *add, const Result *type, const Result *retry,
                    const Result *begin, u32 status, bool ok)
{
    Line l;
    lnStart(&l);
    lnStr(&l, "post fc=");
    lnStr(&l, fc);
    lnStr(&l, " len=");
    lnInt(&l, len);
    lnStr(&l, " open=");
    lnHex(&l, (u32)open);
    // "-" = this step was not reached
    lnStr(&l, " add=");    if (add) lnHex(&l, (u32)*add); else lnStr(&l, "-");
    lnStr(&l, " type=");   if (type) lnHex(&l, (u32)*type); else lnStr(&l, "-");
    lnStr(&l, " retry=");  if (retry) lnHex(&l, (u32)*retry); else lnStr(&l, "-");
    lnStr(&l, " begin=");  if (begin) lnHex(&l, (u32)*begin); else lnStr(&l, "-");
    lnStr(&l, " http=");
    lnInt(&l, status);
    lnStr(&l, ok ? " ok" : " FAILED");
    lnWrite(&l);
}

static bool post(const u8 *data, u32 len, const char *fcHeader)
{
    httpcContext ctx;
    Result orc = httpcOpenContext(&ctx, HTTPC_METHOD_POST, UPLOAD_URL, 1);
    g_status.openResult = (u32)orc;
    if (R_FAILED(orc)) {
        logPost(fcHeader, len, orc, NULL, NULL, NULL, NULL, 0, false);
        return false;
    }
    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "RvtA/" RVTA_VERSION);
    httpcAddRequestHeaderField(&ctx, "Content-Type", "text/csv");
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Id", consoleId);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Version", RVTA_VERSION);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-FC", fcHeader);

    bool ok = false;
    u32 status = 0;
    bool triedType = false, reachedBegin = false;
    Result addRc = httpcAddPostDataRaw(&ctx, (u32 *)data, len), typeRc = 0, retryRc = 0, beginRc = 0;
    g_status.addResult = (u32)addRc;
    Result dataRc = addRc;
    if (R_FAILED(addRc)) {
        // the default POST type is form data; raw data may need the type set first
        triedType = true;
        typeRc = httpcSetPostDataTypeRaw(&ctx);
        retryRc = httpcAddPostDataRaw(&ctx, (u32 *)data, len);
        g_status.typeResult = (u32)typeRc;
        g_status.addRetry = (u32)retryRc;
        dataRc = retryRc;
    }
    if (R_SUCCEEDED(dataRc)) {
        reachedBegin = true;
        beginRc = httpcBeginRequest(&ctx);
        g_status.beginResult = (u32)beginRc;
        if (R_SUCCEEDED(beginRc) && R_SUCCEEDED(httpcGetResponseStatusCode(&ctx, &status))) g_status.httpStatus = status;
        if (R_SUCCEEDED(beginRc) && status == 200) {
            char resp[16] = {0};
            u32 got = 0;
            httpcDownloadData(&ctx, (u8 *)resp, sizeof resp - 1, &got);
            ok = got >= 3 && memcmp(resp, "ok ", 3) == 0; // the server's confirmation
        }
    }
    logPost(fcHeader, len, orc, &addRc, triedType ? &typeRc : NULL, triedType ? &retryRc : NULL,
            reachedBegin ? &beginRc : NULL, status, ok);

    // 4xx = the server refused our data or id (being offline or a server outage is not our problem)
    if (status >= 400 && status < 500) signalProblem(PROBLEM_REJECTED);
    if (ok) g_status.uploads++;
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
    bool ok = acReady && R_SUCCEEDED(ACU_GetStatus(&st));
    g_status.acStatus = ok ? st : 0xFFFFFFFF;
    return ok && st == 3;
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
    bool uploaded = false;
    g_status.uploadRounds++;
    if (!sdOpen()) return;
    if (!online()) {
        Line l;
        lnStart(&l);
        lnStr(&l, "upload round skipped: offline (ac status ");
        lnHex(&l, g_status.acStatus);
        lnStr(&l, ")");
        lnWrite(&l);
        return;
    }
    if (!httpReady) {
        Result hrc = ensureHeap();
        if (R_FAILED(hrc)) {
            Line l;
            lnStart(&l);
            lnStr(&l, "heap allocation failed ");
            lnHex(&l, (u32)hrc);
            lnWrite(&l);
        } else {
            hrc = httpcInit(HTTP_POST_BUFFER);
        }
        g_status.httpInit = (u32)hrc;
        httpReady = R_SUCCEEDED(hrc);
    }
    if (!httpReady) {
        Line l;
        lnStart(&l);
        lnStr(&l, "upload round skipped: httpcInit ");
        lnHex(&l, g_status.httpInit);
        lnWrite(&l);
        return;
    }

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
    g_status.filesFound = (u32)count;
    {
        Line l;
        lnStart(&l);
        lnStr(&l, "upload round ");
        lnInt(&l, g_status.uploadRounds);
        lnStr(&l, ": online, ");
        lnInt(&l, count);
        lnStr(&l, " log file(s)");
        lnWrite(&l);
    }

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
        bool done = uploadFile(up, fc);
        Line l;
        lnStart(&l);
        lnStr(&l, done ? "upload ok: " : "upload failed, kept for next time: ");
        lnStr(&l, names[i]);
        lnStr(&l, ".up");
        lnWrite(&l);
        if (!done) break; // offline again or server trouble: retry next time
        uploaded = true;
    }
    if (uploaded) signalUploaded();
}

// ---------------------------------------------------------------- main loop

#ifdef RVTA_PROBE
// Diagnostic build (make RVTA_PROBE=1): reports progress through the notification LED, and a watchdog
// thread crashes RvtA on purpose after 75 s if the probe has not finished, so Luma's exception screen
// shows where it stopped: R0 = stage (see g_status), R1 = fsInit tries, R2 = last fsInit result,
// R3 = last mcu::HWC result, R4 = SD open result. Process name on the screen: RvtA.
static volatile bool g_probeDone;
static u8 watchdogStack[0x1000] __attribute__((aligned(8)));

static void watchdog(void *arg)
{
    (void)arg;
    svcSleepThread(75LL * 1000 * 1000 * 1000);
    if (g_probeDone) svcExitThread();
    register u32 r0 asm("r0") = g_status.stage;
    register u32 r1 asm("r1") = g_fsTries;
    register u32 r2 asm("r2") = g_lastFsResult;
    register u32 r3 asm("r3") = g_status.mcuResult;
    register u32 r4 asm("r4") = g_status.sdResult;
    asm volatile(".word 0xE7F000F0" :: "r"(r0), "r"(r1), "r"(r2), "r"(r3), "r"(r4)); // undefined instruction
    for (;;) svcSleepThread(1000000000LL);
}

static void startWatchdog(void)
{
    Handle t;
    // priority may not be higher (numerically lower) than the process's (0x3C, see rvta.rsf)
    svcCreateThread(&t, watchdog, 0, (u32 *)(watchdogStack + sizeof watchdogStack), 0x3C, -2);
}

static void ledPulse(u8 r, u8 g, u8 b)
{
    static InfoLedPattern p;
    memset(&p, 0, sizeof p);
    p.delay = 0x02;
    p.smoothing = 0x20;
    p.loopDelay = 0xFF;
    for (int i = 0; i < 16; i++) {
        p.redPattern[i] = r;
        p.greenPattern[i] = g;
        p.bluePattern[i] = b;
    }
    if (mcuOpen()) {
        MCUHWC_SetInfoLedPattern(&p);
        mcuClose();
    }
    sleepSeconds(4);
}

static void probe(void)
{
    g_status.stage = 3;
    sleepSeconds(20);
    g_status.stage = 4;
    ledPulse(0, 0xFF, 0);                // 1 green: code runs, srv ok
    g_status.stage = 5;
    ledPulse(0, 0, 0xFF);                // 2 blue: fsInit ok (initSystem finished)
    g_status.stage = 6;
    Result rc = FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    g_status.sdResult = (u32)rc;
    if (R_SUCCEEDED(rc)) {
        sdReady = true;
        static const char msg[] = "RvtA probe: SD ok\n";
        bool ok = fileReplace("/3ds/rvta-probe.txt", msg, sizeof msg - 1);
        g_status.stage = ok ? 8 : 7;
        if (ok) ledPulse(0xFF, 0xFF, 0xFF); // 3 white: SD open + write ok
        else ledPulse(0xFF, 0x60, 0);        // 3 orange: SD open, write failed
    } else {
        ledPulse(0x80, 0, 0xFF);              // 3 purple: SD open failed
    }
    g_probeDone = true;
    for (;;) sleepSeconds(3600);
}
#endif

int main(void)
{
#ifdef RVTA_PROBE
    probe();
#endif
    // let the rest of boot settle briefly; services that are not up yet are retried later anyway
    sleepSeconds(5);
    g_status.stage = 3;
    while (!sdOpen()) {
        signalProblem(PROBLEM_SD_OPEN);
        sleepSeconds(5);
    }
    g_status.stage = 4;
    loadIdentity();
    g_status.stage = 5;

    Sample cur, last;
    readSlow(&slow);
    selectLog(slow.friendCode);
    readSample(&cur);
    writeRow("boot", &cur);
    {
        Line l;
        lnStart(&l);
        lnStr(&l, "boot v" RVTA_VERSION " boot#");
        lnInt(&l, bootCount);
        lnStr(&l, " srv tries=");
        lnInt(&l, g_status.srvTries);
        lnStr(&l, " fs tries=");
        lnInt(&l, g_status.fsTries);
        lnStr(&l, " mcu=");
        lnHex(&l, g_status.mcuResult);
        lnStr(&l, " fc=");
        char d[13];
        fcDigits(d, slow.friendCode);
        lnStr(&l, d);
        lnWrite(&l);
    }
    g_status.stage = 6;
    last = cur;

    s64 lastTick = cur.time, lastSample = cur.time, lastSlow = cur.time,
        lastUpload = cur.time - UPLOAD_SECONDS + FIRST_UPLOAD_DELAY; // first upload shortly after boot

    g_status.stage = 7;
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
