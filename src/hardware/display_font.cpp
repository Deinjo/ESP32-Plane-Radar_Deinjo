#include "hardware/display_font.h"

#include "hardware/display.h"

extern "C" {
extern const uint8_t _binary_data_ui_font_vlw_start[] asm(
    "_binary_data_ui_font_vlw_start");
extern const uint8_t _binary_data_ui_font_vlw_end[] asm("_binary_data_ui_font_vlw_end");
}

namespace {

bool s_vlw_loaded = false;
// Both the panel and the framebuffer use the same immutable font. Keeping
// its owner outside LGFXBase avoids duplicate glyph tables and prevents
// setFont(Font0) from destroying/reallocating it on each animation frame.
lgfx::PointerWrapper s_vlw_data;
lgfx::VLWfont s_vlw_font;

const uint8_t* vlwData() { return _binary_data_ui_font_vlw_start; }

size_t vlwDataLen() {
  return static_cast<size_t>(_binary_data_ui_font_vlw_end -
                               _binary_data_ui_font_vlw_start);
}

bool vlwActiveOn(const lgfx::LGFXBase& gfx) {
  return gfx.getFont() == &s_vlw_font;
}

}  // namespace

bool displayFontInit() {
  s_vlw_data.set(vlwData());
  s_vlw_loaded = vlwDataLen() > 0 && s_vlw_font.loadFont(&s_vlw_data);
  if (s_vlw_loaded) {
    tft.setFont(&s_vlw_font);
  }
  if (!s_vlw_loaded) {
    Serial.println("Smooth font load failed — using bitmap fallback");
  }
  return s_vlw_loaded;
}

bool displayFontIsSmooth() { return s_vlw_loaded; }

bool displayFontEnsureLoaded(lgfx::LGFXBase& gfx) {
  if (!s_vlw_loaded) {
    return false;
  }
  if (vlwActiveOn(gfx)) {
    return true;
  }
  gfx.setFont(&s_vlw_font);
  return true;
}

void displayFontSetSmoothSize(lgfx::LGFXBase& gfx, float size) {
  displayFontEnsureLoaded(gfx);
  gfx.setTextSize(size);
}

void displayFontSetBitmap(lgfx::LGFXBase& gfx, const lgfx::GFXfont* font) {
  gfx.setFont(font);
  gfx.setTextSize(1);
}
