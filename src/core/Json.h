// Minimal JSON value + parser + serialiser (enough for the track sidecar; no external deps).
#pragma once
#include <map>
#include <string>
#include <vector>
#include <memory>

namespace pm {

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object };
  Type type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::map<std::string, Json> obj;

  Json() = default;
  Json(bool v) : type(Bool), b(v) {}
  Json(int v) : type(Number), num(v) {}
  Json(double v) : type(Number), num(v) {}
  Json(const char* s) : type(String), str(s) {}
  Json(const std::string& s) : type(String), str(s) {}
  static Json array() { Json j; j.type = Array; return j; }
  static Json object() { Json j; j.type = Object; return j; }

  bool isObject() const { return type == Object; }
  bool isArray() const { return type == Array; }
  bool has(const std::string& k) const { return type == Object && obj.count(k); }
  const Json& operator[](const std::string& k) const { static Json null; auto it = obj.find(k); return it == obj.end() ? null : it->second; }
  Json& operator[](const std::string& k) { type = Object; return obj[k]; }
  double asNum(double def = 0) const { return type == Number ? num : def; }
  int asInt(int def = 0) const { return type == Number ? (int)num : def; }
  bool asBool(bool def = false) const { return type == Bool ? b : def; }
  std::string asStr(const std::string& def = "") const { return type == String ? str : def; }
  void push(const Json& v) { type = Array; arr.push_back(v); }

  std::string dump() const;
  static bool parse(const std::string& text, Json& out);
};

}  // namespace pm
