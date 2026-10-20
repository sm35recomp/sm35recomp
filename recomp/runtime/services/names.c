// Names for the game's own network stack: nsd:u / nsd:a (the system's server-name settings: a host name template's
// '%' becomes the environment, "lp1") and sfdnsres (the resolver: GetAddrInfo, GetHostByName). Every name is
// answered from the redirect table (runtime/net/netcfg.h): the game's hosts go to the local NEX test server with
// backend local-nex, and every other name (and every name with the default backend) does not exist. The host's own
// DNS is never asked.
//
// sfdnsres answers are serialized as the system's resolver does [inf: switchbrew "Sfdnsres services", libnx's
// resolver (ISC)]: per address a big-endian {magic 0xBEEFCAFE, flags, family, socktype, protocol, addrlen}, the
// sockaddr_in (BSD layout), the canonical name (NUL-terminated); a zero word ends the list. A host entry is
// {name, u32 alias count + aliases, u16 family, u16 address length, u32 count + addresses}.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../net/netcfg.h"

enum { EAI_OK = 0, EAI_FAMILY_ = 5, EAI_NONAME_ = 8, EAI_SOCKTYPE_ = 10 };
enum { HOST_NOT_FOUND_ = 1 };

static void buf_str(IpcBuf *b, char *out, size_t cap) {
    out[0] = 0;
    if (!b || !b->size) return;
    size_t n = b->size < cap - 1 ? (size_t)b->size : cap - 1;
    memcpy(out, (const void *)(uintptr_t)b->addr, n);
    out[n] = 0;
}

static void buf_put_str(IpcBuf *b, const char *s) {
    if (!b || !b->size) return;
    memset((void *)(uintptr_t)b->addr, 0, b->size);
    size_t n = strlen(s);
    memcpy((void *)(uintptr_t)b->addr, s, n < b->size ? n : (size_t)b->size - 1);
}

// ---------------------------------------------------------------- nsd
static void nsd_resolve(const char *in, char *out, size_t cap) {
    size_t o = 0;
    const char *env = net_nsd_environment();
    for (const char *p = in; *p && o + 1 < cap; p++) {
        if (*p == '%') for (const char *e = env; *e && o + 1 < cap; e++) out[o++] = *e;
        else out[o++] = *p;
    }
    out[o] = 0;
}

static void nsd(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 10: case 11:  // GetSettingName / GetEnvironmentIdentifier
        buf_put_str(out_buf(req, 0), net_nsd_environment());
        return;
    case 12: {  // GetDeviceId: a fixed local id
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size) memset((void *)(uintptr_t)b->addr, '0', b->size < 16 ? b->size : 16);
        return;
    }
    case 20: case 21: {  // Resolve / ResolveEx(fqdn in) -> fqdn out (Ex: + an inner result, 0)
        char in[0x100], out[0x100];
        buf_str(in_buf(req, 0), in, sizeof in);
        nsd_resolve(in, out, sizeof out);
        buf_put_str(out_buf(req, 0), out);
        NLOG("[nsd] resolve \"%s\" -> \"%s\"\n", in, out);
        if (req->cmd == 21) OUT(uint32_t, 0) = 0, resp_size(resp, 4);
        return;
    }
    case 63: OUT(uint8_t, 0) = 0; resp_size(resp, 1); return;   // IsChangeEnvironmentIdentifierDisabled
    case 100: OUT(uint8_t, 0) = 0; resp_size(resp, 1); return;  // GetApplicationServerEnvironmentType
    default:
        LOG("[nsd] cmd %u (ok, empty)\n", req->cmd);
        for (int i = 0; i < req->nb || i < req->nc; i++) {
            IpcBuf *b = out_buf(req, i);
            if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);
        }
        resp_size(resp, 0x10);
    }
}

SERVICE("nsd:u", nsd_u) { nsd(s, req, resp); }
SERVICE("nsd:a", nsd_a) { nsd(s, req, resp); }

// ---------------------------------------------------------------- sfdnsres
typedef struct { uint8_t *p; size_t n, cap; } Out;
static void put8(Out *o, uint8_t v) { if (o->n < o->cap) o->p[o->n] = v; o->n++; }
static void put16(Out *o, uint16_t v) { put8(o, (uint8_t)(v >> 8)), put8(o, (uint8_t)v); }
static void put32(Out *o, uint32_t v) { put16(o, (uint16_t)(v >> 16)), put16(o, (uint16_t)v); }
static void putstr(Out *o, const char *s) { do put8(o, (uint8_t)*s); while (*s++); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint16_t service_port(const char *svc) {
    if (!svc[0]) return 0;
    char *end;
    long v = strtol(svc, &end, 10);
    if (!*end && v >= 0 && v < 65536) return (uint16_t)v;
    if (!strcmp(svc, "https")) return 443;
    if (!strcmp(svc, "http")) return 80;
    return 0;
}

// GetAddrInfo: (node, service, hints) -> the serialized list; returns the gai error and sets *size
static int addrinfo(IpcReq *req, uint32_t *size) {
    char node[256], svc[64];
    buf_str(in_buf(req, 0), node, sizeof node);
    buf_str(in_buf(req, 1), svc, sizeof svc);
    IpcBuf *h = in_buf(req, 2), *out = out_buf(req, 0);
    uint32_t family = 0, socktype = 0, proto = 0, flags = 0;
    if (h && h->size >= 24) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)h->addr;
        if (be32(p) == 0xBEEFCAFEu) flags = be32(p + 4), family = be32(p + 8), socktype = be32(p + 12), proto = be32(p + 16);
    }
    *size = 0;
    if (family != 0 && family != 2) return EAI_FAMILY_;
    if (socktype > 2) return EAI_SOCKTYPE_;
    uint32_t ip;
    bool found = node[0] ? net_resolve(node, &ip) : (ip = (flags & 1) ? 0 : 0x7f000001, true);  // (AI_PASSIVE: any)
    char a[16] = "-";
    if (found) ip_to_str(ip, a);
    LOG("[sfdnsres] getaddrinfo \"%s\" port \"%s\": %s\n", node, svc, found ? a : "no such name (not in the redirect table)");
    if (!found) return EAI_NONAME_;
    uint16_t port = service_port(svc);
    Out o = {out ? (uint8_t *)(uintptr_t)out->addr : NULL, 0, out && out->addr ? (size_t)out->size : 0};
    for (int t = 1; t <= 2; t++) {
        if (socktype && socktype != (uint32_t)t) continue;
        put32(&o, 0xBEEFCAFEu);
        put32(&o, 0);                                   // ai_flags
        put32(&o, 2);                                   // AF_INET
        put32(&o, (uint32_t)t);                         // SOCK_STREAM / SOCK_DGRAM
        put32(&o, proto ? proto : t == 1 ? 6 : 17);     // IPPROTO_TCP / UDP
        put32(&o, 16);                                  // ai_addrlen
        put8(&o, 16), put8(&o, 2), put16(&o, port), put32(&o, ip);
        for (int i = 0; i < 8; i++) put8(&o, 0);
        putstr(&o, node[0] ? node : "");
    }
    put32(&o, 0);
    *size = (uint32_t)(o.n <= o.cap ? o.n : o.cap);
    return EAI_OK;
}

static int hostbyname(IpcReq *req, uint32_t *size) {
    char node[256];
    buf_str(in_buf(req, 0), node, sizeof node);
    IpcBuf *out = out_buf(req, 0);
    uint32_t ip;
    *size = 0;
    bool found = net_resolve(node, &ip);
    char a[16] = "-";
    if (found) ip_to_str(ip, a);
    LOG("[sfdnsres] gethostbyname \"%s\": %s\n", node, found ? a : "no such name (not in the redirect table)");
    if (!found) return HOST_NOT_FOUND_;
    Out o = {out ? (uint8_t *)(uintptr_t)out->addr : NULL, 0, out ? (size_t)out->size : 0};
    putstr(&o, node);
    put32(&o, 0);           // aliases
    put16(&o, 2);           // AF_INET
    put16(&o, 4);           // address length
    put32(&o, 1);           // one address
    // [obs] the game's library reads each address as a big-endian u32 and stores it as in_addr's value (little-endian
    // memory): written as 0x7f000001 it connected to 1.0.0.127. So the bytes go in reverse.
    put32(&o, (ip >> 24) | (ip >> 8 & 0xff00) | (ip << 8 & 0xff0000) | (ip << 24));
    *size = (uint32_t)(o.n <= o.cap ? o.n : o.cap);
    return 0;
}

SERVICE("sfdnsres", sfdnsres) {
    (void)s;
    uint32_t size = 0;
    switch (req->cmd) {
    case 2: {  // GetHostByNameRequest -> {h_errno, errno, size}
        int e = hostbyname(req, &size);
        OUT(int32_t, 0) = e, OUT(int32_t, 4) = 0, OUT(uint32_t, 8) = size;
        resp_size(resp, 12);
        return;
    }
    case 10: {  // GetHostByNameRequestWithOptions -> {size, h_errno, errno}
        int e = hostbyname(req, &size);
        OUT(uint32_t, 0) = size, OUT(int32_t, 4) = e, OUT(int32_t, 8) = 0;
        resp_size(resp, 16);
        return;
    }
    case 6: {  // GetAddrInfoRequest -> {errno, gai error, size}
        int e = addrinfo(req, &size);
        OUT(int32_t, 0) = 0, OUT(int32_t, 4) = e, OUT(uint32_t, 8) = size;
        resp_size(resp, 12);
        return;
    }
    case 12: {  // GetAddrInfoRequestWithOptions -> {size, gai error, errno}
        int e = addrinfo(req, &size);
        OUT(uint32_t, 0) = size, OUT(int32_t, 4) = e, OUT(int32_t, 8) = 0;
        resp_size(resp, 16);
        return;
    }
    case 8: OUT(uint32_t, 0) = 1; resp_size(resp, 4); return;  // RequestCancelHandle
    case 9: return;                                            // CancelRequest
    default:
        LOG("[sfdnsres] cmd %u: not supported (no such name)\n", req->cmd);
        OUT(int32_t, 0) = 0, OUT(int32_t, 4) = HOST_NOT_FOUND_, OUT(int32_t, 8) = 0;
        resp_size(resp, 16);
    }
}
