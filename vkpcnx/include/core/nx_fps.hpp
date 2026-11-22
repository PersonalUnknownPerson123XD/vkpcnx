#pragma once
// Export presented FPS in the NX-FPS / SaltyNX shared-memory format that
// Status Monitor (Tesla overlay) reads. Official NX-FPS hooks
// nvnQueuePresentTexture / eglSwapBuffers; this process links Mesa EGL
// statically, so those hooks never see a swap. Writing the block ourselves
// is what makes the system FPS counter work, when SaltyNX is installed and
// this process is an Application (HOME shortcut / title override). Applet
// launches from hbmenu are invisible to pmdmntGetApplicationProcessId.
namespace vkpcnx::nxfps {
void attachEarly(); // SaltySD handshake; call once, early in main(), single-threaded
void init();
void tick(); // call once per presented frame / main-loop iteration
void shutdown();
} // namespace vkpcnx::nxfps
