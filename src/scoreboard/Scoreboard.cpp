#include "Scoreboard.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "BurnEffect.h"
#include "PpucLogoMask.h"
#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"
#ifdef PPUC_HAS_SDL3_TTF
#include "SDL3_ttf/SDL_ttf.h"
#endif

namespace scoreboard
{

Board::~Board() { Unload(); }

void Board::Unload()
{
  for (auto& [key, face] : m_faces)
  {
    for (auto& [cp, glyph] : face.glyphs)
    {
      if (glyph.texture) SDL_DestroyTexture(glyph.texture);
    }
#ifdef PPUC_HAS_SDL3_TTF
    if (face.font) TTF_CloseFont(face.font);
#endif
  }
  m_faces.clear();
  for (auto& [name, texture] : m_pictures)
  {
    if (texture) SDL_DestroyTexture(texture);
  }
  m_pictures.clear();
  if (m_frame) SDL_DestroyTexture(m_frame);
  if (m_idle) SDL_DestroyTexture(m_idle);
  if (m_logo) SDL_DestroyTexture(m_logo);
  m_frame = m_idle = m_logo = nullptr;
  m_renderer = nullptr;
}

size_t Board::GlyphCount() const
{
  size_t n = 0;
  for (const auto& [key, face] : m_faces) n += face.glyphs.size();
  return n;
}

int Board::PixelSize(int layoutSize) const
{
  const float scale = m_outputH > 0 ? static_cast<float>(m_outputH) / static_cast<float>(m_layout.height) : 1.0f;
  return std::max(4, static_cast<int>(std::lround(static_cast<float>(layoutSize) * scale)));
}

static SDL_Texture* LoadTexture(SDL_Renderer* renderer, const std::string& path, std::string* pError)
{
  SDL_Texture* texture = IMG_LoadTexture(renderer, path.c_str());
  if (texture == nullptr && pError) *pError = path + ": " + SDL_GetError();
  return texture;
}

SDL_Texture* Board::Picture(const std::string& name)
{
  if (name.empty()) return nullptr;
  const auto it = m_pictures.find(name);
  if (it != m_pictures.end()) return it->second;
  std::string error;
  SDL_Texture* texture = LoadTexture(m_renderer, (std::filesystem::path(m_folder) / name).string(), &error);
  if (texture == nullptr) SDL_Log("Scoreboard: %s", error.c_str());
  m_pictures[name] = texture;
  return texture;
}

bool Board::Load(const Layout& layout, const std::string& folder, SDL_Renderer* renderer, std::string* pError)
{
  Unload();
#ifndef PPUC_HAS_SDL3_TTF
  (void)layout;
  (void)folder;
  (void)renderer;
  if (pError) *pError = "this build has no SDL_ttf, and the scoreboard is all text";
  return false;
#else
  if (!TTF_WasInit() && !TTF_Init())
  {
    if (pError) *pError = std::string("TTF_Init failed: ") + SDL_GetError();
    return false;
  }
  m_layout = layout;
  m_folder = folder;
  m_renderer = renderer;
  m_fontPath = (std::filesystem::path(folder) / layout.font).string();
  if (!std::filesystem::exists(m_fontPath))
  {
    if (pError) *pError = m_fontPath + ": no such font";
    m_renderer = nullptr;
    return false;
  }
  if (!layout.frame.empty() &&
      (m_frame = LoadTexture(renderer, (std::filesystem::path(folder) / layout.frame).string(), pError)) == nullptr)
  {
    Unload();
    return false;
  }
  if (!layout.idle.empty() &&
      (m_idle = LoadTexture(renderer, (std::filesystem::path(folder) / layout.idle).string(), pError)) == nullptr)
  {
    Unload();
    return false;
  }
  for (const Field& field : layout.fields)
  {
    if (field.type != Field::Type::Icons) continue;
    if (Picture(field.on) == nullptr || (!field.off.empty() && Picture(field.off) == nullptr))
    {
      if (pError) *pError = "an icon picture of the layout is missing, see the log";
      Unload();
      return false;
    }
  }
  return true;
#endif
}

Board::Face* Board::GetFace(const std::string& style, int pixelSize)
{
#ifdef PPUC_HAS_SDL3_TTF
  const auto key = std::make_pair(style, pixelSize);
  auto it = m_faces.find(key);
  if (it != m_faces.end()) return &it->second;
  Face face;
  face.font = TTF_OpenFont(m_fontPath.c_str(), static_cast<float>(pixelSize));
  if (face.font == nullptr)
  {
    SDL_Log("Scoreboard: %s at %d px: %s", m_fontPath.c_str(), pixelSize, SDL_GetError());
    return nullptr;
  }
  const auto styleIt = m_layout.styles.find(style);
  face.pad = BurnPad(styleIt != m_layout.styles.end() ? styleIt->second : Style{}, pixelSize);
  return &m_faces.emplace(key, std::move(face)).first->second;
#else
  (void)style;
  (void)pixelSize;
  return nullptr;
#endif
}

static SDL_Texture* TextureFromRgba(SDL_Renderer* renderer, const std::vector<uint8_t>& rgba, int w, int h)
{
  SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
  if (texture == nullptr) return nullptr;
  SDL_UpdateTexture(texture, nullptr, rgba.data(), w * 4);
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  return texture;
}

#ifdef PPUC_HAS_SDL3_TTF
// A font renderer's coverage for some text, one byte per pixel.
static bool Coverage(TTF_Font* font, const char* utf8, std::vector<uint8_t>* out, int* w, int* h)
{
  SDL_Surface* surface = TTF_RenderText_Blended(font, utf8, 0, SDL_Color{255, 255, 255, 255});
  if (surface == nullptr) return false;
  SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(surface);
  if (rgba == nullptr) return false;
  *w = rgba->w;
  *h = rgba->h;
  out->assign(static_cast<size_t>(rgba->w) * rgba->h, 0);
  for (int y = 0; y < rgba->h; ++y)
  {
    const uint8_t* row = static_cast<const uint8_t*>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch;
    for (int x = 0; x < rgba->w; ++x) (*out)[static_cast<size_t>(y) * rgba->w + x] = row[x * 4 + 3];
  }
  SDL_DestroySurface(rgba);
  return true;
}

static std::string EncodeUtf8(uint32_t cp)
{
  std::string s;
  if (cp < 0x80)
    s += static_cast<char>(cp);
  else if (cp < 0x800)
  {
    s += static_cast<char>(0xc0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3f));
  }
  else if (cp < 0x10000)
  {
    s += static_cast<char>(0xe0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    s += static_cast<char>(0x80 | (cp & 0x3f));
  }
  else
  {
    s += static_cast<char>(0xf0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    s += static_cast<char>(0x80 | (cp & 0x3f));
  }
  return s;
}
#endif

const Board::Glyph* Board::GetGlyph(Face* face, const std::string& style, int pixelSize, uint32_t codepoint)
{
  auto it = face->glyphs.find(codepoint);
  if (it != face->glyphs.end()) return &it->second;
  Glyph glyph;
#ifdef PPUC_HAS_SDL3_TTF
  int advance = 0;
  int minx = 0, maxx = 0, miny = 0, maxy = 0;
  if (TTF_GetGlyphMetrics(face->font, codepoint, &minx, &maxx, &miny, &maxy, &advance)) glyph.advance = advance;
  if (codepoint != ' ')
  {
    std::vector<uint8_t> coverage;
    int w = 0, h = 0;
    if (Coverage(face->font, EncodeUtf8(codepoint).c_str(), &coverage, &w, &h))
    {
      const auto styleIt = m_layout.styles.find(style);
      std::vector<uint8_t> rgba;
      Burn(coverage.data(), w, h, styleIt != m_layout.styles.end() ? styleIt->second : Style{}, pixelSize, &rgba,
           &glyph.width, &glyph.height);
      glyph.texture = TextureFromRgba(m_renderer, rgba, glyph.width, glyph.height);
    }
  }
#else
  (void)style;
  (void)pixelSize;
#endif
  return &face->glyphs.emplace(codepoint, glyph).first->second;
}

int Board::MeasureText(const std::string& text, const std::string& style, int pixelSize)
{
  Face* face = GetFace(style, pixelSize);
  if (face == nullptr) return 0;
  int width = 0;
  for (const uint32_t cp : DecodeUtf8(text))
  {
    const Glyph* glyph = GetGlyph(face, style, pixelSize, cp);
    width += glyph->advance;
  }
  return width;
}

void Board::DrawText(const std::string& text, const std::string& style, int pixelSize, float x, float y, Align align)
{
  Face* face = GetFace(style, pixelSize);
  if (face == nullptr || text.empty()) return;
  const int width = MeasureText(text, style, pixelSize);
  float pen = x;
  if (align == Align::Center) pen -= static_cast<float>(width) / 2.0f;
  if (align == Align::Right) pen -= static_cast<float>(width);
  for (const uint32_t cp : DecodeUtf8(text))
  {
    const Glyph* glyph = GetGlyph(face, style, pixelSize, cp);
    if (glyph->texture)
    {
      const SDL_FRect dst{std::round(pen) - face->pad, std::round(y) - face->pad, static_cast<float>(glyph->width),
                          static_cast<float>(glyph->height)};
      SDL_RenderTexture(m_renderer, glyph->texture, nullptr, &dst);
    }
    pen += static_cast<float>(glyph->advance);
  }
}

void Board::DrawPosed(const std::string& text, const std::string& style, int pixelSize, float cx, float cy, float scale,
                      float alpha)
{
  Face* face = GetFace(style, pixelSize);
  if (face == nullptr || text.empty() || alpha <= 0.0f) return;
  const float width = static_cast<float>(MeasureText(text, style, pixelSize)) * scale;
  // The glyph boxes are the font's line height; their middle is close enough
  // to the middle of capitals and digits for a callout.
  const Glyph* any = nullptr;
  for (const uint32_t cp : DecodeUtf8(text))
  {
    any = GetGlyph(face, style, pixelSize, cp);
    if (any->texture) break;
  }
  if (any == nullptr) return;
  const float boxH = static_cast<float>(any->height - 2 * face->pad);
  float pen = cx - width / 2.0f;
  const float top = cy - boxH * scale / 2.0f;
  const uint8_t a = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
  for (const uint32_t cp : DecodeUtf8(text))
  {
    const Glyph* glyph = GetGlyph(face, style, pixelSize, cp);
    if (glyph->texture)
    {
      SDL_SetTextureAlphaMod(glyph->texture, a);
      const SDL_FRect dst{pen - face->pad * scale, top - face->pad * scale, glyph->width * scale,
                          glyph->height * scale};
      SDL_RenderTexture(m_renderer, glyph->texture, nullptr, &dst);
      SDL_SetTextureAlphaMod(glyph->texture, 255);
    }
    pen += static_cast<float>(glyph->advance) * scale;
  }
}

void Board::StartCallout(const Callout& callout, uint64_t nowMs)
{
  const std::string spot = m_layout.spots.count(callout.spot) ? callout.spot : "center";
  auto it = m_callouts.find(spot);
  if (it != m_callouts.end() && nowMs >= it->second.startMs &&
      !CalloutReplaces(it->second.callout, nowMs - it->second.startMs, callout))
  {
    return;
  }
  ActiveCallout active;
  active.callout = callout;
  active.callout.spot = spot;
  active.startMs = nowMs;
  m_callouts[spot] = active;
}

bool Board::Animating(uint64_t nowMs) const
{
  for (const auto& [spot, active] : m_callouts)
  {
    if (nowMs >= active.startMs && PoseAt(active.callout, nowMs - active.startMs).visible) return true;
  }
  return false;
}

void Board::DrawCallout(const ActiveCallout& active, float sx, float sy, uint64_t nowMs)
{
  if (nowMs < active.startMs) return;
  const Callout& callout = active.callout;
  const CalloutPose pose = PoseAt(callout, nowMs - active.startMs);
  if (!pose.visible) return;
  const auto spotIt = m_layout.spots.find(callout.spot);
  if (spotIt == m_layout.spots.end()) return;
  const Spot& spot = spotIt->second;
  const std::string& style = callout.style.empty() ? spot.style : callout.style;
  const int px = PixelSize(callout.size > 0 ? callout.size : spot.size);
  const float cx = spot.x * sx + pose.dx * px;
  const float cy = spot.y * sy + pose.dy * px;
  // A callout over the scene stays in the scene: shrunk to 90 % of the
  // window's width when it is wider, and clipped there, so the zoom of a long
  // word never paints over the frame.
  const SDL_Rect window{static_cast<int>(m_layout.windowX * sx), static_cast<int>(m_layout.windowY * sy),
                        static_cast<int>(m_layout.windowW * sx), static_cast<int>(m_layout.windowH * sy)};
  const bool inWindow = cx >= window.x && cx < window.x + window.w && cy >= window.y && cy < window.y + window.h;
  float fit = 1.0f;
  if (inWindow)
  {
    const float width = static_cast<float>(MeasureText(callout.text, style, px));
    const float room = window.w * 0.9f;
    if (width > room) fit = room / width;
    SDL_SetRenderClipRect(m_renderer, &window);
  }
  DrawPosed(callout.text, style, px, cx, cy, pose.scale * fit, pose.alpha);
  if (inWindow) SDL_SetRenderClipRect(m_renderer, nullptr);
}

void Board::Prepare(const std::string& text, const std::string& style, int pixelSize)
{
  Face* face = GetFace(style, pixelSize);
  if (face == nullptr) return;
  for (const uint32_t cp : DecodeUtf8(text)) GetGlyph(face, style, pixelSize, cp);
}

// The PPUC brand: the emblem from the boot splash, burnt like everything else
// on the board. Made once at the size it is shown at.
bool Board::MakeLogo(int pixelSize, std::string* pError)
{
  if (m_logo) SDL_DestroyTexture(m_logo);
  m_logo = nullptr;
  SDL_IOStream* io = SDL_IOFromConstMem(kPpucLogoMaskPng, kPpucLogoMaskPng_len);
  SDL_Surface* loaded = io ? IMG_Load_IO(io, true) : nullptr;
  SDL_Surface* mask = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : nullptr;
  if (loaded) SDL_DestroySurface(loaded);
  if (mask == nullptr)
  {
    if (pError) *pError = std::string("the PPUC logo: ") + SDL_GetError();
    return false;
  }
  std::vector<uint8_t> source(static_cast<size_t>(mask->w) * mask->h);
  for (int y = 0; y < mask->h; ++y)
  {
    const uint8_t* row = static_cast<const uint8_t*>(mask->pixels) + static_cast<size_t>(y) * mask->pitch;
    for (int x = 0; x < mask->w; ++x) source[static_cast<size_t>(y) * mask->w + x] = row[x * 4];
  }
  const int size = std::max(8, pixelSize);
  std::vector<uint8_t> coverage = ScaleCoverage(source.data(), mask->w, mask->h, size, size);
  SDL_DestroySurface(mask);

  const auto styleIt = m_layout.styles.find(m_layout.logoStyle);
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  // Burnt as if it were small type: the ring lettering is fine detail, and
  // a scorch sized for the whole emblem would char it into one blot.
  Burn(coverage.data(), size, size, styleIt != m_layout.styles.end() ? styleIt->second : Style{},
       std::max(12, size / 7), &rgba, &w, &h);
  m_logo = TextureFromRgba(m_renderer, rgba, w, h);
  m_logoPixels = size;
  if (m_logo == nullptr && pError) *pError = SDL_GetError();
  return m_logo != nullptr;
}

void Board::Draw(int w, int h, const Snapshot& snapshot, bool fillWindow, uint64_t nowMs)
{
  if (m_renderer == nullptr || w <= 0 || h <= 0) return;
  // The board keeps its own shape: on a screen of another shape (a 21:9
  // monitor, a 4:3 one) it is the largest board that fits, in the middle, and
  // the rest is black. Stretched, the frame would widen while the type and the
  // brand, made at the board's height, would not, and the logo would leave its
  // plate.
  const float fit = std::min(static_cast<float>(w) / static_cast<float>(m_layout.width),
                             static_cast<float>(h) / static_cast<float>(m_layout.height));
  const int bw = std::max(1, static_cast<int>(std::lround(m_layout.width * fit)));
  const int bh = std::max(1, static_cast<int>(std::lround(m_layout.height * fit)));
  const int bx = (w - bw) / 2;
  const int by = (h - bh) / 2;
  if (bw == w && bh == h)
  {
    DrawBoard(w, h, snapshot, fillWindow, nowMs);
    return;
  }
  Uint8 r = 0, g = 0, b = 0, a = 0;
  SDL_GetRenderDrawColor(m_renderer, &r, &g, &b, &a);
  SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
  // Opaque even over a backglass video: whatever plays there was not made for
  // the strips beside the board.
  const SDL_FRect bars[2] = {
      bw < w ? SDL_FRect{0, 0, static_cast<float>(bx), static_cast<float>(h)}
             : SDL_FRect{0, 0, static_cast<float>(w), static_cast<float>(by)},
      bw < w ? SDL_FRect{static_cast<float>(bx + bw), 0, static_cast<float>(w - bx - bw), static_cast<float>(h)}
             : SDL_FRect{0, static_cast<float>(by + bh), static_cast<float>(w), static_cast<float>(h - by - bh)}};
  SDL_RenderFillRects(m_renderer, bars, 2);
  SDL_SetRenderDrawColor(m_renderer, r, g, b, a);

  // Everything below draws in board coordinates; a clip rectangle a callout
  // sets is relative to this viewport too.
  const SDL_Rect viewport{bx, by, bw, bh};
  SDL_SetRenderViewport(m_renderer, &viewport);
  DrawBoard(bw, bh, snapshot, fillWindow, nowMs);
  SDL_SetRenderViewport(m_renderer, nullptr);
}

void Board::DrawBoard(int w, int h, const Snapshot& snapshot, bool fillWindow, uint64_t nowMs)
{
  if (w != m_outputW || h != m_outputH)
  {
    // A new output size: glyphs and the brand are remade at the size they will
    // be shown at, so nothing is scaled and nothing goes soft.
    for (auto& [key, face] : m_faces)
    {
      for (auto& [cp, glyph] : face.glyphs)
        if (glyph.texture) SDL_DestroyTexture(glyph.texture);
#ifdef PPUC_HAS_SDL3_TTF
      if (face.font) TTF_CloseFont(face.font);
#endif
    }
    m_faces.clear();
    m_outputW = w;
    m_outputH = h;
    MakeLogo(PixelSize(m_layout.logoSize), nullptr);
    for (const Field& field : m_layout.fields)
    {
      if (field.type == Field::Type::Icons) continue;
      const int px = PixelSize(field.size);
      Prepare("0123456789,PS ", field.style, px);
      Prepare(field.text, field.style, px);
      Prepare(field.format, field.style, px);
      for (const auto& [name, items] : m_layout.lists)
        for (const std::string& item : items) Prepare(item, field.style, px);
    }
    // Callouts: digits and the layout's words, at every spot's size.
    for (const auto& [name, spot] : m_layout.spots)
    {
      const int px = PixelSize(spot.size);
      Prepare("0123456789,", spot.style, px);
      for (const std::string& word : m_layout.calloutWords) Prepare(word, spot.style, px);
    }
  }

  const float sx = static_cast<float>(w) / static_cast<float>(m_layout.width);
  const float sy = static_cast<float>(h) / static_cast<float>(m_layout.height);

  if (fillWindow && m_idle)
  {
    const SDL_FRect window{m_layout.windowX * sx, m_layout.windowY * sy, m_layout.windowW * sx, m_layout.windowH * sy};
    SDL_RenderTexture(m_renderer, m_idle, nullptr, &window);
  }
  if (m_frame) SDL_RenderTexture(m_renderer, m_frame, nullptr, nullptr);

  const int featured = FeaturedPlayer(snapshot);
  for (const Field& field : m_layout.fields)
  {
    if (!FieldVisible(field, snapshot)) continue;
    const int px = PixelSize(field.size);
    const float x = field.x * sx;
    const float y = field.y * sy;
    switch (field.type)
    {
      case Field::Type::Text:
        DrawText(ExpandText(field.text, m_layout, snapshot), field.style, px, x, y, field.align);
        break;
      case Field::Type::CurrentScore:
        if (featured > 0)
        {
          DrawText(FormatScore(snapshot.facts.scores[featured - 1]), field.style, px, x, y, field.align);
        }
        break;
      case Field::Type::OtherScores:
      {
        const std::vector<int> others = OtherPlayers(snapshot);
        if (others.empty()) break;
        const float slot = field.width * sx / static_cast<float>(others.size());
        for (size_t k = 0; k < others.size(); ++k)
        {
          const int p = others[k];
          std::string text = field.format;
          const size_t n = text.find("{n}");
          if (n != std::string::npos) text.replace(n, 3, std::to_string(p));
          const size_t s = text.find("{score}");
          if (s != std::string::npos) text.replace(s, 7, FormatScore(snapshot.facts.scores[p - 1]));
          DrawText(text, field.style, px, x + slot * (static_cast<float>(k) + 0.5f), y, Align::Center);
        }
        break;
      }
      case Field::Type::Icons:
      {
        const std::vector<bool> lit = IconsLit(field, m_layout, snapshot);
        SDL_Texture* on = Picture(field.on);
        SDL_Texture* off = Picture(field.off);
        for (int i = 0; i < field.count; ++i)
        {
          SDL_Texture* texture = lit[static_cast<size_t>(i)] ? on : off;
          if (texture == nullptr) continue;
          float tw = static_cast<float>(field.iconWidth);
          float th = static_cast<float>(field.iconHeight);
          if (tw <= 0 || th <= 0) SDL_GetTextureSize(texture, &tw, &th);
          const int col = field.columns > 0 ? i % field.columns : i;
          const int row = field.columns > 0 ? i / field.columns : 0;
          const float ix = field.columns > 0 ? field.x + col * field.dx : field.x + i * field.dx;
          const float iy = field.columns > 0 ? field.y + row * field.dy : field.y + i * field.dy;
          const SDL_FRect dst{ix * sx, iy * sy, tw * sx, th * sy};
          SDL_RenderTexture(m_renderer, texture, nullptr, &dst);
        }
        break;
      }
    }
  }

  for (auto it = m_callouts.begin(); it != m_callouts.end();)
  {
    if (nowMs >= it->second.startMs && !PoseAt(it->second.callout, nowMs - it->second.startMs).visible)
    {
      it = m_callouts.erase(it);
      continue;
    }
    DrawCallout(it->second, sx, sy, nowMs);
    ++it;
  }

  // The brand last, so nothing can cover it.
  if (m_logo)
  {
    float lw = 0, lh = 0;
    SDL_GetTextureSize(m_logo, &lw, &lh);
    const float pad = (lw - m_logoPixels) / 2.0f;
    const float margin = m_layout.logoMargin * sx;
    const bool right = m_layout.logoCorner == Corner::TopRight || m_layout.logoCorner == Corner::BottomRight;
    const bool bottom = m_layout.logoCorner == Corner::BottomLeft || m_layout.logoCorner == Corner::BottomRight;
    const float lx = right ? w - margin - m_logoPixels - pad : margin - pad;
    const float ly = bottom ? h - margin - m_logoPixels - pad : margin - pad;
    const SDL_FRect dst{std::round(lx), std::round(ly), lw, lh};
    SDL_RenderTexture(m_renderer, m_logo, nullptr, &dst);
  }
}

}  // namespace scoreboard
