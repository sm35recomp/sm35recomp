// The file backend of the secret store (runtime/net/secure_store.h): net/nextendo.txt, one "key value" line per
// entry, written to a temporary file (mode 0600) and renamed over, so a crash never leaves half a file. On Apple the
// Keychain is the default; RECOMP_SECURE_STORE=file selects this one.
#include "secure_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define STORE_FILE "net/nextendo.txt"
#define MAX_ENTRIES 8
#define VAL_MAX 2048

typedef struct { char key[32]; char val[VAL_MAX]; } Entry;

static int read_all(Entry *e) {
    int n = 0;
    FILE *f = fopen(STORE_FILE, "r");
    if (!f) return 0;
    static char line[VAL_MAX + 48];
    while (n < MAX_ENTRIES && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *sp = strchr(line, ' ');
        if (line[0] == '#' || !sp || sp == line || (size_t)(sp - line) >= sizeof e[n].key) continue;
        *sp = 0;
        snprintf(e[n].key, sizeof e[n].key, "%s", line);
        snprintf(e[n].val, sizeof e[n].val, "%s", sp + 1);
        n++;
    }
    fclose(f);
    return n;
}

static bool write_all(const Entry *e, int n) {
    const char *tmp = STORE_FILE ".tmp";
#ifdef _WIN32
    _mkdir("net");
    FILE *f = fopen(tmp, "w");
#else
    mkdir("net", 0700);
    unlink(tmp);  // (a stale file keeps its old mode; a symlink is never followed)
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
    if (fd >= 0 && !f) close(fd);
#endif
    if (!f) return false;
    fputs("# Private: Nextendo sign-in. Never share this file.\n", f);
    for (int i = 0; i < n; i++) fprintf(f, "%s %s\n", e[i].key, e[i].val);
    bool ok = fclose(f) == 0;
#ifdef _WIN32
    if (ok) ok = MoveFileExA(tmp, STORE_FILE, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    if (ok) ok = rename(tmp, STORE_FILE) == 0;
#endif
    if (!ok) remove(tmp);
    return ok;
}

static bool file_get(const char *key, char *out, size_t cap) {
    static Entry e[MAX_ENTRIES];
    int n = read_all(e);
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].key, key) && strlen(e[i].val) < cap) { strcpy(out, e[i].val); return true; }
    return false;
}

static bool file_set(const char *key, const char *val) {
    static Entry e[MAX_ENTRIES];
    int n = read_all(e), at = -1;
    if (strlen(key) >= sizeof e[0].key || strlen(val) >= VAL_MAX || strpbrk(val, "\r\n")) return false;
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].key, key)) at = i;
    if (at < 0) {
        if (n == MAX_ENTRIES) return false;
        at = n++;
    }
    snprintf(e[at].key, sizeof e[at].key, "%s", key);
    snprintf(e[at].val, sizeof e[at].val, "%s", val);
    return write_all(e, n);
}

static void file_del(const char *key) {
    static Entry e[MAX_ENTRIES];
    int n = read_all(e), m = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(e[i].key, key)) e[m++] = e[i];
    if (m != n) write_all(e, m);
}

#ifdef __APPLE__
static bool use_file(void) {
    const char *v = getenv("RECOMP_SECURE_STORE");
    return v && !strcmp(v, "file");
}
bool secure_get(const char *key, char *out, size_t cap) { return use_file() ? file_get(key, out, cap) : secure_apple_get(key, out, cap); }
bool secure_set(const char *key, const char *val) { return use_file() ? file_set(key, val) : secure_apple_set(key, val); }
void secure_del(const char *key) { if (use_file()) file_del(key); else secure_apple_del(key); }
#else
bool secure_get(const char *key, char *out, size_t cap) { return file_get(key, out, cap); }
bool secure_set(const char *key, const char *val) { return file_set(key, val); }
void secure_del(const char *key) { file_del(key); }
#endif
