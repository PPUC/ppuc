#include "BurnEffect.h"

#include <algorithm>
#include <cmath>

namespace scoreboard
{

static int ScorchRadius(const Style& style, int fontSize)
{
  return std::max(1, static_cast<int>(std::lround(style.scorchReach * static_cast<float>(fontSize))));
}

static int ShadowRadius(const Style& style, int fontSize)
{
  return std::max(1, static_cast<int>(std::lround(style.shadowSoftness * static_cast<float>(fontSize))));
}

bool HasShadow(const Style& style) { return style.shadow > 0.0f; }

int ShadowOffset(const Style& style, int fontSize)
{
  return HasShadow(style) ? static_cast<int>(std::lround(style.shadowOffset * static_cast<float>(fontSize))) : 0;
}

// Room for the scorch, and for a shadow's blur when there is one: a glyph and
// its shadow share one size, so they line up by the same pad.
int BurnPad(const Style& style, int fontSize)
{
  const int scorch = ScorchRadius(style, fontSize) * 2 + 1;
  return HasShadow(style) ? std::max(scorch, ShadowRadius(style, fontSize) * 2 + 1) : scorch;
}

void BurnShadow(const uint8_t* coverage, int width, int height, const Style& style, int fontSize,
                std::vector<uint8_t>* rgba, int* outWidth, int* outHeight)
{
  const int pad = BurnPad(style, fontSize);
  const int w = width + pad * 2;
  const int h = height + pad * 2;
  *outWidth = w;
  *outHeight = h;
  rgba->assign(static_cast<size_t>(w) * h * 4, 0);
  if (width <= 0 || height <= 0 || !HasShadow(style)) return;
  std::vector<float> shape(static_cast<size_t>(w) * h, 0.0f);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      shape[static_cast<size_t>(y + pad) * w + (x + pad)] = coverage[static_cast<size_t>(y) * width + x] / 255.0f;
  BlurCoverage(&shape, w, h, ShadowRadius(style, fontSize));
  const float opacity = std::clamp(style.shadow, 0.0f, 1.0f);
  const uint8_t r = static_cast<uint8_t>((style.shadowColor >> 16) & 0xffu);
  const uint8_t g = static_cast<uint8_t>((style.shadowColor >> 8) & 0xffu);
  const uint8_t b = static_cast<uint8_t>(style.shadowColor & 0xffu);
  for (size_t i = 0; i < shape.size(); ++i)
  {
    // A little denser than the blur alone: thin strokes would otherwise cast
    // next to no shadow.
    const float a = std::min(1.0f, shape[i] * 1.4f) * opacity;
    (*rgba)[i * 4 + 0] = r;
    (*rgba)[i * 4 + 1] = g;
    (*rgba)[i * 4 + 2] = b;
    (*rgba)[i * 4 + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
  }
}

static void BoxBlurLine(const float* in, float* out, int n, int stride, int radius)
{
  // Running sum, clamped at the ends.
  const float scale = 1.0f / static_cast<float>(radius * 2 + 1);
  float sum = 0.0f;
  for (int i = -radius; i <= radius; ++i)
  {
    sum += in[std::clamp(i, 0, n - 1) * stride];
  }
  for (int i = 0; i < n; ++i)
  {
    out[i * stride] = sum * scale;
    sum += in[std::min(i + radius + 1, n - 1) * stride] - in[std::max(i - radius, 0) * stride];
  }
}

void BlurCoverage(std::vector<float>* values, int width, int height, int radius)
{
  if (radius <= 0 || width <= 0 || height <= 0) return;
  std::vector<float>& v = *values;
  std::vector<float> tmp(v.size());
  for (int pass = 0; pass < 3; ++pass)
  {
    for (int y = 0; y < height; ++y)
    {
      BoxBlurLine(&v[static_cast<size_t>(y) * width], &tmp[static_cast<size_t>(y) * width], width, 1, radius);
    }
    for (int x = 0; x < width; ++x)
    {
      BoxBlurLine(&tmp[x], &v[x], height, width, radius);
    }
  }
}

std::vector<uint8_t> ScaleCoverage(const uint8_t* src, int srcW, int srcH, int dstW, int dstH)
{
  std::vector<uint8_t> out(static_cast<size_t>(std::max(dstW, 0)) * std::max(dstH, 0), 0);
  if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return out;
  for (int y = 0; y < dstH; ++y)
  {
    const int y0 = y * srcH / dstH;
    const int y1 = std::max(y0 + 1, (y + 1) * srcH / dstH);
    for (int x = 0; x < dstW; ++x)
    {
      const int x0 = x * srcW / dstW;
      const int x1 = std::max(x0 + 1, (x + 1) * srcW / dstW);
      uint32_t sum = 0;
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx) sum += src[static_cast<size_t>(sy) * srcW + sx];
      out[static_cast<size_t>(y) * dstW + x] = static_cast<uint8_t>(sum / static_cast<uint32_t>((y1 - y0) * (x1 - x0)));
    }
  }
  return out;
}

// Deterministic per-pixel noise in [0, 1): the same glyph burns the same way
// every time, so a score does not shimmer when it is redrawn.
static float Noise(int x, int y)
{
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<float>(h & 0xffffu) / 65536.0f;
}

static float Channel(uint32_t rgb, int shift) { return static_cast<float>((rgb >> shift) & 0xffu); }

void Burn(const uint8_t* coverage, int width, int height, const Style& style, int fontSize, std::vector<uint8_t>* rgba,
          int* outWidth, int* outHeight)
{
  const int pad = BurnPad(style, fontSize);
  const int w = width + pad * 2;
  const int h = height + pad * 2;
  *outWidth = w;
  *outHeight = h;
  rgba->assign(static_cast<size_t>(w) * h * 4, 0);
  if (width <= 0 || height <= 0) return;

  std::vector<float> core(static_cast<size_t>(w) * h, 0.0f);
  for (int y = 0; y < height; ++y)
  {
    for (int x = 0; x < width; ++x)
    {
      core[static_cast<size_t>(y + pad) * w + (x + pad)] = coverage[static_cast<size_t>(y) * width + x] / 255.0f;
    }
  }
  // The scorch reaches out from the letter; a second, tight blur rounds the
  // inside edge of the core the way a hot iron leaves it.
  std::vector<float> scorch = core;
  BlurCoverage(&scorch, w, h, ScorchRadius(style, fontSize));
  std::vector<float> edge = core;
  BlurCoverage(&edge, w, h, std::max(1, fontSize / 40));

  const float coreR = Channel(style.core, 16), coreG = Channel(style.core, 8), coreB = Channel(style.core, 0);
  const float scR = Channel(style.scorch, 16), scG = Channel(style.scorch, 8), scB = Channel(style.scorch, 0);

  for (int y = 0; y < h; ++y)
  {
    for (int x = 0; x < w; ++x)
    {
      const size_t i = static_cast<size_t>(y) * w + x;
      const float c = std::min(core[i], edge[i] * 1.15f);
      // Scorch: strongest at the letter, gone at the reach. Squared so it falls
      // off quickly, the way char does, instead of a soft grey cloud.
      float s = std::min(1.0f, scorch[i] * 2.2f);
      s = s * s * style.scorchStrength;
      // The iron does not touch evenly: a little texture in the core.
      const float grain = 0.86f + 0.14f * Noise(x, y);

      float r, g, b, a;
      if (style.glow)
      {
        // Hot core over an orange heat that is brighter close in.
        const float heat = s;
        const float coreA = c;
        a = coreA + heat * (1.0f - coreA);
        const float mix = a > 0.0f ? coreA / a : 0.0f;
        r = coreR * mix + scR * (1.0f - mix);
        g = coreG * mix + scG * (1.0f - mix);
        b = coreB * mix + scB * (1.0f - mix);
        // The very middle of a stroke is hottest.
        const float hot = std::clamp((edge[i] - 0.85f) * 6.0f, 0.0f, 1.0f) * 0.35f;
        r += (255.0f - r) * hot;
        g += (255.0f - g) * hot;
        b += (230.0f - b) * hot;
      }
      else
      {
        const float coreA = c * grain;
        a = coreA + s * (1.0f - coreA);
        const float mix = a > 0.0f ? coreA / a : 0.0f;
        r = coreR * mix + scR * (1.0f - mix);
        g = coreG * mix + scG * (1.0f - mix);
        b = coreB * mix + scB * (1.0f - mix);
      }
      uint8_t* px = &(*rgba)[i * 4];
      px[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
      px[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
      px[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
      px[3] = static_cast<uint8_t>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  }
}

}  // namespace scoreboard
