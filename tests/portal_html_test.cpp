#include "services/portal_html.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace services::portal;

int main() {
  std::string output;
  std::vector<size_t> chunks;
  auto sink = [&output, &chunks](const char* bytes, size_t size) {
    assert(size > 0 && size <= 512);
    output.append(bytes, size);
    chunks.push_back(size);
    return true;
  };
  HtmlWriter<decltype(sink)> html(sink);
  const std::string large(17000, 'x'); // larger than the measured free block
  html.append(large.c_str());
  html.append(nullptr);
  html.escaped("&<>'\"");
  html.escaped(nullptr);
  assert(html.flush());
  assert(output == large + "&amp;&lt;&gt;&#39;&quot;");
  assert(chunks.size() > 30);
  const size_t chunk_count = chunks.size();
  assert(html.flush());
  assert(chunks.size() == chunk_count); // no accidental zero HTTP terminator

  output.clear();
  HtmlWriter<decltype(sink)> boundary(sink);
  const std::string edge(511, 'a');
  boundary.append(edge.c_str());
  boundary.escaped("&");
  assert(boundary.flush());
  assert(output == edge + "&amp;");

  output.clear();
  HtmlWriter<decltype(sink)> json(sink);
  json.jsonString("quote\" slash\\ tab\t line\n <&>\x01");
  json.append(",");
  json.jsonString(nullptr);
  assert(json.flush());
  assert(output == "\"quote\\\" slash\\\\ tab\\u0009 line\\u000a <&>\\u0001\",\"\"");

  int writes = 0;
  auto disconnected = [&writes](const char*, size_t) { ++writes; return false; };
  HtmlWriter<decltype(disconnected)> failed(disconnected);
  failed.append(large.c_str());
  failed.append("must not continue after a closed connection");
  assert(!failed.flush() && writes == 1);

  const auto all_fields = [](const char*) { return true; };
  assert(!completeCommonForm(nullptr, all_fields));
  assert(!completeCommonForm("", all_fields));
  assert(!completeCommonForm(kComponentFormVersion, all_fields));
  assert(!completeCommonForm("common-v1", all_fields)); // pre-toggle forms must reload
  assert(!completeComponentForm(kCommonFormVersion, all_fields));
  assert(completeCommonForm(kCommonFormVersion, all_fields));
  assert(completeComponentForm(kComponentFormVersion, all_fields));
  const char* common[] = {"radar_lat", "radar_lon", "range_index", "text_scale",
                          "night_start", "night_end", "ota_password"};
  for (const char* omitted : common) {
    assert(!completeCommonForm(kCommonFormVersion, [omitted](const char* name) {
      return std::strcmp(omitted, name) != 0;
    }));
  }
  const char* colors[] = {"color_bg", "color_grid", "color_label", "color_center",
      "color_aircraft", "color_track", "color_tag_type", "color_tag_alt",
      "color_runway", "color_runway_label", "color_footer", "color_road",
      "color_city", "color_road_primary"};
  for (const char* omitted : colors) {
    assert(!completeComponentForm(kComponentFormVersion, [omitted](const char* name) {
      return std::strcmp(omitted, name) != 0;
    }));
  }
  // Unchecked checkboxes are legitimately absent; only text/color controls
  // and the final marker are mandatory.
  assert(completeCommonForm(kCommonFormVersion, [](const char* name) {
    return std::strstr(name, "show_") == nullptr;
  }));
  assert(completeComponentForm(kComponentFormVersion, [](const char* name) {
    return std::strstr(name, "_visible") == nullptr;
  }));
  std::puts("PASS: bounded 512-byte HTML chunks, escaping across boundaries, disconnect handling,");
  std::puts("      complete-form markers, every required field, absent unchecked checkboxes");
}
