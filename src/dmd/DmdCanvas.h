#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A DMD framebuffer: one byte per pixel, value 0..15, plus a whole-frame tint.
//
// 4-bit grey rather than RGB24 because every sink downstream wants indexed data:
// ZeDMD's native input is Mode::Data, ConsoleDMD::Render takes a buffer plus a
// bit depth, and libdmdutil converts Data to RGB24 with the tint for the SDL
// virtual DMD. RGB24 would triple the USB payload for a screen that is a single
// amber colour anyway.
//
// Drawing calls therefore take a level, never a colour. Colour is set once for
// the whole frame.
//
// Every primitive clips silently. A score that grows one digit wider than the
// layout expected must not crash a machine in a basement.

struct DmdTextOptions
{
  uint8_t level = 15;
  // Integer pixel scale. 1 gives the 5x7 cell; 2 gives 10x14, which is the
  // right size for a score on a 128x32 panel.
  uint8_t scale = 1;
  // Extra pixels between glyph cells, on top of the built-in one-pixel gap.
  int spacing = 0;
  // "left" | "center" | "right" -- x is the anchor, not always the left edge.
  enum class Align
  {
    Left,
    Center,
    Right
  } align = Align::Left;
  // Thousands separators, for scores.
  bool commas = false;
  // Pad to at least this many digits.
  int digits = 0;
  // Padding character when `digits` forces it: ' ' or '0'.
  char pad = ' ';
};

class DmdCanvas
{
 public:
  DmdCanvas(uint16_t width, uint16_t height);

  uint16_t Width() const { return m_width; }
  uint16_t Height() const { return m_height; }
  const uint8_t* Data() const { return m_pixels.data(); }
  size_t Size() const { return m_pixels.size(); }

  // True when anything has been drawn since the last ClearDirty(). The renderer
  // uses this to avoid pushing frames nobody changed.
  bool IsDirty() const { return m_dirty; }
  void ClearDirty() { m_dirty = false; }

  void SetColor(uint8_t r, uint8_t g, uint8_t b);
  uint8_t ColorR() const { return m_r; }
  uint8_t ColorG() const { return m_g; }
  uint8_t ColorB() const { return m_b; }

  void Clear(uint8_t level = 0);
  void Pixel(int x, int y, uint8_t level);
  uint8_t GetPixel(int x, int y) const;
  void Line(int x1, int y1, int x2, int y2, uint8_t level);
  void Rect(int x, int y, int w, int h, uint8_t level, bool filled = false);
  void Fill(int x, int y, int w, int h, uint8_t level);

  // Draws text and returns the width in pixels that was drawn. Lower-case input
  // is folded to upper case: the bundled font has no lower-case glyphs, and a
  // machine showing blanks would be worse than one showing capitals.
  int Text(int x, int y, const std::string& text, const DmdTextOptions& options = {});
  int Number(int x, int y, int64_t value, const DmdTextOptions& options = {});

  // Pixel width the same call would draw, without drawing it. Exists so a
  // script can right-align without knowing font metrics, and so layout can be
  // asserted in tests independently of rendering.
  static int TextWidth(const std::string& text, const DmdTextOptions& options = {});
  static std::string FormatNumber(int64_t value, const DmdTextOptions& options);

  // Renders the buffer as ASCII art: '.' for level 0, '#' otherwise. Used by
  // tests, where a failure that prints a picture is one you fix rather than
  // delete.
  std::string Dump() const;

 private:
  void MarkDirty() { m_dirty = true; }
  int DrawGlyph(int x, int y, char character, const DmdTextOptions& options);

  uint16_t m_width;
  uint16_t m_height;
  std::vector<uint8_t> m_pixels;
  bool m_dirty = true;
  uint8_t m_r = 255;
  uint8_t m_g = 120;
  uint8_t m_b = 0;
};
