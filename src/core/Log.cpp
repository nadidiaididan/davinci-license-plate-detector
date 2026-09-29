#include "Log.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

namespace pm {

static std::string logPath() {
#ifdef _WIN32
  const char* ad = getenv("APPDATA");
  return std::string(ad ? ad : ".") + "\\PlateMask.log";
#elif defined(__APPLE__)
  const char* home = getenv("HOME");
  return std::string(home ? home : "/tmp") + "/Library/Logs/PlateMask.log";
#else
  const char* home = getenv("HOME");
  return std::string(home ? home : "/tmp") + "/.platemask.log";
#endif
}

bool logVerbose() {
  static int enabled = -1;
  static int counter = 0;
  if (enabled < 0 || (++counter & 63) == 0) {
    struct stat st;
    std::string marker = logPath();
    marker = marker.substr(0, marker.size() - 4) + ".verbose";
    enabled = (getenv("PLATEMASK_LOG") != nullptr || stat(marker.c_str(), &st) == 0) ? 1 : 0;
  }
  return enabled == 1;
}

static void vlogf(const char* fmt, va_list ap) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lk(mu);
  FILE* f = fopen(logPath().c_str(), "a");
  if (!f) return;
  time_t t = time(nullptr);
  char ts[32];
  strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
  fprintf(f, "%s ", ts);
  vfprintf(f, fmt, ap);
  fputc('\n', f);
  fclose(f);
}

void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlogf(fmt, ap);
  va_end(ap);
}

void logv(const char* fmt, ...) {
  if (!logVerbose()) return;
  va_list ap;
  va_start(ap, fmt);
  vlogf(fmt, ap);
  va_end(ap);
}

}  // namespace pm
