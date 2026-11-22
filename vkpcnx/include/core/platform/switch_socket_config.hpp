#pragma once

#ifdef __SWITCH__

namespace vkpcnx::platform {

// Logs socketInitialize's result (captured in the wrap, before the logger
// existed) and nifmGetInternetConnectionStatus. Prefix: NET:
void logNetworkStartup();

} // namespace vkpcnx::platform

#endif
