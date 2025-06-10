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

  virtual void Update(uint8_t* pData) override;

 private:
  SDL_Renderer* m_pRenderer;
};
