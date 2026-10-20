// bcat:u / bcat:a / bcat:m / bcat:s (Background Content Asymmetric Synchronized Delivery and Transmission): the
// game's delivery cache, as Ryujinx (MIT) answers it: always empty, and a sync request finishes at once with nothing
// new. Interfaces as switchbrew documents them: IServiceCreator (0 CreateBcatService, 1 CreateDeliveryCacheStorageService,
// 2 ...WithApplicationId), IBcatService (10100 RequestSyncDeliveryCache -> IDeliveryCacheProgressService),
// IDeliveryCacheStorageService (0 CreateFileService, 1 CreateDirectoryService, 10 EnumerateDeliveryCacheDirectory),
// IDeliveryCacheFileService and IDeliveryCacheDirectoryService (0 Open, 1 Read, 2 GetSize / GetCount, 3 GetDigest).
// Nothing here goes online. (Super Mario Bros. 35 creates the service at start-up; Nextendo Network delivers its
// Special Battle event as a RomFS file instead, not through this cache.)
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

#define BCAT_NOT_FOUND MAKE_RESULT(122, 2)

enum { PROGRESS_DONE = 9 };  // DeliveryCacheProgressImpl status

typedef struct { uint32_t ev; KEvent *e; } Progress;

static void progress(Service *s, IpcReq *req, IpcResp *resp) {
    Progress *p = s->state;
    switch (req->cmd) {
    case 0:  // GetEvent: signalled, the sync has already ended
        if (!p->ev) {
            uint32_t w;
            p->e = kernel_create_event_pair(&w, &p->ev);
            pthread_mutex_lock(&g_klock);
            kernel_signal(p->e);
            pthread_mutex_unlock(&g_klock);
        }
        resp->copy_handles[resp->ncopy++] = p->ev;
        break;
    case 1: {  // GetImpl: DeliveryCacheProgressImpl (0x200): status Done, result success, nothing downloaded
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 8) {
            memset((void *)(uintptr_t)b->addr, 0, b->size);
            *(int32_t *)(uintptr_t)b->addr = PROGRESS_DONE;
        }
        break;
    }
    default: LOG("[bcat] IDeliveryCacheProgressService cmd %u (ok)\n", req->cmd);
    }
}

static void bcat_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 10100: case 10101:  // RequestSyncDeliveryCache(WithDirectoryName)
        ipc_return_object(resp, service_new("IDeliveryCacheProgressService", progress, calloc(1, sizeof(Progress))));
        break;
    default: LOG("[bcat] IBcatService cmd %u (ok)\n", req->cmd);  // cancel, push notification log, ...: accepted
    }
}

static void file_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->result = BCAT_NOT_FOUND; break;                  // Open(DirectoryName, FileName): no such file
    case 1: case 2: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // Read / GetSize: nothing
    case 3: memset(resp->data, 0, 0x10); resp_size(resp, 0x10); break;  // GetDigest
    default: LOG("[bcat] IDeliveryCacheFileService cmd %u (ok)\n", req->cmd);
    }
}

static void directory_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->result = BCAT_NOT_FOUND; break;                  // Open(DirectoryName): no such directory
    case 1: case 2: OUT(int32_t, 0) = 0; resp_size(resp, 4); break;  // Read / GetCount: no entries
    default: LOG("[bcat] IDeliveryCacheDirectoryService cmd %u (ok)\n", req->cmd);
    }
}

static void storage_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IDeliveryCacheFileService", file_service, NULL)); break;
    case 1: ipc_return_object(resp, service_new("IDeliveryCacheDirectoryService", directory_service, NULL)); break;
    case 10: OUT(int32_t, 0) = 0; resp_size(resp, 4); break;      // EnumerateDeliveryCacheDirectory: none
    default: LOG("[bcat] IDeliveryCacheStorageService cmd %u (ok)\n", req->cmd);
    }
}

static void creator(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: ipc_return_object(resp, service_new("IBcatService", bcat_service, NULL)); break;
    case 1: case 2: ipc_return_object(resp, service_new("IDeliveryCacheStorageService", storage_service, NULL)); break;
    default: LOG("[bcat] IServiceCreator cmd %u (ok)\n", req->cmd);
    }
}

SERVICE("bcat:u", bcat_u) { creator(s, req, resp); }
SERVICE("bcat:a", bcat_a) { creator(s, req, resp); }
SERVICE("bcat:m", bcat_m) { creator(s, req, resp); }
SERVICE("bcat:s", bcat_s) { creator(s, req, resp); }
