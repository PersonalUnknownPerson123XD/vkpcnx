#ifdef __SWITCH__
#include <switch.h>
#include "core/saltynx/ipc.h"
#include "core/saltynx/saltynx_api.h"

Handle saltysd_orig;

Result vkpcnx_SaltySD_Connect(void) {
    for (int i = 0; i < 200; i++) {
        if (!svcConnectToNamedPort(&saltysd_orig, "SaltySD"))
            return 0;
        svcSleepThread(1000 * 1000);
    }
    return 1;
}

Result vkpcnx_SaltySD_Term(void) {
    Result ret;
    IpcCommand c;
    ipcInitialize(&c);
    ipcSendPid(&c);

    struct input {
        u64 magic;
        u64 cmd_id;
        u64 zero;
        u64 reserved[2];
    } *raw;

    raw = (struct input *)ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 0;
    raw->zero = 0;

    ret = ipcDispatch(saltysd_orig);
    if (R_SUCCEEDED(ret)) {
        IpcParsedCommand r;
        ipcParse(&r);
        struct output {
            u64 magic;
            u64 result;
        } *resp = (struct output *)r.Raw;
        ret = resp->result;
    }
    svcCloseHandle(saltysd_orig);
    if (ret == 0xf601)
        return 0;
    return ret;
}

Result vkpcnx_SaltySD_CheckIfSharedMemoryAvailable(ptrdiff_t *offset, u64 size) {
    Result ret = 0;
    IpcCommand c;
    ipcInitialize(&c);
    ipcSendPid(&c);

    struct input {
        u64 magic;
        u64 cmd_id;
        u64 size;
        u32 reserved[2];
    } *raw;

    raw = (struct input *)ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 6;
    raw->size = size;

    ret = ipcDispatch(saltysd_orig);
    if (R_SUCCEEDED(ret)) {
        IpcParsedCommand r;
        ipcParse(&r);
        struct output {
            u64 magic;
            u64 result;
            u64 offset;
        } *resp = (struct output *)r.Raw;
        ret = resp->result;
        if (!ret)
            *offset = (ptrdiff_t)resp->offset;
    }
    return ret;
}

Result vkpcnx_SaltySD_GetSharedMemoryHandle(Handle *retrieve) {
    Result ret = 0;
    if (retrieve)
        *retrieve = 0;
    IpcCommand c;
    ipcInitialize(&c);
    ipcSendPid(&c);

    struct input {
        u64 magic;
        u64 cmd_id;
        u32 reserved[4];
    } *raw;

    raw = (struct input *)ipcPrepareHeader(&c, sizeof(*raw));
    raw->magic = SFCI_MAGIC;
    raw->cmd_id = 7;

    ret = ipcDispatch(saltysd_orig);
    if (R_SUCCEEDED(ret)) {
        IpcParsedCommand r;
        ipcParse(&r);
        struct output {
            u64 magic;
            u64 result;
            u64 reserved[2];
        } *resp = (struct output *)r.Raw;
        /* Official client checks resp->result, but a copied handle in the
         * response is enough: some replies have a usable handle even if the
         * raw result word is unread / unaligned. */
        if (r.NumHandles > 0 && r.Handles[0])
            *retrieve = r.Handles[0];
        if (resp && resp->result)
            ret = (Result)resp->result;
        if (*retrieve)
            ret = 0;
    }
    return ret;
}

Result vkpcnx_SaltySD_ConnectEx(int tries, Result *first, Result *last, int *attempts) {
    Result rc = 0;
    if (first) *first = 0;
    if (last) *last = 0;
    int i;
    for (i = 0; i < tries; i++) {
        rc = svcConnectToNamedPort(&saltysd_orig, "SaltySD");
        if (i == 0 && first) *first = rc;
        if (last) *last = rc;
        if (R_SUCCEEDED(rc)) {
            if (attempts) *attempts = i + 1;
            return 0;
        }
        svcSleepThread(1000 * 1000);
    }
    if (attempts) *attempts = tries;
    return rc ? rc : 1;
}

bool vkpcnx_probe_named_port(const char *name, int tries, Result *first, Result *last) {
    Handle h = 0;
    Result rc = 0;
    if (first) *first = 0;
    for (int i = 0; i < tries; i++) {
        rc = svcConnectToNamedPort(&h, name);
        if (i == 0 && first) *first = rc;
        if (last) *last = rc;
        if (R_SUCCEEDED(rc)) {
            svcCloseHandle(h);
            return true;
        }
        svcSleepThread(1000 * 1000);
    }
    return false;
}

Result vkpcnx_session_usage(s64 *cur, s64 *limit, s64 *peak) {
    *cur = *limit = *peak = -1;
    Handle rl = 0;
    u64 info = 0;
    Result rc = svcGetInfo(&info, InfoType_ResourceLimit, INVALID_HANDLE, 0);
    if (R_SUCCEEDED(rc)) {
        rl = (Handle)info;
    } else {
        return rc;
    }
    Result first = 0, r;
    r = svcGetResourceLimitCurrentValue(cur, rl, LimitableResource_Sessions);
    if (R_FAILED(r) && !first) first = r;
    r = svcGetResourceLimitLimitValue(limit, rl, LimitableResource_Sessions);
    if (R_FAILED(r) && !first) first = r;
    r = svcGetResourceLimitPeakValue(peak, rl, LimitableResource_Sessions);
    if (R_FAILED(r) && !first) first = r;
    svcCloseHandle(rl);
    return first;
}

static VkpcnxSaltyEarly g_early = { .magicOffset = -1, .allocOffset = -1 };
static Thread g_earlyThread;
static u64 g_earlyBlockSize = 174;

static void early_main(void *arg) {
    (void)arg;
    VkpcnxSaltyEarly *e = &g_early;
    e->sessRc = vkpcnx_session_usage(&e->sessCur, &e->sessLimit, &e->sessPeak);

    // Up to ~4 s: SaltySD's loop is 10 ms per turn and can be busy hijacking
    // our own freshly created process for a moment.
    Result rc = vkpcnx_SaltySD_ConnectEx(4000, &e->connectFirst, &e->connectLast, &e->attempts);
    if (R_SUCCEEDED(rc)) {
        e->handleRc = vkpcnx_SaltySD_GetSharedMemoryHandle(&e->handle);
        if (R_SUCCEEDED(e->handleRc) && e->handle) {
            shmemLoadRemote(&e->shmem, e->handle, 0x1000, Perm_Rw);
            e->mapRc = shmemMap(&e->shmem);
            if (R_SUCCEEDED(e->mapRc)) {
                e->mapped = 1;
                uintptr_t base = (uintptr_t)shmemGetAddr(&e->shmem);
                for (long off = 0; off + 4 <= 0x1000; off += 4) {
                    if (*(volatile uint32_t *)(base + off) == 0x465053) {
                        e->magicOffset = off;
                        break;
                    }
                }
                if (e->magicOffset < 0) {
                    ptrdiff_t off = -1;
                    e->availRc = vkpcnx_SaltySD_CheckIfSharedMemoryAvailable(&off, g_earlyBlockSize);
                    if (R_SUCCEEDED(e->availRc) && off >= 0 && (u64)off + g_earlyBlockSize <= 0x1000)
                        e->allocOffset = (long)off;
                }
            }
        }
        e->termRc = vkpcnx_SaltySD_Term();
    }

    // Informational: is the InjectServ port (Status Monitor's liveness check) there?
    e->injectAttempted = 1;
    e->injectOk = vkpcnx_probe_named_port("InjectServ", 200, &e->injectFirst, &e->injectLast);
    e->state = 2;
}

void vkpcnx_nxfps_early_start(u64 blockSize) {
    if (g_early.state != 0)
        return;
    g_early.state = 1;
    g_earlyBlockSize = blockSize;
    if (R_FAILED(threadCreate(&g_earlyThread, early_main, NULL, NULL, 0x8000, 0x2C, -2)) ||
        R_FAILED(threadStart(&g_earlyThread))) {
        g_early.state = 2;
        g_early.connectFirst = g_early.connectLast = 0xFFFF;
    }
}

VkpcnxSaltyEarly *vkpcnx_nxfps_early(void) { return &g_early; }
#endif
