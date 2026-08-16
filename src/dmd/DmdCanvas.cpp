#include "DmdCanvas.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "DmdFont5x7.h"

namespace
{
constexpr int kGlyphGap = 1;

const uint8_t* GlyphRows(char character)
{
  auto code = static_cast<unsigned char>(character);
  if (code >= 'a' && code <= 'z')
  {
    code = static_cast<unsigned char>(code - 'a' + 'A');
  }
  if (code < kDmdFont5x7First || code > kDmdFont5x7Last)
  {
    code = kDmdFont5x7First;  // space
  }
  return kDmdFont5x7[code - kDmdFont5x7First];
}

uint8_t ClampLevel(uint8_t level) { return level > 15 ? 15 : level; }

uint8_t EffectiveScale(const DmdTextOptions& options) { return options.scale == 0 ? 1 : options.scale; }
}  // namespace

DmdCanvas::DmdCanvas(uint16_t width, uint16_t height)
    : m_width(width), m_height(height), m_pixels(static_cast<size_t>(width) * height, 0)
{
}

void DmdCanvas::SetColor(uint8_t r, uint8_t g, uint8_t b)
{
  m_r = r;
  m_g = g;
  m_b = b;
  MarkDirty();
}

void DmdCanvas::Clear(uint8_t level)
{
  std::fill(m_pixels.begin(), m_pixels.end(), ClampLevel(level));
  MarkDirty();
}

void DmdCanvas::Pixel(int x, int y, uint8_t level)
{
  if (x < 0 || y < 0 || x >= m_width || y >= m_height)
  {
    return;
  }
  m_pixels[static_cast<size_t>(y) * m_width + x] = ClampLevel(level);
  MarkDirty();
}

uint8_t DmdCanvas::GetPixel(int x, int y) const
{
  if (x < 0 || y < 0 || x >= m_width || y >= m_height)
  {
    return 0;
  }
  return m_pixels[static_cast<size_t>(y) * m_width + x];
}

void DmdCanvas::Line(int x1, int y1, int x2, int y2, uint8_t level)
{
  // Bresenham. Clipping happens per pixel rather than up front: the line is at
  // most a few hundred pixels and the arithmetic for clipped endpoints is a
  // reliable source of off-by-ones.
  int dx = std::abs(x2 - x1);
  int dy = -std::abs(y2 - y1);
  int sx = x1 < x2 ? 1 : -1;
  int sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;

  while (true)
  {
    Pixel(x1, y1, level);
    if (x1 == x2 && y1 == y2) break;
    const int e2 = 2 * err;
    if (e2 >= dy)
    {
      err += dy;
      x1 += sx;
    }
    if (e2 <= dx)
    {
      err += dx;
      y1 += sy;
    }
  }
}

void DmdCanvas::Rect(int x, int y, int w, int h, uint8_t level, bool filled)
{
  if (w <= 0 || h <= 0) return;

  if (filled)
  {
    for (int row = y; row < y + h; ++row)
    {
      for (int column = x; column < x + w; ++column)
      {
        Pixel(column, row, level);
      }
    }
    return;
  }

  Line(x, y, x + w - 1, y, level);
  Line(x, y + h - 1, x + w - 1, y + h - 1, level);
  Line(x, y, x, y + h - 1, level);
  Line(x + w - 1, y, x + w - 1, y + h - 1, level);
}

void DmdCanvas::Fill(int x, int y, int w, int h, uint8_t level) { Rect(x, y, w, h, level, true); }

std::string DmdCanvas::FormatNumber(int64_t value, const DmdTextOptions& options)
{
  const bool negative = value < 0;
  uint64_t magnitude = negative ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);

  std::string digits;
  do
  {
    digits.insert(digits.begin(), static_cast<char>('0' + (magnitude % 10)));
    magnitude /= 10;
  } while (magnitude != 0);

  if (options.digits > 0 && static_cast<int>(digits.size()) < options.digits)
  {
    digits.insert(digits.begin(), static_cast<size_t>(options.digits) - digits.size(), options.pad);
  }

  if (options.commas)
  {
    // Grouped from the right, skipping any padding that is not a digit so
    // "  1234" does not become "  1,234" with a comma in the padding.
    std::string grouped;
    int sinceGroup = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
    {
      if (sinceGroup == 3 && std::isdigit(static_cast<unsigned char>(*it)))
      {
        grouped.push_back(',');
        sinceGroup = 0;
      }
      grouped.push_back(*it);
      if (std::isdigit(static_cast<unsigned char>(*it)))
      {
        ++sinceGroup;
      }
    }
    digits.assign(grouped.rbegin(), grouped.rend());
  }

  if (negative)
  {
    digits.insert(digits.begin(), '-');
  }
  return digits;
}

int DmdCanvas::TextWidth(const std::string& text, const DmdTextOptions& options)
{
  if (text.empty()) return 0;
  const int scale = EffectiveScale(options);
  const int cell = kDmdFont5x7Width * scale;
  const int gap = kGlyphGap * scale + options.spacing;
  return static_cast<int>(text.size()) * cell + (static_cast<int>(text.size()) - 1) * gap;
}

int DmdCanvas::DrawGlyph(int x, int y, char character, const DmdTextOptions& options)
{
  const uint8_t* rows = GlyphRows(character);
  const int scale = EffectiveScale(options);
  const uint8_t level = ClampLevel(options.level);

  for (int row = 0; row < kDmdFont5x7Height; ++row)
  {
    const uint8_t bits = rows[row];
    for (int column = 0; column < kDmdFont5x7Width; ++column)
    {
      if ((bits & (1 << (kDmdFont5x7Width - 1 - column))) == 0)
      {
        continue;
      }
      if (scale == 1)
      {
        Pixel(x + column, y + row, level);
      }
      else
      {
        Fill(x + column * scale, y + row * scale, scale, scale, level);
      }
    }
  }

  return kDmdFont5x7Width * scale;
}

int DmdCanvas::Text(int x, int y, const std::string& text, const DmdTextOptions& options)
{
  if (text.empty()) return 0;

  const int width = TextWidth(text, options);
  int cursor = x;
  switch (options.align)
  {
    case DmdTextOptions::Align::Left: break;
    // Right alignment puts the LAST pixel column at x, so a right-aligned score
    // and a rect ending at the same x line up.
    case DmdTextOptions::Align::Right: cursor = x - width + 1; break;
    case DmdTextOptions::Align::Center: cursor = x - width / 2; break;
  }

  const int scale = EffectiveScale(options);
  const int gap = kGlyphGap * scale + options.spacing;
  for (size_t i = 0; i < text.size(); ++i)
  {
    cursor += DrawGlyph(cursor, y, text[i], options);
    if (i + 1 < text.size())
    {
      cursor += gap;
    }
  }

  MarkDirty();
  return width;
}

int DmdCanvas::Number(int x, int y, int64_t value, const DmdTextOptions& options)
{
  return Text(x, y, FormatNumber(value, options), options);
}

std::string DmdCanvas::Dump() const
{
  std::string out;
  out.reserve(m_pixels.size() + m_height);
  for (int y = 0; y < m_height; ++y)
  {
    for (int x = 0; x < m_width; ++x)
    {
      out.push_back(GetPixel(x, y) == 0 ? '.' : '#');
    }
    out.push_back('\n');
  }
  return out;
}
