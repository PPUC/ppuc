#include "VirtualDMD.h"

#include <vector>

#include "xbrz/xbrz.h"

void VirtualDMD::Update(uint8_t* pData)
{
  switch (m_renderingMode)
  {
    case RenderingMode::SmoothScaling:
      RenderSmoothScaling(pData);
      break;

    case RenderingMode::XBRZ:
      RenderXBRZ(pData);
      break;

    default:
      RenderDots(pData);
      break;
  }
}

void VirtualDMD::RenderDots(uint8_t* pData)
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

void VirtualDMD::RenderSmoothScaling(uint8_t* pData)
{
  int windowWidth, windowHeight;
  SDL_GetRenderOutputSize(m_pRenderer, &windowWidth, &windowHeight);

  // Create source texture
  SDL_Texture* srcTexture =
      SDL_CreateTexture(m_pRenderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC, m_width, m_height);
  SDL_UpdateTexture(srcTexture, NULL, pData, m_width * 3);

  // Create intermediate texture at 4x resolution
  int intermediateW = m_width * 4;
  int intermediateH = m_height * 4;
  SDL_Texture* intermediate =
      SDL_CreateTexture(m_pRenderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_TARGET, intermediateW, intermediateH);

  // First scale pass (nearest neighbor to 4x)
  SDL_SetRenderTarget(m_pRenderer, intermediate);
  SDL_SetTextureScaleMode(srcTexture, SDL_SCALEMODE_NEAREST);
  SDL_RenderTexture(m_pRenderer, srcTexture, NULL, NULL);

  // Second scale pass (linear to final size)
  SDL_SetRenderTarget(m_pRenderer, NULL);
  SDL_SetTextureScaleMode(intermediate, SDL_SCALEMODE_LINEAR);
  SDL_FRect dest = {0, 0, (float)windowWidth, (float)windowHeight};
  SDL_RenderTexture(m_pRenderer, intermediate, NULL, &dest);

  SDL_RenderPresent(m_pRenderer);

  // Cleanup
  SDL_DestroyTexture(srcTexture);
  SDL_DestroyTexture(intermediate);
}

void VirtualDMD::RenderXBRZ(uint8_t* pData)
{
  // Get the window size
  int windowWidth, windowHeight;
  SDL_GetRenderOutputSize(m_pRenderer, &windowWidth, &windowHeight);

  // Prepare source ARGB buffer
  const int srcWidth = m_width;
  const int srcHeight = m_height;
  std::vector<uint32_t> srcImage(srcWidth * srcHeight);

  for (int y = 0; y < srcHeight; ++y)
  {
    for (int x = 0; x < srcWidth; ++x)
    {
      int i = y * srcWidth + x;
      int j = i * 3;
      uint8_t r = pData[j];
      uint8_t g = pData[j + 1];
      uint8_t b = pData[j + 2];
      srcImage[i] = (255u << 24) | (r << 16) | (g << 8) | b;  // ARGB format
    }
  }

  // Set up xBRZ scaler configuration
  xbrz::ScalerCfg cfg;
  cfg.luminanceWeight = 1.0;
  cfg.equalColorTolerance = 30.0;
  cfg.centerDirectionBias = 4.0;
  cfg.dominantDirectionThreshold = 3.6;
  cfg.steepDirectionThreshold = 2.2;

  // xBRZ upscale (max factor: 6)
  const size_t scaleFactor = 6;
  const int scaledWidth = srcWidth * scaleFactor;
  const int scaledHeight = srcHeight * scaleFactor;
  std::vector<uint32_t> dstImage(scaledWidth * scaledHeight);

  xbrz::scale(scaleFactor, srcImage.data(), dstImage.data(), srcWidth, srcHeight, xbrz::ColorFormat::ARGB, cfg, 0,
              srcHeight);

  // Create an SDL texture from the scaled buffer
  SDL_Texture* texture =
      SDL_CreateTexture(m_pRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, scaledWidth, scaledHeight);

  SDL_UpdateTexture(texture, nullptr, dstImage.data(), scaledWidth * sizeof(uint32_t));

  // Clear and render the texture
  SDL_RenderClear(m_pRenderer);

  SDL_FRect dstRect = {0.0f, 0.0f, (float)windowWidth, (float)windowHeight};
  SDL_RenderTexture(m_pRenderer, texture, nullptr, &dstRect);

  SDL_RenderPresent(m_pRenderer);

  SDL_DestroyTexture(texture);
}
