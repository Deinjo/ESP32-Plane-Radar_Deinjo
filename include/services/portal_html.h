#pragma once

#include <cstddef>
#include <cstring>

namespace services::portal {

// The sink accepts (bytes, length) and returns false on a closed connection.
// Never assemble an entire settings page in the fragmented Wi-Fi/TLS heap.
template <typename Sink>
class HtmlWriter {
 public:
  explicit HtmlWriter(Sink& sink) : sink_(sink) {}

  void append(const char* text) {
    if (text == nullptr) {
      return;
    }
    while (*text != '\0' && good_) {
      buffer_[used_++] = *text++;
      if (used_ == sizeof(buffer_)) {
        flush();
      }
    }
  }

  void escaped(const char* text) {
    if (text == nullptr) {
      return;
    }
    while (*text != '\0' && good_) {
      switch (*text) {
        case '&': append("&amp;"); break;
        case '<': append("&lt;"); break;
        case '>': append("&gt;"); break;
        case '"': append("&quot;"); break;
        case '\'': append("&#39;"); break;
        default: {
          const char character[] = {*text, '\0'};
          append(character);
          break;
        }
      }
      ++text;
    }
  }

  bool flush() {
    if (good_ && used_ != 0) {
      good_ = sink_(buffer_, used_);
      used_ = 0;
    }
    return good_;
  }

  void jsonString(const char* text) {
    append("\"");
    if (text != nullptr) {
      while (*text != '\0' && good_) {
        const unsigned char value = static_cast<unsigned char>(*text++);
        if (value == '"') append("\\\"");
        else if (value == '\\') append("\\\\");
        else if (value < 32) {
          constexpr char hex[] = "0123456789abcdef";
          const char escape[] = {'\\', 'u', '0', '0', hex[value >> 4], hex[value & 15], '\0'};
          append(escape);
        } else {
          const char character[] = {static_cast<char>(value), '\0'};
          append(character);
        }
      }
    }
    append("\"");
  }

 private:
  Sink& sink_;
  char buffer_[512];
  size_t used_ = 0;
  bool good_ = true;
};

constexpr char kFormCompleteField[] = "_form_complete";
constexpr char kCommonFormVersion[] = "common-v2";
constexpr char kComponentFormVersion[] = "component-v1";

// Checkboxes are deliberately not required: unchecked controls are omitted
// by browsers. The final marker and all always-submitted controls are required
// before any settings/NVS writes. This is completeness checking, not CSRF auth.
template <typename HasField>
bool completeCommonForm(const char* marker, HasField has_field) {
  if (marker == nullptr || std::strcmp(marker, kCommonFormVersion) != 0) {
    return false;
  }
  const char* required[] = {"radar_lat", "radar_lon", "range_index", "text_scale",
                            "night_start", "night_end", "ota_password"};
  for (const char* name : required) {
    if (!has_field(name)) {
      return false;
    }
  }
  return true;
}

template <typename HasField>
bool completeComponentForm(const char* marker, HasField has_field) {
  if (marker == nullptr || std::strcmp(marker, kComponentFormVersion) != 0) {
    return false;
  }
  const char* required[] = {"color_bg", "color_grid", "color_label", "color_center",
      "color_aircraft", "color_track", "color_tag_type", "color_tag_alt",
      "color_runway", "color_runway_label", "color_footer", "color_road",
      "color_city", "color_road_primary"};
  for (const char* name : required) {
    if (!has_field(name)) {
      return false;
    }
  }
  return true;
}

}  // namespace services::portal
