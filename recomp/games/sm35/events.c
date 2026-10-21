// Special Battle events from Nextendo Network. The game reads the current event from Dat/sp_battle.dat in its RomFS
// (a 1-byte placeholder there); Nintendo delivered it by BCAT. Nextendo publishes its own as a zip holding
// Dat/sp_battle.dat at GET https://nextendo.network/api/bcat/0100277011f1a000 (204: none), which their console app
// lays over the RomFS. Here: at start, while Nextendo is the backend, the zip is fetched (at most a few seconds) and
// its files under Dat/ are written to events/romfs/ in the data folder, which runtime/services/fs.c lays over the
// RomFS (game_romfs_overlay). Offline, the copy from the last start is used. A 204 removes it.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#define mkdir_one(p) mkdir(p, 0755)
#endif

#include "../../runtime/hle.h"
#include "../../runtime/net/http.h"
#include "../../runtime/net/netcfg.h"
#include "../../third_party/puff/puff.h"

#define EVENTS_URL "https://nextendo.network/api/bcat/0100277011f1a000"
#define EVENTS_ROOT "events/romfs"
#define MAX_FILE (1u << 20)

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

// a zip entry name we accept: under Dat/, plain characters, no ".." or empty parts
static bool name_ok(const char *s) {
    if (strncmp(s, "Dat/", 4) || strstr(s, "..") || strstr(s, "//") || s[strlen(s) - 1] == '/') return false;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("_-./", *p)))
            return false;
    return true;
}

static void mkdirs(const char *path) {  // every directory above the file
    char d[512];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir_one(d); *p = '/'; }
}

static bool write_file(const char *path, const uint8_t *data, size_t n) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mkdirs(path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }  // (Windows: rename does not replace)
    if (!ok) remove(tmp);
    return ok;
}

// the zip's files under Dat/ into EVENTS_ROOT; the number written, -1 when the zip is not one we can read
static int unzip(const uint8_t *z, size_t n) {
    if (n < 22) return -1;
    size_t e = n - 22;  // the end-of-central-directory record (a comment may follow it)
    while (rd32(z + e) != 0x06054b50) { if (e == 0 || n - e > 22 + 0xffff) return -1; e--; }
    uint32_t count = rd16(z + e + 10), cd = rd32(z + e + 16);
    int written = 0;
    for (uint32_t i = 0; i < count; i++) {
        if ((size_t)cd + 46 > n || rd32(z + cd) != 0x02014b50) return -1;
        uint32_t method = rd16(z + cd + 10), csize = rd32(z + cd + 20), usize = rd32(z + cd + 24);
        uint32_t nl = rd16(z + cd + 28), xl = rd16(z + cd + 30), cl = rd16(z + cd + 32), lo = rd32(z + cd + 42);
        if ((size_t)cd + 46 + nl > n || nl >= 256) return -1;
        char name[256];
        memcpy(name, z + cd + 46, nl), name[nl] = 0;
        cd += 46 + nl + xl + cl;
        if (name[nl - 1] == '/') continue;  // (a directory entry)
        if (!name_ok(name) || usize > MAX_FILE) { LOG("[events] skipped %s\n", name); continue; }
        if ((size_t)lo + 30 > n || rd32(z + lo) != 0x04034b50) return -1;
        size_t data = (size_t)lo + 30 + rd16(z + lo + 26) + rd16(z + lo + 28);
        if (data + csize > n) return -1;
        uint8_t *out = malloc(usize ? usize : 1);
        unsigned long dl = usize, sl = csize;
        bool ok = method == 0 ? (csize == usize && (memcpy(out, z + data, usize), true))
                : method == 8 && puff(out, &dl, z + data, &sl) == 0 && dl == usize;
        char path[512];
        snprintf(path, sizeof path, EVENTS_ROOT "/%s", name);
        if (ok && write_file(path, out, usize)) written++;
        else LOG("[events] %s: could not unpack\n", name);
        free(out);
    }
    return written;
}

static void fetch(void) {
    HttpResponse r;
    int status = http_request("GET", EVENTS_URL, NULL, NULL, 0, 4000, &r);
    if (status == 204) {
        remove(EVENTS_ROOT "/Dat/sp_battle.dat");
        LOG("[events] Nextendo: no Special Battle event now\n");
    } else if (status == 200 && r.body) {
        int n = unzip((const uint8_t *)r.body, r.len);
        LOG("[events] Nextendo: %s\n", n < 0 ? "the event bundle is not a zip we can read" : n ? "Special Battle event updated" : "nothing to use in the event bundle");
    } else {
        struct stat st;
        LOG("[events] Nextendo: no answer (%d %s); %s\n", status, status ? "" : r.error,
            stat(EVENTS_ROOT "/Dat/sp_battle.dat", &st) == 0 ? "using the last event" : "no event");
    }
    http_free(&r);
}

const char *game_romfs_overlay(void) {
    static int done;
    static bool have;
    if (!done) {
        done = 1;
        if (net_backend() == NET_BACKEND_NEXTENDO) fetch();
        struct stat st;
        have = !stat(EVENTS_ROOT, &st);
    }
    return have ? EVENTS_ROOT : NULL;
}
