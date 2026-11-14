#include "json_lite.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ao4json {
namespace {

struct Parser {
  const std::string& t;
  size_t i = 0;
  std::string err;

  void Ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i; }
  bool Fail(const char* what) { if (err.empty()) err = std::string(what) + " at " + std::to_string(i); return false; }

  bool Str(std::string& out) {
    if (i >= t.size() || t[i] != '"') return Fail("string expected");
    ++i;
    while (i < t.size() && t[i] != '"') {
      char c = t[i++];
      if (c == '\\' && i < t.size()) {
        const char e = t[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (i + 4 > t.size()) return Fail("bad escape");
            const unsigned cp = unsigned(std::strtoul(t.substr(i, 4).c_str(), nullptr, 16));
            i += 4;
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
            else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
            break;
          }
          default: out += e;
        }
      } else {
        out += c;
      }
    }
    if (i >= t.size()) return Fail("unterminated string");
    ++i;
    return true;
  }

  bool Val(Value& v) {
    Ws();
    if (i >= t.size()) return Fail("value expected");
    const char c = t[i];
    if (c == '{') {
      v.type = Value::Type::Object;
      ++i;
      Ws();
      if (i < t.size() && t[i] == '}') { ++i; return true; }
      for (;;) {
        Ws();
        std::string key;
        if (!Str(key)) return false;
        Ws();
        if (i >= t.size() || t[i] != ':') return Fail("':' expected");
        ++i;
        Value child;
        if (!Val(child)) return false;
        v.obj.emplace_back(std::move(key), std::move(child));
        Ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == '}') { ++i; return true; }
        return Fail("',' or '}' expected");
      }
    }
    if (c == '[') {
      v.type = Value::Type::Array;
      ++i;
      Ws();
      if (i < t.size() && t[i] == ']') { ++i; return true; }
      for (;;) {
        Value child;
        if (!Val(child)) return false;
        v.arr.push_back(std::move(child));
        Ws();
        if (i < t.size() && t[i] == ',') { ++i; continue; }
        if (i < t.size() && t[i] == ']') { ++i; return true; }
        return Fail("',' or ']' expected");
      }
    }
    if (c == '"') { v.type = Value::Type::String; return Str(v.s); }
    if (t.compare(i, 4, "true") == 0) { v.type = Value::Type::Bool; v.b = true; i += 4; return true; }
    if (t.compare(i, 5, "false") == 0) { v.type = Value::Type::Bool; v.b = false; i += 5; return true; }
    if (t.compare(i, 4, "null") == 0) { v.type = Value::Type::Null; i += 4; return true; }
    char* end = nullptr;
    const double n = std::strtod(t.c_str() + i, &end);
    if (end == t.c_str() + i) return Fail("bad value");
    v.type = Value::Type::Number;
    v.n = n;
    i = size_t(end - t.c_str());
    return true;
  }
};

void Escape(const std::string& s, std::string& out) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  out += '"';
}

void Write(const Value& v, int indent, std::string& out) {
  const std::string pad(size_t(indent) * 2, ' ');
  switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.b ? "true" : "false"; break;
    case Value::Type::Number: {
      char buf[64];
      if (std::fabs(v.n - std::round(v.n)) < 1e-12 && std::fabs(v.n) < 1e15) std::snprintf(buf, sizeof(buf), "%.1f", v.n);
      else std::snprintf(buf, sizeof(buf), "%.17g", v.n);
      out += buf;
      break;
    }
    case Value::Type::String: Escape(v.s, out); break;
    case Value::Type::Array:
      out += "[";
      for (size_t k = 0; k < v.arr.size(); ++k) { if (k) out += ", "; Write(v.arr[k], indent + 1, out); }
      out += "]";
      break;
    case Value::Type::Object:
      out += "{\n";
      for (size_t k = 0; k < v.obj.size(); ++k) {
        out += pad + "  ";
        Escape(v.obj[k].first, out);
        out += ": ";
        Write(v.obj[k].second, indent + 1, out);
        out += k + 1 < v.obj.size() ? ",\n" : "\n";
      }
      out += pad + "}";
      break;
  }
}

}  // namespace

bool Parse(const std::string& text, Value& out, std::string* error) {
  Parser p{text};
  size_t start = 0;
  if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF) start = 3;  // BOM
  p.i = start;
  out = Value{};
  const bool ok = p.Val(out);
  if (!ok && error) *error = p.err;
  return ok;
}

std::string Serialize(const Value& v, int indent) {
  std::string out;
  Write(v, indent, out);
  out += "\n";
  return out;
}

}  // namespace ao4json
