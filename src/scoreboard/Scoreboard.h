#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ScoreboardModel.h"

struct SDL_Renderer;
struct SDL_Texture;
struct TTF_Font;

namespace scoreboard
{

// Draws a scoreboard layout into an SDL renderer.
//
// Everything it draws is made once: the frame and the pictures are textures,
// every glyph is burnt once per style and size and kept, the PPUC brand is one
// texture. A frame is then the frame texture plus a few dozen small copies, which
// is nothing next to the scene video under it. Drawing makes no new texture
// unless a text uses a character that was never drawn before.
//
// Belongs to the thread of the renderer it was loaded into, like the renderer.
class Board
{
 public:
  Board() = default;
  ~Board();
  Board(const Board&) = delete;
  Board& operator=(const Board&) = delete;

  // `folder` holds the layout's pictures and font. Prepares the glyphs every
  // field can show, so the first score of a game does not wait for them.
  bool Load(const Layout& layout, const std::string& folder, SDL_Renderer* renderer, std::string* pError);
  void Unload();
  bool Loaded() const { return m_renderer != nullptr; }
  SDL_Renderer* Renderer() const { return m_renderer; }
  const Layout& GetLayout() const { return m_layout; }

  // On top of what the renderer holds, scaled from the layout's size to w x h.
  // `fillWindow`: put the idle picture into the window first, for when no
  // video plays behind the frame.
  void Draw(int w, int h, const Snapshot& snapshot, bool fillWindow);

  // How many glyph textures exist, for the tests and the log.
  size_t GlyphCount() const;

 private:
  struct Glyph
  {
    SDL_Texture* texture = nullptr;
    int width = 0;   // the texture, scorch included
    int height = 0;
    int advance = 0;
  };
  struct Face
  {
    TTF_Font* font = nullptr;
    int pad = 0;
    std::map<uint32_t, Glyph> glyphs;
  };

  Face* GetFace(const std::string& style, int pixelSize);
  const Glyph* GetGlyph(Face* face, const std::string& style, int pixelSize, uint32_t codepoint);
  int MeasureText(const std::string& text, const std::string& style, int pixelSize);
  void DrawText(const std::string& text, const std::string& style, int pixelSize, float x, float y, Align align);
  void Prepare(const std::string& text, const std::string& style, int pixelSize);
  SDL_Texture* Picture(const std::string& name);
  bool MakeLogo(int pixelSize, std::string* pError);
  int PixelSize(int layoutSize) const;

  Layout m_layout;
  std::string m_folder;
  std::string m_fontPath;
  SDL_Renderer* m_renderer = nullptr;
  SDL_Texture* m_frame = nullptr;
  SDL_Texture* m_idle = nullptr;
  SDL_Texture* m_logo = nullptr;
  int m_logoPixels = 0;
  std::map<std::string, SDL_Texture*> m_pictures;
  std::map<std::pair<std::string, int>, Face> m_faces;
  // Output size the glyphs were made for; a change remakes them.
  int m_outputW = 0;
  int m_outputH = 0;
};

}  // namespace scoreboard
