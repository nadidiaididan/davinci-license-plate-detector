#include "TrackStore.h"
#include "Json.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <random>
#include <sys/stat.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

namespace pm {

bool PlateTrack::keyAt(int frame, bool hold, Key& out) const {
  if (keys.empty()) return false;
  auto it = keys.find(frame);
  if (it != keys.end()) { out = it->second; return true; }
  auto hi = keys.lower_bound(frame);
  if (hi == keys.end()) {
    if (!hold) return false;
    out = keys.rbegin()->second;
    return true;
  }
  if (hi == keys.begin()) {
    if (!hold) return false;
    out = hi->second;
    return true;
  }
  auto lo = std::prev(hi);
  double t = (double)(frame - lo->first) / (hi->first - lo->first);
  out = lo->second;
  out.q = quadLerp(lo->second.q, hi->second.q, t);
  out.grow = lo->second.grow * (1 - t) + hi->second.grow * t;
  out.conf = std::min(lo->second.conf, hi->second.conf);
  if (!isEstimated(lo->second.st) || !isEstimated(hi->second.st)) out.st = VisState::Occluded;
  else if (!isVisible(lo->second.st) || !isVisible(hi->second.st)) out.st = VisState::Inferred;
  return true;
}

void PlateTrack::clearAuto() {
  for (auto it = keys.begin(); it != keys.end();)
    if (!isAnchor(it->second.st)) it = keys.erase(it); else ++it;
}

static Json quadToJson(const Quad& q) {
  Json a = Json::array();
  for (const Pt& p : q) { a.push(p.x); a.push(p.y); }
  return a;
}
static bool quadFromJson(const Json& j, Quad& q) {
  if (!j.isArray() || j.arr.size() != 8) return false;
  for (int i = 0; i < 4; ++i) q[i] = {j.arr[2 * i].asNum(), j.arr[2 * i + 1].asNum()};
  return true;
}

std::string Store::toJson() const {
  Json root = Json::object();
  root["version"] = version;
  root["detectHandled"] = detectHandled;
  root["trackHandled"] = trackHandled;
  root["clearHandled"] = clearHandled;
  root["status"] = status;
  root["stopHandled"] = stopHandled;
  Json ps = Json::array();
  for (const PlateTrack& p : plates) {
    Json pj = Json::object();
    pj["hasSeed"] = p.hasSeed;
    pj["live"] = p.live;
    pj["fwdDone"] = p.fwdDone;
    pj["bwdDone"] = p.bwdDone;
    pj["seed"] = Json::array(); pj["seed"].push(p.seed.x); pj["seed"].push(p.seed.y);
    pj["seedFrame"] = p.seedFrame == INT_MIN ? Json() : Json(p.seedFrame);
    Json ks = Json::array();
    for (auto& kv : p.keys) {
      Json k = Json::object();
      k["f"] = kv.first;
      k["q"] = quadToJson(kv.second.q);
      k["s"] = (int)kv.second.st;
      k["c"] = kv.second.conf;
      k["g"] = kv.second.grow;
      ks.push(k);
    }
    pj["keys"] = ks;
    Json t = Json::array();
    for (float v : p.templ) t.push((double)v);
    pj["templ"] = t;
    ps.push(pj);
  }
  root["plates"] = ps;
  return root.dump();
}

bool Store::fromJson(const std::string& s) {
  Json root;
  if (!Json::parse(s, root) || !root.isObject()) return false;
  Store out;
  out.version = root["version"].asInt(1);
  out.detectHandled = root["detectHandled"].asInt();
  out.trackHandled = root["trackHandled"].asInt();
  out.clearHandled = root["clearHandled"].asInt();
  out.status = root["status"].asStr();
  out.stopHandled = root["stopHandled"].asInt();
  const Json& ps = root["plates"];
  if (ps.isArray()) {
    for (size_t i = 0; i < ps.arr.size() && i < (size_t)kMaxPlates; ++i) {
      const Json& pj = ps.arr[i];
      PlateTrack& p = out.plates[i];
      p.hasSeed = pj["hasSeed"].asBool();
      p.live = pj["live"].asBool();
      p.fwdDone = pj["fwdDone"].asBool();
      p.bwdDone = pj["bwdDone"].asBool();
      if (pj["seed"].isArray() && pj["seed"].arr.size() == 2) p.seed = {pj["seed"].arr[0].asNum(), pj["seed"].arr[1].asNum()};
      p.seedFrame = pj["seedFrame"].type == Json::Number ? pj["seedFrame"].asInt() : INT_MIN;
      const Json& ks = pj["keys"];
      if (ks.isArray())
        for (const Json& k : ks.arr) {
          Key key;
          if (!quadFromJson(k["q"], key.q)) continue;
          key.st = (VisState)k["s"].asInt(1);
          key.conf = k["c"].asNum();
          key.grow = k["g"].asNum();
          p.keys[k["f"].asInt()] = key;
        }
      const Json& t = pj["templ"];
      if (t.isArray()) for (const Json& v : t.arr) p.templ.push_back((float)v.asNum());
    }
  }
  *this = std::move(out);
  return true;
}

// mkdir -p
static void makeDirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if ((path[i] == '/' || path[i] == '\\') && cur.size() > 1) MKDIR(cur.c_str());
  }
  MKDIR(path.c_str());
}

std::string sidecarDir() {
  std::string base;
#ifdef _WIN32
  const char* ad = getenv("APPDATA");
  base = ad ? std::string(ad) + "\\PlateMask" : "C:\\PlateMask";
#elif defined(__APPLE__)
  const char* home = getenv("HOME");
  base = std::string(home ? home : "/tmp") + "/Library/Application Support/PlateMask";
#else
  const char* home = getenv("HOME");
  base = std::string(home ? home : "/tmp") + "/.local/share/PlateMask";
#endif
  std::string dir = base + "/tracks";
  makeDirs(dir);
  return dir;
}

std::string sidecarPath(const std::string& instanceId) { return sidecarDir() + "/" + instanceId + ".json"; }

bool saveStore(const std::string& path, const Store& s) {
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << s.toJson();
    if (!f) return false;
  }
#ifdef _WIN32
  if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { std::remove(tmp.c_str()); return false; }
#else
  if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
#endif
  return true;
}

bool loadStore(const std::string& path, Store& s) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  return s.fromJson(ss.str());
}

long long fileMTime(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
#ifdef __APPLE__
  return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
  return (long long)st.st_mtime * 1000000000LL;
#else
  return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
}

std::string randomId() {
  std::random_device rd;
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (int i = 0; i < 32; ++i) s += hex[rd() % 16];
  return s;
}

}  // namespace pm
