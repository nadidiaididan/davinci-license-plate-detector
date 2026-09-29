// Per-effect-instance plate tracks, JSON serialisation, sidecar file persistence.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <climits>
#include "Tracker.h"

namespace pm {

constexpr int kMaxPlates = 8;

struct PlateTrack {
  bool hasSeed = false;
  Pt seed;                 // canonical
  int seedFrame = INT_MIN;
  std::map<int, Key> keys; // frame -> key
  std::vector<float> templ;
  bool live = false;       // live (incremental) tracking armed: extend the track on every render
  bool fwdDone = false, bwdDone = false;

  bool empty() const { return keys.empty(); }
  int firstFrame() const { return keys.empty() ? INT_MIN : keys.begin()->first; }
  int lastFrame() const { return keys.empty() ? INT_MIN : keys.rbegin()->first; }
  // Interpolated key at `frame`. hold: extend the first/last key outside the tracked range.
  bool keyAt(int frame, bool hold, Key& out) const;
  void clearAuto();        // remove non-anchor keys
};

struct Store {
  std::vector<PlateTrack> plates{kMaxPlates};
  int detectHandled = 0, trackHandled = 0, clearHandled = 0, stopHandled = 0;
  int version = 1;
  std::string status;   // last operation summary, shown in the inspector

  std::string toJson() const;
  bool fromJson(const std::string& s);
};

std::string sidecarDir();                                   // creates the directory
std::string sidecarPath(const std::string& instanceId);
bool saveStore(const std::string& path, const Store& s);    // atomic write
bool loadStore(const std::string& path, Store& s);
long long fileMTime(const std::string& path);               // 0 if missing
std::string randomId();

}  // namespace pm
