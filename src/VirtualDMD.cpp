#include "VirtualDMD.h"

VirtualDMD::VirtualDMD(SDL_Window* window, uint16_t width, uint16_t height)
    : RGB24DMD(width, height), m_pWindow(window), m_texture(0), m_program(0), m_vbo(0)
{
}

VirtualDMD::~VirtualDMD() { m_pWindow = nullptr; }

void VirtualDMD::Update(uint8_t* pData)
{
  if (!m_contextInitialized)
  {
    LoadGLFunctions();
    InitGL();

    if (!m_program)
    {
      printf("Invalid shader program\n");
      return;
    }

    gl.glUseProgram(m_program);

    auto error = gl.glGetError();
    if (error != GL_NO_ERROR)
    {
      printf("OpenGL error after program initialization: %d\n", error);
    }
    else
    {
      printf("OpenGL program initialized.\n");
    }

    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBindTexture(GL_TEXTURE_2D, m_texture);

    GLint texUniform = gl.glGetUniformLocation(m_program, "uTexture");
    gl.glUniform1i(texUniform, 0);

    m_contextInitialized = true;
  }

  gl.glClear(GL_COLOR_BUFFER_BIT);

  // Shader und Textur aktivieren
  gl.glUseProgram(m_program);
  gl.glActiveTexture(GL_TEXTURE0);
  gl.glBindTexture(GL_TEXTURE_2D, m_texture);

  // Textur-Daten aktualisieren
  gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_width, m_height, 0, GL_RGB, GL_UNSIGNED_BYTE, pData);

  // VAO binden und zeichnen
  gl.glBindVertexArray(m_vao);
  gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // VAO unbinden
  gl.glBindVertexArray(0);

  // Buffer tauschen
  SDL_GL_SwapWindow(m_pWindow);
}

void VirtualDMD::LoadGLFunctions()
{
  gl.glCreateShader = (PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress("glCreateShader");
  gl.glShaderSource = (PFNGLSHADERSOURCEPROC)SDL_GL_GetProcAddress("glShaderSource");
  gl.glCompileShader = (PFNGLCOMPILESHADERPROC)SDL_GL_GetProcAddress("glCompileShader");
  gl.glCreateProgram = (PFNGLCREATEPROGRAMPROC)SDL_GL_GetProcAddress("glCreateProgram");
  gl.glAttachShader = (PFNGLATTACHSHADERPROC)SDL_GL_GetProcAddress("glAttachShader");
  gl.glLinkProgram = (PFNGLLINKPROGRAMPROC)SDL_GL_GetProcAddress("glLinkProgram");
  gl.glDeleteShader = (PFNGLDELETESHADERPROC)SDL_GL_GetProcAddress("glDeleteShader");
  gl.glUseProgram = (PFNGLUSEPROGRAMPROC)SDL_GL_GetProcAddress("glUseProgram");
  gl.glGetAttribLocation = (PFNGLGETATTRIBLOCATIONPROC)SDL_GL_GetProcAddress("glGetAttribLocation");
  gl.glGenBuffers = (PFNGLGENBUFFERSPROC)SDL_GL_GetProcAddress("glGenBuffers");
  gl.glBindBuffer = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
  gl.glBufferData = (PFNGLBUFFERDATAPROC)SDL_GL_GetProcAddress("glBufferData");
  gl.glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)SDL_GL_GetProcAddress("glEnableVertexAttribArray");
  gl.glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)SDL_GL_GetProcAddress("glVertexAttribPointer");
  gl.glDrawArrays = (PFNGLDRAWARRAYSPROC)SDL_GL_GetProcAddress("glDrawArrays");
  gl.glGenTextures = (PFNGLGENTEXTURESPROC)SDL_GL_GetProcAddress("glGenTextures");
  gl.glBindTexture = (PFNGLBINDTEXTUREPROC)SDL_GL_GetProcAddress("glBindTexture");
  gl.glTexImage2D = (PFNGLTEXIMAGE2DPROC)SDL_GL_GetProcAddress("glTexImage2D");
  gl.glTexParameteri = (PFNGLTEXPARAMETERIPROC)SDL_GL_GetProcAddress("glTexParameteri");
  gl.glGetShaderiv = (PFNGLGETSHADERIVPROC)SDL_GL_GetProcAddress("glGetShaderiv");
  gl.glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)SDL_GL_GetProcAddress("glGetShaderInfoLog");
  gl.glGetProgramiv = (PFNGLGETPROGRAMIVPROC)SDL_GL_GetProcAddress("glGetProgramiv");
  gl.glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)SDL_GL_GetProcAddress("glGetProgramInfoLog");
  gl.glDeleteProgram = (PFNGLDELETEPROGRAMPROC)SDL_GL_GetProcAddress("glDeleteProgram");
  gl.glGetError = (PFNGLGETERRORPROC)SDL_GL_GetProcAddress("glGetError");
  gl.glActiveTexture = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
  gl.glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)SDL_GL_GetProcAddress("glGetUniformLocation");
  gl.glUniform1i = (PFNGLUNIFORM1IPROC)SDL_GL_GetProcAddress("glUniform1i");
  gl.glClear = (PFNGLCLEARPROC)SDL_GL_GetProcAddress("glClear");
  gl.glGetIntegerv = (PFNGLGETINTEGERVPROC)SDL_GL_GetProcAddress("glGetIntegerv");
  gl.glGenVertexArrays = (PFNGLGENVERTEXARRAYSOESPROC)SDL_GL_GetProcAddress("glGenVertexArraysOES");
  gl.glBindVertexArray = (PFNGLBINDVERTEXARRAYOESPROC)SDL_GL_GetProcAddress("glBindVertexArrayOES");
  gl.glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSOESPROC)SDL_GL_GetProcAddress("glDeleteVertexArraysOES");
}

void VirtualDMD::InitGL()
{
  if (!m_pWindow)
  {
    printf("Window is missing\n");
    return;
  }

  // Initialize GL context attributes
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  // Create GL context
  m_glContext = SDL_GL_CreateContext(m_pWindow);
  if (!m_glContext)
  {
    printf("Failed to create GL context: %s\n", SDL_GetError());
    return;
  }

  const char* vsSource = R"(
        #version 150

        in vec2 aPos;
        in vec2 aTexCoord;
        out vec2 vTexCoord;

        void main() {
            vTexCoord = aTexCoord;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
    )";

  const char* fsSource = R"(
        #version 150

        in vec2 vTexCoord;
        out vec4 FragColor;
        uniform sampler2D uTexture;

        void main() {
            vec2 gridSize = textureSize(uTexture, 0);
            vec2 pixelCoord = floor(vTexCoord * gridSize) + 0.5;
            vec2 localCoord = fract(vTexCoord * gridSize) - 0.5;
            float dist = length(localCoord);
            if (dist > 0.5) discard;
            vec3 color = texture(uTexture, pixelCoord / gridSize).rgb;
            if (color == vec3(0.0)) discard;
            FragColor = vec4(color, 1.0);
        }
    )";

  m_program = CreateProgram(vsSource, fsSource);
  if (!m_program)
  {
    printf("Failed to create shader program\n");
    return;
  }

  gl.glGenVertexArrays(1, &m_vao);
  gl.glBindVertexArray(m_vao);

  // VBO erstellen und Daten übertragen
  float quadVertices[] = {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
                          -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f};

  gl.glGenBuffers(1, &m_vbo);
  gl.glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
  gl.glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

  // Shader-Attribute einrichten während VAO gebunden ist
  gl.glUseProgram(m_program);
  GLint posLoc = gl.glGetAttribLocation(m_program, "aPos");
  GLint texLoc = gl.glGetAttribLocation(m_program, "aTexCoord");

  gl.glEnableVertexAttribArray(posLoc);
  gl.glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
  gl.glEnableVertexAttribArray(texLoc);
  gl.glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

  // VAO unbinden
  gl.glBindVertexArray(0);
}

GLuint VirtualDMD::CompileShader(GLenum type, const char* source)
{
  GLuint shader = gl.glCreateShader(type);
  if (!shader)
  {
    printf("Failed to create shader\n");
    return 0;
  }

  gl.glShaderSource(shader, 1, &source, nullptr);
  gl.glCompileShader(shader);

  // Shader-Kompilierung prüfen
  GLint success;
  GLchar infoLog[512];
  gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success)
  {
    gl.glGetShaderInfoLog(shader, sizeof(infoLog), NULL, infoLog);
    printf("Shader compilation failed: %s\n", infoLog);
    gl.glDeleteShader(shader);
    return 0;
  }

  return shader;
}

GLuint VirtualDMD::CreateProgram(const char* vsSource, const char* fsSource)
{
  printf("Creating shader program...\n");

  GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSource);
  if (!vs)
  {
    printf("Vertex shader compilation failed\n");
    return 0;
  }

  GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSource);
  if (!fs)
  {
    printf("Fragment shader compilation failed\n");
    gl.glDeleteShader(vs);
    return 0;
  }

  GLuint program = gl.glCreateProgram();
  if (!program)
  {
    printf("Failed to create program\n");
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    return 0;
  }

  gl.glAttachShader(program, vs);
  gl.glAttachShader(program, fs);
  gl.glLinkProgram(program);

  // Programm-Verlinkung prüfen
  GLint success;
  GLchar infoLog[512];
  gl.glGetProgramiv(program, GL_LINK_STATUS, &success);
  if (!success)
  {
    gl.glGetProgramInfoLog(program, sizeof(infoLog), NULL, infoLog);
    printf("Program linking failed: %s\n", infoLog);
    gl.glDeleteProgram(program);
    gl.glDeleteShader(vs);
    gl.glDeleteShader(fs);
    return 0;
  }

  gl.glDeleteShader(vs);
  gl.glDeleteShader(fs);

  printf("Shader program created successfully\n");
  return program;
}
