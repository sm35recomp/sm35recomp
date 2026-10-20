// ldn:u (local wireless communication). The runtime has no radio: the service exists so a game's local-play menus
// open (SMM2's Nearby Play crashed on a missing object), reports itself initialized and finds no networks; creating
// or joining a wireless network fails. A game's LAN mode (UDP on the virtual LAN, runtime/net/vlan.h and bsd.c) is
// the way two recomp players meet.
//
// IUserLocalCommunicationService commands: 0 GetState, 1 GetNetworkInfo, 2 GetIpv4Address, 3 GetDisconnectReason,
// 100 AttachStateChangeEvent, 101 GetNetworkInfoLatestUpdate, 102 Scan, 103 ScanPrivate, 200/201 Open/CloseAccessPoint,
// 202/203 CreateNetwork(Private), 204 DestroyNetwork, 206 SetAdvertiseData, 207 SetStationAcceptPolicy,
// 300/301 Open/CloseStation, 302/303 Connect(Private), 304 Disconnect, 400 Initialize, 401 Finalize, 402 Initialize2.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

#define LDN_RESULT_DISABLED MAKE_RESULT(203, 22)  // (module 203: ldn) "wireless off / unavailable" [inf]

enum { ST_NONE = 0, ST_INITIALIZED = 1, ST_AP = 2, ST_STATION = 4, ST_ERROR = 6 };

typedef struct { uint32_t state, ev; KEvent *e; } Ldn;

static void comm(Service *s, IpcReq *req, IpcResp *resp) {
    Ldn *l = s->state;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = l->state; resp_size(resp, 4); return;  // GetState
    case 1: case 101: {  // GetNetworkInfo(LatestUpdate): none
        for (int i = 0; i < 2; i++) {
            IpcBuf *b = out_buf(req, i);
            if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);
        }
        resp->result = LDN_RESULT_DISABLED;
        return;
    }
    case 2: OUT(uint32_t, 0) = 0, OUT(uint32_t, 4) = 0; resp_size(resp, 8); return;  // GetIpv4Address
    case 3: OUT(uint16_t, 0) = 0; resp_size(resp, 2); return;                       // GetDisconnectReason
    case 100: {  // AttachStateChangeEvent
        if (!l->ev) {
            uint32_t w;
            l->e = kernel_create_event_pair(&w, &l->ev);
        }
        resp->copy_handles[resp->ncopy++] = l->ev;
        return;
    }
    case 102: case 103: {  // Scan(Private): no networks
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size) memset((void *)(uintptr_t)b->addr, 0, b->size);
        OUT(uint16_t, 0) = 0;
        resp_size(resp, 4);
        return;
    }
    case 200: case 300: l->state = req->cmd == 200 ? ST_AP : ST_STATION; return;      // Open AP / station
    case 201: case 301: case 204: case 304: l->state = ST_INITIALIZED; return;         // close / destroy / disconnect
    case 202: case 203: case 302: case 303:                                             // create / connect: no radio
        LOG("[ldn] cmd %u: no local wireless here\n", req->cmd);
        resp->result = LDN_RESULT_DISABLED;
        return;
    case 400: case 402: l->state = ST_INITIALIZED; LOG("[ldn] initialized (no radio: networks never found)\n"); return;
    case 401: l->state = ST_NONE; return;
    default: LOG("[ldn] cmd %u (ok)\n", req->cmd); return;
    }
}

static void root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) {  // CreateUserLocalCommunicationService
        ipc_return_object(resp, service_new("ldn:IUserLocalCommunicationService", comm, calloc(1, sizeof(Ldn))));
        return;
    }
    LOG("[ldn] root cmd %u (ok)\n", req->cmd);
}

SERVICE("ldn:u", ldn_u) { root(s, req, resp); }
