#pragma once

// Host-only fake at the NVS seam. Never included by the firmware build.
#include <cassert>
#include <cstdint>
#include <map>
#include <string>

using String = std::string;
struct HostSerial {
  template <typename... Args> void printf(const char*, Args...) {}
};
inline HostSerial Serial;

class Preferences {
 public:
  inline static std::map<std::string, int64_t> numbers;
  inline static std::map<std::string, std::string> strings;
  inline static bool available = true;
  bool begin(const char* name, bool read_only) {
    prefix_ = std::string(name) + "/";
    read_only_ = read_only;
    opened_ = available;
    return opened_;
  }
  void end() { opened_ = false; }
  bool getBool(const char* key, bool fallback) { return number(key, fallback) != 0; }
  int getInt(const char* key, int fallback) { return static_cast<int>(number(key, fallback)); }
  uint16_t getUShort(const char* key, uint16_t fallback) { return static_cast<uint16_t>(number(key, fallback)); }
  uint32_t getULong(const char* key, uint32_t fallback) { return static_cast<uint32_t>(number(key, fallback)); }
  String getString(const char* key, const char* fallback) {
    assert(opened_);
    const auto found = strings.find(prefix_ + key);
    return found == strings.end() ? fallback : found->second;
  }
  size_t putBool(const char* key, bool value) { return put(key, value); }
  size_t putInt(const char* key, int value) { return put(key, value); }
  size_t putUShort(const char* key, uint16_t value) { return put(key, value); }
  size_t putULong(const char* key, uint32_t value) { return put(key, value); }
  size_t putString(const char* key, const char* value) {
    assert(opened_ && !read_only_);
    strings[prefix_ + key] = value;
    return strings[prefix_ + key].size();
  }
  bool clear() {
    assert(opened_ && !read_only_ && prefix_ == "display/");
    numbers.clear(); strings.clear(); return true;
  }
 private:
  int64_t number(const char* key, int64_t fallback) {
    assert(opened_);
    const auto found = numbers.find(prefix_ + key);
    return found == numbers.end() ? fallback : found->second;
  }
  size_t put(const char* key, int64_t value) {
    assert(opened_ && !read_only_);
    numbers[prefix_ + key] = value;
    return sizeof(value);
  }
  std::string prefix_;
  bool opened_ = false;
  bool read_only_ = true;
};
