#include "VirtualDMD.h"

void VirtualDMD::Update(uint8_t* pData)
{
  // Get window size to calculate scaling
  int windowWidth, windowHeight;
  SDL_GetRenderOutputSize(m_pRenderer, &windowWidth, &windowHeight);

  // Calculate pixel scaling factors
  float pixelWidth = (float)windowWidth / m_width;
  float pixelRadius = pixelWidth / 2.0f;

  // Clear the renderer
  SDL_SetRenderDrawColor(m_pRenderer, 0, 0, 0, 255);
  SDL_RenderClear(m_pRenderer);

  // Draw each pixel as a filled circle
  for (int y = 0; y < m_height; y++)
  {
    float centerY = y * pixelWidth + pixelWidth / 2.0f;

    for (int x = 0; x < m_width; x++)
    {
      // Calculate position in the array
      int idx = (y * m_width + x) * 3;

      // Get RGB values
      uint8_t r = pData[idx];
      uint8_t g = pData[idx + 1];
      uint8_t b = pData[idx + 2];

      // Skip drawing if pixel is black
      if (r == 0 && g == 0 && b == 0) continue;

      float centerX = x * pixelWidth + pixelWidth / 2.0f;

      SDL_SetRenderDrawColor(m_pRenderer, r, g, b, 255);

      // Simple filled circle implementation
      int radius = (int)pixelRadius;
      int centerXi = (int)centerX;
      int centerYi = (int)centerY;

      for (int dy = -radius; dy <= radius; dy++)
      {
        for (int dx = -radius; dx <= radius; dx++)
        {
          if (dx * dx + dy * dy <= radius * radius)
          {
            SDL_RenderPoint(m_pRenderer, centerXi + dx, centerYi + dy);
          }
        }
      }
    }
  }

  SDL_RenderPresent(m_pRenderer);
}
