#pragma once
// C wrappers around SaltyNX's named-port protocol (Status Monitor / NX-FPS).
#ifdef __SWITCH__
#include <switch.h>
#ifdef __cplusplus
extern "C" {
#endif
Result vkpcnx_SaltySD_Connect(void);
// Same, but reports the first and last svcConnectToNamedPort result.
Result vkpcnx_SaltySD_ConnectEx(int tries, Result *first, Result *last, int *attempts);
Result vkpcnx_SaltySD_Term(void);
Result vkpcnx_SaltySD_CheckIfSharedMemoryAvailable(ptrdiff_t *offset, u64 size);
Result vkpcnx_SaltySD_GetSharedMemoryHandle(Handle *retrieve);

// Probe a named port: connect + close, retrying. Returns true if it connected.
bool vkpcnx_probe_named_port(const char *name, int tries, Result *first, Result *last);

// Own process session usage (resource limit). rc is the first failing call.
Result vkpcnx_session_usage(s64 *cur, s64 *limit, s64 *peak);

// Early attach: runs on a helper thread started from userAppInit (before
// bsd/nifm/hid/... have used up the process' session budget) so the
// SaltySD handshake gets a session. Results are read later by nxfps::init.
typedef struct {
    volatile int state; // 0 not started, 1 running, 2 done
    s64 sessCur, sessLimit, sessPeak;
    Result sessRc;
    int attempts;
    Result connectFirst, connectLast;
    Result handleRc;
    Handle handle;
    Result mapRc;
    int mapped;
    SharedMemory shmem;
    long magicOffset; // -1 if no NX-FPS block found in the page
    Result availRc;
    long allocOffset; // -1 if not allocated
    Result termRc;
    int injectOk;
    int injectAttempted;
    Result injectFirst, injectLast;
} VkpcnxSaltyEarly;

void vkpcnx_nxfps_early_start(u64 blockSize);
VkpcnxSaltyEarly *vkpcnx_nxfps_early(void);
#ifdef __cplusplus
}
#endif
#endif
