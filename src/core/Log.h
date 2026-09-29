// Tiny append-only diagnostic logger (~/Library/Logs/PlateMask.log on macOS). Events are always
// logged (a few lines per user action); per-frame lines (logv) only when PLATEMASK_LOG=1 is set or
// a file named PlateMask.verbose exists next to the log (checked periodically, no restart needed).
#pragma once
#include <string>
namespace pm {
void logf(const char* fmt, ...);
void logv(const char* fmt, ...);
bool logVerbose();
}
