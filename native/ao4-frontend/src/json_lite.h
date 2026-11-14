// Minimal JSON for the port's small config files (objects, strings, numbers, bools, null).
// Objects keep their key order so a read-modify-write leaves the file layout recognisable.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao4json {

struct Value {
  enum class Type { Null, Bool, Number, String, Object, Array } type = Type::Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<std::pair<std::string, Value>> obj;
  std::vector<Value> arr;

  const Value* Get(const std::string& key) const {
    for (const auto& [k, v] : obj) if (k == key) return &v;
    return nullptr;
  }
  Value& Set(const std::string& key, Value v) {
    for (auto& [k, x] : obj) if (k == key) { x = std::move(v); return x; }
    obj.emplace_back(key, std::move(v));
    return obj.back().second;
  }
  static Value Str(std::string v) { Value x; x.type = Type::String; x.s = std::move(v); return x; }
  static Value Num(double v) { Value x; x.type = Type::Number; x.n = v; return x; }
  static Value Bool(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
  static Value Obj() { Value x; x.type = Type::Object; return x; }
};

bool Parse(const std::string& text, Value& out, std::string* error = nullptr);
std::string Serialize(const Value& v, int indent = 0);

}  // namespace ao4json
