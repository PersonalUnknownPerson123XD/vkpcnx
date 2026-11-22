#pragma once
// C wrappers around SaltyNX's named-port protocol (Status Monitor / NX-FPS).
#ifdef __SWITCH__
#include <switch.h>
#ifdef __cplusplus
extern "C" {
#endif
Result vkpcnx_SaltySD_Connect(void);
Result vkpcnx_SaltySD_Term(void);
Result vkpcnx_SaltySD_CheckIfSharedMemoryAvailable(ptrdiff_t *offset, u64 size);
Result vkpcnx_SaltySD_GetSharedMemoryHandle(Handle *retrieve);
#ifdef __cplusplus
}
#endif
#endif
