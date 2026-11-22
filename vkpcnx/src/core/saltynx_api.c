#ifdef __SWITCH__
#include <switch.h>
#include "core/saltynx/ipc.h"

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
#endif
