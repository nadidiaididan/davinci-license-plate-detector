// Monitor window: no-op on platforms without a native implementation (Linux).
#include "Monitor.h"
#if !defined(__APPLE__) && !defined(_WIN32)
namespace pmui {
void monitorShow(const std::string&) {}
void monitorUpdate(std::shared_ptr<std::vector<uint8_t>>, int, int, const std::string&, double) {}
void monitorClose() {}
bool monitorCancelled() { return false; }
bool monitorMainThreadResponsive() { return true; }
}  // namespace pmui
#endif
