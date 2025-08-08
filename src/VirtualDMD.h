#pragma once

#include <SDL3/SDL.h>

#include "DMDUtil/RGB24DMD.h"

class DMDUTILAPI VirtualDMD : public DMDUtil::RGB24DMD
{
 public:
  VirtualDMD(SDL_Renderer* renderer, uint16_t width, uint16_t height) : RGB24DMD(width, height), m_pRenderer(renderer)
  {
  }

  ~VirtualDMD() { m_pRenderer = nullptr; }

  enum RenderingMode : int
  {
    Dots = 0,
    SmoothScaling = 1,
    XBRZ = 2,
  };

  virtual void Update(uint8_t* pData, uint16_t width = 0, uint16_t height = 0) override;

  void SetRenderingMode(RenderingMode mode) { m_renderingMode = mode; }

 private:
  SDL_Renderer* m_pRenderer;
  int m_renderingMode = RenderingMode::Dots;

  void RenderDots(uint8_t* pData, uint16_t width, uint16_t height);
  void RenderSmoothScaling(uint8_t* pData, uint16_t width, uint16_t height);
  void RenderXBRZ(uint8_t* pData, uint16_t width, uint16_t height);
};
