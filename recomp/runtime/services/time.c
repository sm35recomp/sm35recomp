// time:u / time:a / time:s
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <time.h>

#include "../hle.h"
#include "../ipc.h"

static const uint8_t clock_uuid[16] = {0x4c, 0x41, 0x52, 0x65, 0x63, 0x6f, 0x6d, 0x70, 1, 2, 3, 4, 5, 6, 7, 8};

static int64_t steady_seconds(void) { return (int64_t)(rt_ticks() / 19200000ull); }
// RECOMP_TIME_OFFSET=<seconds> (test): the game's calendar clock that far from the host's (e.g. 86400: tomorrow)
static int64_t unix_now(void) {
    static int64_t off = -1;
    if (off == -1) { const char *e = getenv("RECOMP_TIME_OFFSET"); off = e && *e ? atoll(e) : 0; if (off == -1) off = 0; }
    return (int64_t)time(NULL) + off;
}

static void write_system_context(uint8_t *p) {  // SystemClockContext (0x20)
    int64_t steady = steady_seconds();
    *(int64_t *)p = unix_now() - steady;
    *(int64_t *)(p + 8) = steady;
    memcpy(p + 0x10, clock_uuid, 16);
}

static void time_shmem_map(KSharedMem *s) {
    uint8_t *m = (uint8_t *)(uintptr_t)s->guest_addr;
    LOG("[time] shared memory mapped at %llx\n", (unsigned long long)s->guest_addr);
    // 0x00: LockFreeAtomicType<SteadyClockContext{u64 internal_offset, uuid}>
    *(uint32_t *)m = 0;
    *(uint64_t *)(m + 0x08) = 0;
    memcpy(m + 0x10, clock_uuid, 16);
    // 0x38 local system clock, 0x80 network system clock
    *(uint32_t *)(m + 0x38) = 0;
    write_system_context(m + 0x40);
    *(uint32_t *)(m + 0x80) = 0;
    write_system_context(m + 0x88);
    // 0xC8 automatic correction enabled
    *(uint32_t *)(m + 0xC8) = 0;
    m[0xCC] = 0;
}

static void system_clock(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(int64_t, 0) = unix_now(); resp_size(resp, 8); break;  // GetCurrentTime
    case 2: write_system_context(resp->data); resp_size(resp, 0x20); break;  // GetSystemClockContext
    default: LOG("[time] ISystemClock cmd %u (ok)\n", req->cmd);
    }
}

static void steady_clock(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:  // GetCurrentTimePoint
        OUT(int64_t, 0) = steady_seconds();
        memcpy(resp->data + 8, clock_uuid, 16);
        resp_size(resp, 0x18);
        break;
    default: OUT(int64_t, 0) = 0; resp_size(resp, 8); LOG("[time] ISteadyClock cmd %u (ok)\n", req->cmd);
    }
}

// CalendarTime (8: year, month, day, hour, minute, second) at o, CalendarAdditionalInfo (0x18: day of week, day of
// year, zone name, DST, UTC offset) at a
static void calendar_into(int64_t t, int utc, uint8_t *o, uint8_t *a) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (utc) gmtime_r(&tt, &tm); else localtime_r(&tt, &tm);
    *(uint16_t *)o = (uint16_t)(tm.tm_year + 1900);
    o[2] = (uint8_t)(tm.tm_mon + 1); o[3] = (uint8_t)tm.tm_mday;
    o[4] = (uint8_t)tm.tm_hour; o[5] = (uint8_t)tm.tm_min; o[6] = (uint8_t)tm.tm_sec; o[7] = 0;
    *(uint32_t *)a = (uint32_t)tm.tm_wday;
    *(uint32_t *)(a + 4) = (uint32_t)tm.tm_yday;
#ifdef _WIN32  // no tm_zone / tm_gmtoff: offset = local broken-down time read back as UTC, minus now
    struct tm g;
    gmtime_r(&tt, &g);
    long gmtoff = (long)difftime(_mkgmtime(&tm), _mkgmtime(&g));
    memset(a + 8, 0, 8);
    memcpy(a + 8, utc ? "UTC" : "LCL", 4);
#else
    long gmtoff = tm.tm_gmtoff;
    memset(a + 8, 0, 8);
    strncpy((char *)a + 8, utc ? "UTC" : tm.tm_zone ? tm.tm_zone : "LCL", 7);
#endif
    *(uint32_t *)(a + 16) = (uint32_t)tm.tm_isdst;
    *(int32_t *)(a + 20) = utc ? 0 : (int32_t)gmtoff;
}

static void to_calendar(int64_t t, IpcResp *resp, int utc) {
    calendar_into(t, utc, resp->data, resp->data + 8);
    resp_size(resp, 0x20);
}

// ClockSnapshot (0xD0), as switchbrew lays it out: the user and network SystemClockContexts (0x00, 0x20), their posix
// times (0x40, 0x48), calendar times (0x50, 0x58), calendar additional infos (0x60, 0x78), the steady clock time point
// (0x90), the location name (0xA8), automatic correction (0xCC) and the type (0xCD). Both clocks are the host's, in
// its own time zone. (Super Mario Bros. 35 takes "today" for its Daily Challenges from this; zeroed, every day was
// the same day and the challenges never changed.)
static void clock_snapshot(IpcReq *req, uint8_t type) {
    IpcBuf *b = out_buf(req, 0);
    if (!b || b->size < 0xD0) return;
    uint8_t *o = (uint8_t *)(uintptr_t)b->addr;
    memset(o, 0, 0xD0);
    write_system_context(o);
    write_system_context(o + 0x20);
    int64_t now = unix_now();
    *(int64_t *)(o + 0x40) = now;
    *(int64_t *)(o + 0x48) = now;
    calendar_into(now, 0, o + 0x50, o + 0x60);
    calendar_into(now, 0, o + 0x58, o + 0x78);
    *(int64_t *)(o + 0x90) = steady_seconds();
    memcpy(o + 0x98, clock_uuid, 16);
    memcpy(o + 0xA8, "UTC", 4);
    o[0xCC] = 0;
    o[0xCD] = type;
}

static void timezone_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: memcpy(resp->data, "UTC", 4); resp_size(resp, 0x24); break;  // GetDeviceLocationName
    case 100: to_calendar(IN(int64_t, 0), resp, 1); break;  // ToCalendarTime(rule buffer)
    case 101: to_calendar(IN(int64_t, 0), resp, 0); break;  // ToCalendarTimeWithMyRule
    case 201: {  // ToPosixTimeWithMyRule -> count + buffer of posix times
        const uint8_t *in = req->data;
        struct tm tm = {0};
        tm.tm_year = *(uint16_t *)in - 1900; tm.tm_mon = in[2] - 1; tm.tm_mday = in[3];
        tm.tm_hour = in[4]; tm.tm_min = in[5]; tm.tm_sec = in[6]; tm.tm_isdst = -1;
        int64_t t = mktime(&tm);
        if (req->nc) *(int64_t *)(uintptr_t)req->c[0].addr = t;
        else if (req->nb) *(int64_t *)(uintptr_t)req->b[0].addr = t;
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    default: LOG("[time] ITimeZoneService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static uint32_t g_shmem_handle;
RT_ON_RESET(time_reset) { g_shmem_handle = 0; }

static void static_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 1: case 4: case 5:
        ipc_return_object(resp, service_new("ISystemClock", system_clock, NULL));
        break;
    case 2: ipc_return_object(resp, service_new("ISteadyClock", steady_clock, NULL)); break;
    case 3: ipc_return_object(resp, service_new("ITimeZoneService", timezone_service, NULL)); break;
    case 20: {  // GetSharedMemoryNativeHandle
        if (!g_shmem_handle) kernel_create_shared_memory(0x1000, time_shmem_map, &g_shmem_handle);
        resp->copy_handles[resp->ncopy++] = g_shmem_handle;
        break;
    }
    case 100: case 101: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 400: case 401: clock_snapshot(req, IN(uint8_t, 0)); break;  // GetClockSnapshot(FromSystemClockContext)
    default: LOG("[time] IStaticService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

SERVICE("time:u", time_u) { static_service(s, req, resp); }
SERVICE("time:a", time_a) { static_service(s, req, resp); }
SERVICE("time:s", time_s) { static_service(s, req, resp); }

// nn::time::StandardUserSystemClock::GetCurrentTime(PosixTime*): answered directly from the host clock. The SDK's
// shared-memory path aborts (svcBreak via nn::diag::Abort, result 0xcc74) if the local clock's source id ever
// differs from the steady clock's; that happened once in the room sweep on an autosave (entering Eagle's Tower) and
// could not be reproduced, so the date stamp no longer depends on it.
HLE_FUNC("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", time_std_user_now) {
    static int n;
    if (n++ < 3) LOG("[time] StandardUserSystemClock::GetCurrentTime (host clock)\n");
    if (c->x[0]) *(int64_t *)(uintptr_t)c->x[0] = unix_now();
    c->x[0] = 0;  // ResultSuccess
}
