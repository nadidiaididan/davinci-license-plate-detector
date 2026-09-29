#include "Json.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace pm {

static void dumpStr(const std::string& s, std::string& out) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) { char buf[8]; snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += c;
    }
  }
  out += '"';
}

static void dumpInto(const Json& j, std::string& out) {
  switch (j.type) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Number: {
      char buf[64];
      if (std::isfinite(j.num) && j.num == std::floor(j.num) && std::fabs(j.num) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)j.num);
      else snprintf(buf, sizeof buf, "%.10g", std::isfinite(j.num) ? j.num : 0.0);
      out += buf;
      break;
    }
    case Json::String: dumpStr(j.str, out); break;
    case Json::Array:
      out += '[';
      for (size_t i = 0; i < j.arr.size(); ++i) { if (i) out += ','; dumpInto(j.arr[i], out); }
      out += ']';
      break;
    case Json::Object:
      out += '{';
      { bool first = true;
        for (auto& kv : j.obj) { if (!first) out += ','; first = false; dumpStr(kv.first, out); out += ':'; dumpInto(kv.second, out); } }
      out += '}';
      break;
  }
}

std::string Json::dump() const { std::string s; dumpInto(*this, s); return s; }

namespace {
struct Parser {
  const std::string& t;
  size_t i = 0;
  explicit Parser(const std::string& s) : t(s) {}
  void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i; }
  bool lit(const char* s) { size_t n = strlen(s); if (t.compare(i, n, s) == 0) { i += n; return true; } return false; }
  bool str(std::string& out) {
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    while (i < t.size()) {
      char c = t[i++];
      if (c == '"') return true;
      if (c == '\\') {
        if (i >= t.size()) return false;
        char e = t[i++];
        switch (e) {
          case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
          case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
          case 'r': out += '\r'; break; case 't': out += '\t'; break;
          case 'u': {
            if (i + 4 > t.size()) return false;
            unsigned cp = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16); i += 4;
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            break;
          }
          default: return false;
        }
      } else out += c;
    }
    return false;
  }
  bool value(Json& out, int depth = 0) {
    if (depth > 200) return false;
    ws();
    if (i >= t.size()) return false;
    char c = t[i];
    if (c == '{') {
      ++i; out = Json::object();
      ws();
      if (i < t.size() && t[i] == '}') { ++i; return true; }
      while (true) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (i >= t.size() || t[i] != ':') return false;
        ++i;
        Json v;
        if (!value(v, depth + 1)) return false;
        out.obj[k] = std::move(v);
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        return false;
      }
    }
    if (c == '[') {
      ++i; out = Json::array();
      ws();
      if (i < t.size() && t[i] == ']') { ++i; return true; }
      while (true) {
        Json v;
        if (!value(v, depth + 1)) return false;
        out.arr.push_back(std::move(v));
        ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        return false;
      }
    }
    if (c == '"') { out = Json(); out.type = Json::String; return str(out.str); }
    if (lit("true")) { out = Json(true); return true; }
    if (lit("false")) { out = Json(false); return true; }
    if (lit("null")) { out = Json(); return true; }
    const char* start = t.c_str() + i;
    char* end = nullptr;
    double d = strtod(start, &end);
    if (end == start) return false;
    i += (size_t)(end - start);
    out = Json(d);
    return true;
  }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out) {
  Parser p(text);
  if (!p.value(out)) return false;
  p.ws();
  return p.i == text.size();
}

}  // namespace pm
