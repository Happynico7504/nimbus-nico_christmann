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
// Memory is kept small on purpose (see "upload memory" below): a 32 KB heap for the thread stacks, and the
// buffers an upload needs only while the upload runs.

#include <3ds.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>

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
void __system_initSyscalls(void);

#ifdef RVTA_PROBE
static void startWatchdog(void);
static u32 g_fsTries, g_lastFsResult;
#endif

void initSystem(void)
{
    __sync_init();
    // Thread vars for newlib (malloc/memalign check them and svcBreak without): libctru's startup, which is
    // skipped here, sets them up the same way.
    __system_initSyscalls();
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

static LightLock g_logLock; // three threads write to the log

static void lnWrite(Line *l)
{
    if (!sdReady) return;
    l->b[l->n++] = '\n';
    LightLock_Lock(&g_logLock);
    if (fileSize(DEBUG_LOG) > DEBUG_LOG_MAX) fileReplace(DEBUG_LOG, "", 0);
    fileAppend(DEBUG_LOG, l->b, l->n);
    LightLock_Unlock(&g_logLock);
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
    if (R_SUCCEEDED(acInit())) {
        ACU_GetLastErrorCode(&s->acError);
        acExit();
    }
}

// ---------------------------------------------------------------- sampling

typedef struct {
    s64 time;
    int batteryPct, batteryMv, charging, adapter, tempC;
    int netState, wifiConnected, wifiStrength, wirelessMode, shellOpen;
} Sample;

// Service sessions are only held while they are used: ports like hid:USER, ac:u or ptm:u have few
// sessions, and a session RvtA keeps is one the running game or applet cannot get (hid ran out with
// Animal Crossing + Miiverse: ErrDisp 0xD8601837 in hid, 2026-10-11).
static void readSample(Sample *s)
{
    memset(s, 0, sizeof *s);
    s->time = unixNow();
    s->tempC = -128;


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

    if (R_SUCCEEDED(ptmuInit())) {
        u8 v = 0;
        bool b = false;
        if (!mcuOk && R_SUCCEEDED(PTMU_GetBatteryLevel(&v))) s->batteryPct = v > 5 ? 100 : v * 20; // coarse 0-5 scale
        if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&v))) s->charging = v ? 1 : 0;
        if (R_SUCCEEDED(PTMU_GetAdapterState(&b))) s->adapter = b ? 1 : 0;
        if (R_SUCCEEDED(PTMU_GetShellState(&v))) s->shellOpen = v ? 1 : 0;
        ptmuExit();
    }

    s->netState = OS_SharedConfig->network_state;
    s->wifiStrength = osGetWifiStrength() & 3;

    if (R_SUCCEEDED(acInit())) {
        u32 st = 0;
        if (R_SUCCEEDED(ACU_GetStatus(&st))) s->wifiConnected = st == 3 ? 1 : 0;
        acExit();
    }
    if (R_SUCCEEDED(ndmuInit())) {
        ndmExclusiveState ex = NDM_EXCLUSIVE_STATE_NONE;
        if (R_SUCCEEDED(NDMU_GetExclusiveState(&ex))) s->wirelessMode = (int)ex & 0xFF;
        ndmuExit();
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

// ---------------------------------------------------------------- upload memory

static bool httpReady;

// RvtA lives in the BASE memory region, which it shares with every system module and with the kernel's own
// allocations. Keeping 768 KB there for good made the kernel panic (OutOfResource) in big applets and games
// (eShop, Animal Crossing; crash dumps #7 and #9, 2026-10-11). So the permanent heap only holds the two
// thread structs and stacks, and an upload round commits what it needs (log chunk, http:C's POST buffer,
// dump chunk) when it starts and gives it back to the system when it ends.
//
// The heap is newlib's (sbrk between fake_heap_start and fake_heap_end, as in libctru's normal startup);
// a round extends it with pages right after its end and shrinks it again after malloc_trim.
#define HEAP_BASE_SIZE   0x8000  // 32 KB: thread structs + 2 x 8 KB stacks
#define ROUND_SMALL      0x20000 // 128 KB: 32 KB log chunk + 64 KB POST buffer
#define ROUND_LARGE      0x60000 // 384 KB: also a 128 KB dump chunk with a 160 KB POST buffer (memory dumps only)
#define DUMP_CHUNK_LARGE 0x20000
extern char *fake_heap_start, *fake_heap_end;

static u32 g_heapBaseEnd;
static u32 g_postBufferSize;
static u32 g_dumpChunkSize;
static u8 *g_chunk;   // CHUNK_SIZE bytes, only during a round
static u8 *g_dumpBuf; // g_dumpChunkSize bytes, only during a round (may be g_chunk)

static Result ensureHeap(void)
{
    if (fake_heap_end) return 0;
    u32 addr = 0;
    Result rc = svcControlMemory(&addr, OS_HEAP_AREA_BEGIN, 0, HEAP_BASE_SIZE, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
    if (R_FAILED(rc)) return rc;
    fake_heap_start = (char *)addr;
    fake_heap_end = fake_heap_start + HEAP_BASE_SIZE;
    g_heapBaseEnd = addr + HEAP_BASE_SIZE;
    return 0;
}

// Commits the round's memory. large = a memory dump is waiting (big chunks); falls back to the small size.
static bool roundBegin(bool large)
{
    u32 end = (u32)fake_heap_end;
    u32 have = end - g_heapBaseEnd; // left over from a round that could not shrink the heap
    u32 want = large ? ROUND_LARGE : ROUND_SMALL;
    if (have < want) {
        u32 addr = 0;
        Result rc = svcControlMemory(&addr, end, 0, want - have, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
        if (R_FAILED(rc) && large) {
            large = false;
            want = ROUND_SMALL;
            rc = have < want ? svcControlMemory(&addr, end, 0, want - have, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE) : 0;
        }
        if (R_FAILED(rc)) {
            Line l;
            lnStart(&l);
            lnStr(&l, "upload round skipped: no memory ");
            lnHex(&l, (u32)rc);
            lnWrite(&l);
            return false;
        }
        if (have < want) fake_heap_end = (char *)(g_heapBaseEnd + want);
    }
    g_chunk = malloc(CHUNK_SIZE);
    g_dumpBuf = large ? memalign(0x1000, DUMP_CHUNK_LARGE) : NULL;
    if (!g_chunk) {
        free(g_dumpBuf);
        g_dumpBuf = NULL;
        return false;
    }
    if (g_dumpBuf) {
        g_dumpChunkSize = DUMP_CHUNK_LARGE;
        g_postBufferSize = 0x28000;
    } else {
        g_dumpBuf = g_chunk;
        g_dumpChunkSize = CHUNK_SIZE;
        g_postBufferSize = 0x10000;
    }
    return true;
}

// Closes http:C (which frees its POST buffer) and hands every page above what is still in use back.
static void roundEnd(void)
{
    if (httpReady) {
        httpcExit();
        httpReady = false;
    }
    if (g_dumpBuf != g_chunk) free(g_dumpBuf);
    free(g_chunk);
    g_chunk = g_dumpBuf = NULL;
    malloc_trim(0);
    u32 keep = ((u32)sbrk(0) + 0xFFF) & ~0xFFFu;
    if (keep < g_heapBaseEnd) keep = g_heapBaseEnd;
    u32 end = (u32)fake_heap_end;
    if (keep < end) {
        u32 addr = 0;
        fake_heap_end = (char *)keep;
        if (R_FAILED(svcControlMemory(&addr, keep, 0, end - keep, MEMOP_FREE, 0))) fake_heap_end = (char *)end;
    }
    if ((u32)fake_heap_end > g_heapBaseEnd) {
        Line l;
        lnStart(&l);
        lnStr(&l, "heap still extended by ");
        lnInt(&l, (s64)((u32)fake_heap_end - g_heapBaseEnd));
        lnStr(&l, " bytes after the round");
        lnWrite(&l);
    }
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
        u32 got = fileRead(path, offset, g_chunk, CHUNK_SIZE);
        if (got == 0) break;
        u32 len = got;
        if (got == CHUNK_SIZE) {
            while (len > 0 && g_chunk[len - 1] != '\n') len--;
            if (len == 0) len = got; // a single over-long line: send it anyway, the server skips it
        }
        if (!post(g_chunk, len, fcHeader)) return false;
        offset += len;
    }
    FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, path));
    return true;
}

static bool online(void)
{
    u32 st = 0;
    bool ok = false;
    if (R_SUCCEEDED(acInit())) {
        ok = R_SUCCEEDED(ACU_GetStatus(&st));
        acExit();
    }
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

// The current friend code as the server wants it ("0" for none).
static void fcHeaderOf(char out[13], u64 fc)
{
    char d[13];
    fcDigits(d, fc);
    const char *p = d;
    while (*p == '0' && p[1]) p++;
    strcpy(out, p);
}

// Shared by every upload round: online check and http:C (with its POST buffer).
static bool uploadReady(void)
{
    if (!sdOpen()) return false;
    if (!online()) {
        Line l;
        lnStart(&l);
        lnStr(&l, "upload round skipped: offline (ac status ");
        lnHex(&l, g_status.acStatus);
        lnStr(&l, ")");
        lnWrite(&l);
        return false;
    }
    if (!httpReady) {
        Result hrc = httpcInit(g_postBufferSize);
        g_status.httpInit = (u32)hrc;
        httpReady = R_SUCCEEDED(hrc);
        if (!httpReady) {
            Line l;
            lnStart(&l);
            lnStr(&l, "upload round skipped: httpcInit ");
            lnHex(&l, (u32)hrc);
            lnWrite(&l);
        }
    }
    return httpReady;
}

// CSV logs: pending ".up" files first, then the current ".csv" files (renamed to ".up" when no older one
// is still waiting for that friend code), all in the same round.
static bool uploadLogs(void)
{
    bool uploaded = false;
    static char names[16][13];
    static bool ups[16];
    int count = 0;
    Handle dir;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, sdmc, fsMakePath(PATH_ASCII, DIR_PATH)))) return false;
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

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < count; i++) {
            if (ups[i] != (pass == 0)) continue;
            char csv[64], up[64];
            u32 a = 0, b = 0;
            putStr(csv, &a, DIR_PATH "/"); putStr(csv, &a, names[i]); putStr(csv, &a, ".csv"); csv[a] = 0;
            putStr(up, &b, DIR_PATH "/");  putStr(up, &b, names[i]);  putStr(up, &b, ".up");   up[b] = 0;

            if (!ups[i]) {
                // an older ".up" for this friend code is still waiting: never overwrite it
                if (fileSize(up) >= 0) continue;
                if (R_FAILED(FSUSER_RenameFile(sdmc, fsMakePath(PATH_ASCII, csv), sdmc, fsMakePath(PATH_ASCII, up)))) continue;
            }
            const char *fc = names[i];
            while (*fc == '0' && fc[1]) fc++;
            bool done = uploadFile(up, fc);
            Line l;
            lnStart(&l);
            lnStr(&l, done ? "upload ok: " : "upload failed, kept for next time: ");
            lnStr(&l, names[i]);
            lnStr(&l, ".up");
            lnWrite(&l);
            if (!done) return uploaded; // offline again or server trouble: retry next round
            uploaded = true;
        }
    }
    return uploaded;
}

// ---------------------------------------------------------------- dump uploads (crash dumps, memory dumps)

#define DUMP_URL        "https://rvta.nicoch.net/v1/dump"
#define DUMP_DIR        DIR_PATH "/dumps"
#define CRASH_DONE      DIR_PATH "/crash-uploaded.txt"

static u64 parseNumberAfter(const char *s, const char *prefix, bool *ok)
{
    size_t n = strlen(prefix);
    *ok = strncmp(s, prefix, n) == 0;
    u64 v = 0;
    const char *p = s + n;
    if (*ok && (*p < '0' || *p > '9')) *ok = false;
    while (*p >= '0' && *p <= '9') v = v * 10 + (u64)(*p++ - '0');
    return v;
}

// One chunk. Returns the number of bytes the server has stored, or -1.
static s64 postDump(const char *kind, const char *name, u64 offset, u64 total, const u8 *data, u32 len)
{
    httpcContext ctx;
    if (R_FAILED(httpcOpenContext(&ctx, HTTPC_METHOD_POST, DUMP_URL, 1))) return -1;
    char fc[13], off[24], tot[24];
    u32 a = 0, b = 0;
    fcHeaderOf(fc, slow.friendCode);
    putInt(off, &a, (s64)offset); off[a] = 0;
    putInt(tot, &b, (s64)total); tot[b] = 0;
    httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_DISABLED);
    httpcAddRequestHeaderField(&ctx, "User-Agent", "RvtA/" RVTA_VERSION);
    httpcAddRequestHeaderField(&ctx, "Content-Type", "application/octet-stream");
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Id", consoleId);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Version", RVTA_VERSION);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-FC", fc);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Dump-Kind", kind);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Dump-Name", name);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Dump-Offset", off);
    httpcAddRequestHeaderField(&ctx, "X-RvtA-Dump-Total", tot);

    s64 have = -1;
    u32 status = 0;
    Result rc = httpcAddPostDataRaw(&ctx, (u32 *)data, len);
    if (R_SUCCEEDED(rc)) rc = httpcBeginRequest(&ctx);
    if (R_SUCCEEDED(rc) && R_SUCCEEDED(httpcGetResponseStatusCode(&ctx, &status)) && (status == 200 || status == 409)) {
        char resp[32] = {0};
        u32 got = 0;
        httpcDownloadData(&ctx, (u8 *)resp, sizeof resp - 1, &got);
        bool ok;
        u64 v = parseNumberAfter(resp, status == 200 ? "ok " : "size ", &ok);
        if (ok) have = (s64)v;
    }
    httpcCloseContext(&ctx);
    if (have < 0) {
        Line l;
        lnStart(&l);
        lnStr(&l, "dump chunk failed: ");
        lnStr(&l, name);
        lnStr(&l, " at ");
        lnInt(&l, (s64)offset);
        lnStr(&l, " rc=");
        lnHex(&l, (u32)rc);
        lnStr(&l, " http=");
        lnInt(&l, status);
        lnWrite(&l);
        if (status >= 400 && status < 500 && status != 409) signalProblem(PROBLEM_REJECTED);
    }
    return have;
}

// Uploads a whole file, continuing from what the server already has. True once the server has all of it.
static bool uploadDumpFile(const char *path, const char *kind, const char *name)
{
    s64 total = fileSize(path);
    if (total <= 0) return false;
    u64 offset = 0;
    int stalls = 0;
    while (offset < (u64)total) {
        u32 n = (u32)((u64)total - offset < g_dumpChunkSize ? (u64)total - offset : g_dumpChunkSize);
        if (fileRead(path, offset, g_dumpBuf, n) != n) return false;
        s64 have = postDump(kind, name, offset, (u64)total, g_dumpBuf, n);
        if (have < 0) return false;
        if ((u64)have <= offset && ++stalls > 2) return false;
        offset = (u64)have;
    }
    return true;
}

static void asciiName(char *out, u32 max, const u16 *name)
{
    u32 i = 0;
    for (; i + 1 < max && name[i]; i++) out[i] = name[i] < 0x80 ? (char)name[i] : '_';
    out[i] = 0;
}

static bool endsWith(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

// Newest Luma crash dump in each folder (Luma numbers them, so the largest name is the newest), once.
static void uploadCrashDumps(void)
{
    static const char *const dirs[2] = {"arm11", "arm9"};
    static char done[4096];
    memset(done, 0, sizeof done);
    fileRead(CRASH_DONE, 0, done, sizeof done - 1);
    for (int d = 0; d < 2; d++) {
        char path[48];
        u32 p = 0;
        putStr(path, &p, "/luma/dumps/"); putStr(path, &p, dirs[d]); path[p] = 0;
        Handle dir;
        if (R_FAILED(FSUSER_OpenDirectory(&dir, sdmc, fsMakePath(PATH_ASCII, path)))) continue;
        char newest[64] = {0};
        FS_DirectoryEntry entry;
        u32 n = 0;
        while (R_SUCCEEDED(FSDIR_Read(dir, &n, 1, &entry)) && n == 1) {
            char nm[64];
            asciiName(nm, sizeof nm, entry.name);
            if (!(entry.attributes & FS_ATTRIBUTE_DIRECTORY) && endsWith(nm, ".dmp") && strcmp(nm, newest) > 0) strcpy(newest, nm);
        }
        FSDIR_Close(dir);
        if (!newest[0]) continue;

        char name[80], file[128];
        u32 a = 0, b = 0;
        putStr(name, &a, dirs[d]); putStr(name, &a, "-"); putStr(name, &a, newest); name[a] = 0;
        putStr(file, &b, path); putStr(file, &b, "/"); putStr(file, &b, newest); file[b] = 0;
        char line[84];
        u32 c = 0;
        putStr(line, &c, name); line[c++] = '\n'; line[c] = 0;
        if (strstr(done, line)) continue; // already uploaded

        bool ok = uploadDumpFile(file, "crash", name);
        Line l;
        lnStart(&l);
        lnStr(&l, ok ? "crash dump uploaded: " : "crash dump upload failed, retry next round: ");
        lnStr(&l, file);
        lnWrite(&l);
        if (ok) {
            fileAppend(CRASH_DONE, line, c);
            signalUploaded();
        }
    }
}

// Memory dumps saved by the button combo; deleted once the server has all of them.
static void uploadMemoryDumps(void)
{
    static char names[8][64];
    int count = 0;
    Handle dir;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, sdmc, fsMakePath(PATH_ASCII, DUMP_DIR)))) return;
    FS_DirectoryEntry entry;
    u32 n = 0;
    while (count < 8 && R_SUCCEEDED(FSDIR_Read(dir, &n, 1, &entry)) && n == 1) {
        asciiName(names[count], sizeof names[count], entry.name);
        if (!(entry.attributes & FS_ATTRIBUTE_DIRECTORY) && endsWith(names[count], ".bin")) count++;
    }
    FSDIR_Close(dir);
    for (int i = 0; i < count; i++) {
        char file[128];
        u32 b = 0;
        putStr(file, &b, DUMP_DIR "/"); putStr(file, &b, names[i]); file[b] = 0;
        bool ok = uploadDumpFile(file, "memory", names[i]);
        Line l;
        lnStart(&l);
        lnStr(&l, ok ? "memory dump uploaded: " : "memory dump upload paused, continues next round: ");
        lnStr(&l, names[i]);
        lnWrite(&l);
        if (!ok) return;
        FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, file));
        signalUploaded();
    }
}

// ---------------------------------------------------------------- upload thread

// A saved memory dump waiting for upload (the round then asks for big chunks).
static bool memoryDumpWaiting(void)
{
    Handle dir;
    if (!sdOpen() || R_FAILED(FSUSER_OpenDirectory(&dir, sdmc, fsMakePath(PATH_ASCII, DUMP_DIR)))) return false;
    bool found = false;
    FS_DirectoryEntry entry;
    u32 n = 0;
    while (!found && R_SUCCEEDED(FSDIR_Read(dir, &n, 1, &entry)) && n == 1) {
        char nm[64];
        asciiName(nm, sizeof nm, entry.name);
        found = !(entry.attributes & FS_ATTRIBUTE_DIRECTORY) && endsWith(nm, ".bin");
    }
    FSDIR_Close(dir);
    return found;
}

static Handle g_uploadEvent; // signalled to start a round early (a new memory dump is ready)

static void uploaderMain(void *arg)
{
    (void)arg;
    svcWaitSynchronization(g_uploadEvent, (s64)FIRST_UPLOAD_DELAY * 1000 * 1000 * 1000);
    for (;;) {
        g_status.uploadRounds++;
        if (online() && roundBegin(memoryDumpWaiting())) {
            if (uploadReady()) {
                if (uploadLogs()) signalUploaded();
                uploadCrashDumps();
                uploadMemoryDumps();
            }
            roundEnd();
        } else if (g_status.acStatus != 3) {
            Line l;
            lnStart(&l);
            lnStr(&l, "upload round skipped: offline (ac status ");
            lnHex(&l, g_status.acStatus);
            lnStr(&l, ")");
            lnWrite(&l);
        }
        svcWaitSynchronization(g_uploadEvent, (s64)UPLOAD_SECONDS * 1000 * 1000 * 1000);
    }
}

// ---------------------------------------------------------------- memory dump (L + START + SELECT)

#define DUMP_COMBO   (KEY_L | KEY_START | KEY_SELECT) // not R: L+R opens the camera on the HOME Menu
#define DUMP_WINDOW  0x12000000 // where target memory is mapped in, 1 MB at a time
#define DUMP_STEP    0x100000
#define DUMP_MAX     (150u << 20)
#define DUMP_REGIONS 64
#define PROBLEM_DUMP 4
#define DUMP_COOLDOWN 120 // seconds after a dump during which the combo is ignored (a double press is not two dumps)

static void ledShow(u8 r, u8 g, u8 b, int litSteps, bool loop)
{
    static InfoLedPattern p;
    memset(&p, 0, sizeof p);
    p.delay = 0x02;     // 1/8 s per step
    p.loopDelay = loop ? 0x00 : 0xFF;
    for (int i = 0; i < 32; i++) {
        bool on = loop || i < litSteps;
        p.redPattern[i] = on ? r : 0;
        p.greenPattern[i] = on ? g : 0;
        p.bluePattern[i] = on ? b : 0;
    }
    if (mcuOpen()) {
        MCUHWC_SetInfoLedPattern(&p);
        mcuClose();
    }
}

// Luma svc 0xA0 / 0xA1 (see Rosalina's csvc.s): r0 = 0xFFFFFFF2 selects the current argument order,
// the destination process handle goes in r6.
static Result mapProcessMemoryEx(Handle dst, u32 dstAddr, Handle src, u32 srcAddr, u32 size)
{
    register u32 r0 asm("r0") = 0xFFFFFFF2;
    register u32 r1 asm("r1") = dstAddr;
    register u32 r2 asm("r2") = src;
    register u32 r3 asm("r3") = srcAddr;
    register u32 r4 asm("r4") = size;
    register u32 r5 asm("r5") = 0; // flags: map as shared
    register u32 r6 asm("r6") = dst;
    asm volatile("svc 0xA0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : "r"(r4), "r"(r5), "r"(r6) : "r12", "lr", "memory");
    return (Result)r0;
}

static Result unmapProcessMemoryEx(Handle process, u32 addr, u32 size)
{
    register u32 r0 asm("r0") = process;
    register u32 r1 asm("r1") = addr;
    register u32 r2 asm("r2") = size;
    asm volatile("svc 0xA1" : "+r"(r0), "+r"(r1), "+r"(r2) : : "r3", "r12", "lr", "memory");
    return (Result)r0;
}

// Luma's pm:dbg command 0x100: program id and pid of the running application.
static bool currentApp(u64 *tid, u32 *pid)
{
    Handle h;
    if (R_FAILED(srvGetServiceHandle(&h, "pm:dbg"))) return false;
    u32 *cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x100, 0, 0);
    bool ok = R_SUCCEEDED(svcSendSyncRequest(h)) && R_SUCCEEDED((Result)cmdbuf[1]);
    if (ok) {
        memcpy(tid, &cmdbuf[2], sizeof *tid);
        *pid = cmdbuf[6];
    }
    svcCloseHandle(h);
    return ok;
}

typedef struct {
    u32 addr, size, perm, state;
} DumpRegion;

typedef struct {
    char magic[8]; // "RVTADUMP"
    u32 version;   // 1
    u32 regionCount;
    u64 titleId;
    u32 pid;
    u32 reserved;
    s64 time;      // console clock
} DumpHeader;      // followed by DumpRegion[regionCount], then each region's bytes in order

static u8 zeroPage[0x1000];

// Like Rosalina's OpenProcessByName: Luma's svcGetProcessInfo 0x10000 = process name, 0x10001 = title id.
static bool openProcessByName(const char *name, Handle *out, u64 *tid, u32 *pid)
{
    static u32 pids[0x40];
    s32 count = 0;
    if (R_FAILED(svcGetProcessList(&count, pids, 0x40))) return false;
    for (s32 i = 0; i < count; i++) {
        Handle h;
        if (R_FAILED(svcOpenProcess(&h, pids[i]))) continue;
        char procName[8] = {0};
        svcGetProcessInfo((s64 *)procName, h, 0x10000);
        if (strncmp(procName, name, 8) == 0) {
            s64 t = 0;
            svcGetProcessInfo(&t, h, 0x10001);
            *out = h;
            *tid = (u64)t;
            *pid = pids[i];
            return true;
        }
        svcCloseHandle(h);
    }
    return false;
}

static const char *dumpApp(void)
{
    u64 tid = 0;
    u32 pid = 0;
    Handle proc;
    if (currentApp(&tid, &pid)) {
        if (R_FAILED(svcOpenProcess(&proc, pid))) return "could not open the application process";
    } else if (openProcessByName("menu", &proc, &tid, &pid)) {
        // no application running: dump the HOME Menu instead
        Line l;
        lnStart(&l);
        lnStr(&l, "no application running, dumping the HOME Menu");
        lnWrite(&l);
    } else {
        return "no application and no HOME Menu process found";
    }

    static DumpRegion regions[DUMP_REGIONS];
    u32 count = 0, total = 0;
    for (u32 addr = 0; addr < 0x40000000 && count < DUMP_REGIONS;) {
        MemInfo mi;
        PageInfo pi;
        if (R_FAILED(svcQueryProcessMemory(&mi, &pi, proc, addr))) break;
        u32 next = mi.base_addr + mi.size;
        bool wanted = mi.state != MEMSTATE_FREE && mi.state != MEMSTATE_RESERVED && mi.state != MEMSTATE_IO &&
                      mi.state != MEMSTATE_STATIC && (mi.perm & MEMPERM_READ) && mi.size > 0;
        if (wanted && total + mi.size <= DUMP_MAX) {
            regions[count] = (DumpRegion){mi.base_addr, mi.size, mi.perm, mi.state};
            total += mi.size;
            count++;
        }
        if (next <= addr) break;
        addr = next;
    }

    char tmp[96], bin[96], hexTid[17];
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) hexTid[i] = hx[(tid >> (60 - 4 * i)) & 15];
    hexTid[16] = 0;
    u32 a = 0;
    putStr(tmp, &a, DUMP_DIR "/mem-"); putStr(tmp, &a, hexTid); putStr(tmp, &a, "-"); putInt(tmp, &a, unixNow());
    tmp[a] = 0;
    strcpy(bin, tmp);
    strcat(tmp, ".tmp");
    strcat(bin, ".bin");
    FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, DUMP_DIR), 0);

    Handle f;
    if (R_FAILED(FSUSER_OpenFile(&f, sdmc, fsMakePath(PATH_ASCII, tmp), FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        svcCloseHandle(proc);
        return "could not create the dump file";
    }
    DumpHeader h = {{'R', 'V', 'T', 'A', 'D', 'U', 'M', 'P'}, 1, count, tid, pid, 0, unixNow()};
    u32 w = 0;
    u64 off = 0;
    bool ok = R_SUCCEEDED(FSFILE_Write(f, &w, off, &h, sizeof h, 0)) && w == sizeof h;
    off += sizeof h;
    ok = ok && R_SUCCEEDED(FSFILE_Write(f, &w, off, regions, count * sizeof(DumpRegion), 0)) && w == count * sizeof(DumpRegion);
    off += count * sizeof(DumpRegion);

    u32 unreadable = 0;
    for (u32 r = 0; ok && r < count; r++) {
        for (u32 done = 0; ok && done < regions[r].size; done += DUMP_STEP) {
            u32 n = regions[r].size - done < DUMP_STEP ? regions[r].size - done : DUMP_STEP;
            if (R_SUCCEEDED(mapProcessMemoryEx(CUR_PROCESS_HANDLE, DUMP_WINDOW, proc, regions[r].addr + done, n))) {
                ok = R_SUCCEEDED(FSFILE_Write(f, &w, off, (void *)DUMP_WINDOW, n, 0)) && w == n;
                unmapProcessMemoryEx(CUR_PROCESS_HANDLE, DUMP_WINDOW, n);
            } else {
                // keep the layout: unreadable pages are written as zeros
                unreadable += n;
                for (u32 z = 0; ok && z < n; z += sizeof zeroPage)
                    ok = R_SUCCEEDED(FSFILE_Write(f, &w, off + z, zeroPage, sizeof zeroPage, 0)) && w == sizeof zeroPage;
            }
            off += n;
        }
    }
    FSFILE_Close(f);
    svcCloseHandle(proc);
    if (!ok) {
        FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, tmp));
        return "writing the dump failed (SD card full?)";
    }
    if (R_FAILED(FSUSER_RenameFile(sdmc, fsMakePath(PATH_ASCII, tmp), sdmc, fsMakePath(PATH_ASCII, bin)))) {
        FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, tmp));
        return "could not finish the dump file";
    }

    Line l;
    lnStart(&l);
    lnStr(&l, "memory dump saved: ");
    lnStr(&l, bin);
    lnStr(&l, " (");
    lnInt(&l, count);
    lnStr(&l, " regions, ");
    lnInt(&l, total);
    lnStr(&l, " bytes, ");
    lnInt(&l, unreadable);
    lnStr(&l, " unreadable)");
    lnWrite(&l);
    return NULL;
}

// The buttons straight from the HID register (mapped through the exheader's IORegisterMapping, active low,
// same bit order as KEY_*), as Rosalina does: no hid:USER session, which games and applets need.
#define HID_PAD_REG ((vu32 *)0x1EC46000)

static u32 padHeld(void)
{
    return (*HID_PAD_REG ^ 0xFFF) & 0xFFF;
}

static void inputMain(void *arg)
{
    (void)arg;
    bool wasHeld = false;
    s64 lastDump = -DUMP_COOLDOWN;
    for (;;) {
        svcSleepThread(100LL * 1000 * 1000);
        bool held = (padHeld() & DUMP_COMBO) == DUMP_COMBO;
        s64 now = unixNow();
        if (held && !wasHeld && now - lastDump < DUMP_COOLDOWN) {
            Line l;
            lnStart(&l);
            lnStr(&l, "dump combo ignored (cooldown, ");
            lnInt(&l, DUMP_COOLDOWN - (now - lastDump));
            lnStr(&l, " s left)");
            lnWrite(&l);
            ledShow(0xFF, 0x60, 0, 2, false); // short orange: wait a moment
        } else if (held && !wasHeld) {
            Line l;
            lnStart(&l);
            lnStr(&l, "dump combo pressed");
            lnWrite(&l);
            ledShow(0xFF, 0xFF, 0xFF, 0, true); // solid white while saving
            const char *err = dumpApp();
            lastDump = unixNow(); // counted from the end of saving, success or not
            if (err) {
                lnStart(&l);
                lnStr(&l, "memory dump failed: ");
                lnStr(&l, err);
                lnWrite(&l);
                // 4 purple pulses, not rate-limited: the player is waiting for an answer
                static InfoLedPattern p;
                memset(&p, 0, sizeof p);
                p.delay = 0x02;
                p.loopDelay = 0xFF;
                for (int i = 0; i < 32; i++) {
                    bool on = (i % 8) < 4;
                    p.redPattern[i] = on ? 0x80 : 0;
                    p.bluePattern[i] = on ? 0xFF : 0;
                }
                if (mcuOpen()) {
                    MCUHWC_SetInfoLedPattern(&p);
                    mcuClose();
                }
            } else {
                ledShow(0, 0xFF, 0, 2, false); // short green: saved
                svcSignalEvent(g_uploadEvent);  // upload it now if online
            }
        }
        wasHeld = held;
    }
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
    LightLock_Init(&g_logLock);
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

    // Uploads (logs and dumps) and the dump button run in their own threads, so a long dump upload never
    // delays the samples. Both need the heap (thread stacks, http:C's POST buffer).
    Result hrc = ensureHeap();
    {
        Line l;
        lnStart(&l);
        lnStr(&l, "heap ");
        if (R_FAILED(hrc)) {
            lnStr(&l, "allocation failed ");
            lnHex(&l, (u32)hrc);
        } else {
            lnInt(&l, (s64)(fake_heap_end - fake_heap_start));
            lnStr(&l, " bytes (uploads add their memory only while they run)");
        }
        lnWrite(&l);
    }
    if (R_SUCCEEDED(hrc)) {
        svcCreateEvent(&g_uploadEvent, RESET_ONESHOT);
        Thread up = threadCreate(uploaderMain, NULL, 0x2000, 0x3C, -2, true);
        Thread in = threadCreate(inputMain, NULL, 0x2000, 0x3C, -2, true);
        if (!up || !in) {
            Line l;
            lnStart(&l);
            lnStr(&l, up ? "input thread could not be started" : "upload thread could not be started");
            lnWrite(&l);
        }
    }

    s64 lastTick = cur.time, lastSample = cur.time, lastSlow = cur.time;

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
    }
    return 0;
}
