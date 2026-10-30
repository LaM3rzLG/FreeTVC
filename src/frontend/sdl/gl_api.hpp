// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenGL 3.3 core functions the picture model uses, loaded through SDL_GL_GetProcAddress (no GL headers or library).
#pragma once

#include <cstddef>
#include <cstdint>

#include <SDL3/SDL.h>

#if defined(_WIN32) && !defined(__CYGWIN__)
#define FREETVC_GLAPI __stdcall
#else
#define FREETVC_GLAPI
#endif

namespace freetvc::sdl::gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLchar = char;
using GLubyte = unsigned char;

inline constexpr GLenum TEXTURE_2D = 0x0DE1;
inline constexpr GLenum TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum TEXTURE_WRAP_T = 0x2803;
inline constexpr GLint NEAREST = 0x2600;
inline constexpr GLint LINEAR = 0x2601;
inline constexpr GLint CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum RGBA = 0x1908;
inline constexpr GLint RGBA8 = 0x8058;
inline constexpr GLint RGBA16F = 0x881A;
inline constexpr GLint RG16F = 0x822F;
inline constexpr GLenum RG = 0x8227;
inline constexpr GLenum BGRA = 0x80E1;
inline constexpr GLenum UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum FLOAT = 0x1406;
inline constexpr GLenum UNPACK_ROW_LENGTH = 0x0CF2;
inline constexpr GLenum UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum FRAMEBUFFER = 0x8D40;
inline constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum FRAMEBUFFER_COMPLETE = 0x8CD5;
inline constexpr GLenum FRAMEBUFFER_BINDING = 0x8CA6;
inline constexpr GLenum TRIANGLES = 0x0004;
inline constexpr GLenum VERTEX_SHADER = 0x8B31;
inline constexpr GLenum FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum COMPILE_STATUS = 0x8B81;
inline constexpr GLenum LINK_STATUS = 0x8B82;
inline constexpr GLenum TEXTURE0 = 0x84C0;
inline constexpr GLenum SCISSOR_TEST = 0x0C11;
inline constexpr GLenum BLEND = 0x0BE2;
inline constexpr GLenum VIEWPORT = 0x0BA2;
inline constexpr GLenum VERSION = 0x1F02;
inline constexpr GLenum RENDERER = 0x1F01;
inline constexpr GLbitfield COLOR_BUFFER_BIT = 0x4000;
inline constexpr GLenum MAJOR_VERSION = 0x821B;
inline constexpr GLenum MINOR_VERSION = 0x821C;

#define FREETVC_GL_FUNCTIONS(X)                                                                                            \
    X(void, GetIntegerv, GLenum, GLint*)                                                                                   \
    X(const GLubyte*, GetString, GLenum)                                                                                   \
    X(void, Viewport, GLint, GLint, GLsizei, GLsizei)                                                                      \
    X(void, Scissor, GLint, GLint, GLsizei, GLsizei)                                                                       \
    X(void, Enable, GLenum)                                                                                                \
    X(void, Disable, GLenum)                                                                                               \
    X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                                                \
    X(void, Clear, GLbitfield)                                                                                             \
    X(void, DrawArrays, GLenum, GLint, GLsizei)                                                                            \
    X(void, GenTextures, GLsizei, GLuint*)                                                                                 \
    X(void, DeleteTextures, GLsizei, const GLuint*)                                                                        \
    X(void, BindTexture, GLenum, GLuint)                                                                                   \
    X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)                       \
    X(void, TexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)                    \
    X(void, TexParameteri, GLenum, GLenum, GLint)                                                                          \
    X(void, PixelStorei, GLenum, GLint)                                                                                    \
    X(void, ActiveTexture, GLenum)                                                                                         \
    X(GLuint, CreateShader, GLenum)                                                                                        \
    X(void, ShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)                                             \
    X(void, CompileShader, GLuint)                                                                                         \
    X(void, GetShaderiv, GLuint, GLenum, GLint*)                                                                           \
    X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                                          \
    X(void, DeleteShader, GLuint)                                                                                          \
    X(GLuint, CreateProgram)                                                                                               \
    X(void, AttachShader, GLuint, GLuint)                                                                                  \
    X(void, LinkProgram, GLuint)                                                                                           \
    X(void, GetProgramiv, GLuint, GLenum, GLint*)                                                                          \
    X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                                         \
    X(void, DeleteProgram, GLuint)                                                                                         \
    X(void, UseProgram, GLuint)                                                                                            \
    X(GLint, GetUniformLocation, GLuint, const GLchar*)                                                                    \
    X(void, Uniform1i, GLint, GLint)                                                                                       \
    X(void, Uniform1f, GLint, GLfloat)                                                                                     \
    X(void, Uniform2f, GLint, GLfloat, GLfloat)                                                                            \
    X(void, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat)                                                          \
    X(void, GenVertexArrays, GLsizei, GLuint*)                                                                             \
    X(void, BindVertexArray, GLuint)                                                                                       \
    X(void, DeleteVertexArrays, GLsizei, const GLuint*)                                                                    \
    X(void, GenFramebuffers, GLsizei, GLuint*)                                                                             \
    X(void, DeleteFramebuffers, GLsizei, const GLuint*)                                                                    \
    X(void, BindFramebuffer, GLenum, GLuint)                                                                               \
    X(void, FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)                                                   \
    X(GLenum, CheckFramebufferStatus, GLenum)

struct Api {
#define FREETVC_GL_MEMBER(ret, name, ...) ret(FREETVC_GLAPI* name)(__VA_ARGS__) = nullptr;
    FREETVC_GL_FUNCTIONS(FREETVC_GL_MEMBER)
#undef FREETVC_GL_MEMBER

    // With a current context; false (missing set to the first absent function) when one is not there
    bool load(const char*& missing) {
#define FREETVC_GL_LOAD(ret, name, ...)                                                                                    \
    name = reinterpret_cast<ret(FREETVC_GLAPI*)(__VA_ARGS__)>(SDL_GL_GetProcAddress("gl" #name));                          \
    if (!name) {                                                                                                           \
        missing = "gl" #name;                                                                                              \
        return false;                                                                                                      \
    }
        FREETVC_GL_FUNCTIONS(FREETVC_GL_LOAD)
#undef FREETVC_GL_LOAD
        return true;
    }
};

}  // namespace freetvc::sdl::gl
