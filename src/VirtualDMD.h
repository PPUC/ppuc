#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengles2.h>

#include "DMDUtil/RGB24DMD.h"

class DMDUTILAPI VirtualDMD : public DMDUtil::RGB24DMD
{
 public:
  VirtualDMD(SDL_Window* window, uint16_t width, uint16_t height);
  ~VirtualDMD();

  virtual void Update(uint8_t* pData) override;

 private:
  SDL_Window* m_pWindow;
  SDL_GLContext m_glContext;
  bool m_contextInitialized{false};
  GLuint m_texture;
  GLuint m_program;
  GLuint m_vbo;
  GLuint m_vao;

  // Function pointers for GLES
  struct GLFunctions
  {
    PFNGLCREATESHADERPROC glCreateShader;
    PFNGLSHADERSOURCEPROC glShaderSource;
    PFNGLCOMPILESHADERPROC glCompileShader;
    PFNGLCREATEPROGRAMPROC glCreateProgram;
    PFNGLATTACHSHADERPROC glAttachShader;
    PFNGLLINKPROGRAMPROC glLinkProgram;
    PFNGLDELETESHADERPROC glDeleteShader;
    PFNGLUSEPROGRAMPROC glUseProgram;
    PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation;
    PFNGLGENBUFFERSPROC glGenBuffers;
    PFNGLBINDBUFFERPROC glBindBuffer;
    PFNGLBUFFERDATAPROC glBufferData;
    PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer;
    PFNGLDRAWARRAYSPROC glDrawArrays;
    PFNGLGENTEXTURESPROC glGenTextures;
    PFNGLBINDTEXTUREPROC glBindTexture;
    PFNGLTEXIMAGE2DPROC glTexImage2D;
    PFNGLTEXPARAMETERIPROC glTexParameteri;
    PFNGLGETSHADERIVPROC glGetShaderiv;
    PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
    PFNGLGETPROGRAMIVPROC glGetProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
    PFNGLDELETEPROGRAMPROC glDeleteProgram;
    PFNGLGETERRORPROC glGetError;
    PFNGLACTIVETEXTUREPROC glActiveTexture;
    PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
    PFNGLUNIFORM1IPROC glUniform1i;
    PFNGLCLEARPROC glClear;
    PFNGLGETINTEGERVPROC glGetIntegerv;
    PFNGLGENVERTEXARRAYSOESPROC glGenVertexArrays;
    PFNGLBINDVERTEXARRAYOESPROC glBindVertexArray;
    PFNGLDELETEVERTEXARRAYSOESPROC glDeleteVertexArrays;
  } gl;

  void InitGL();
  void LoadGLFunctions();
  GLuint CompileShader(GLenum type, const char* source);
  GLuint CreateProgram(const char* vsSource, const char* fsSource);
};