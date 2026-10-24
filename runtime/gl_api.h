/* Minimal EGL + OpenGL ES 2.0/3.0 declarations, so neither the Android build
 * (Termux has no Khronos headers) nor the Linux test build needs GL headers.
 * Functions resolve against the system libEGL / libGLESv2. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLNativeWindowType, *EGLNativeDisplayType;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean, EGLenum;
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NONE 0x3038
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_SURFACE_TYPE 0x3033
#define EGL_WINDOW_BIT 0x0004
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
EGLDisplay eglGetDisplay(EGLNativeDisplayType);
EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean eglBindAPI(EGLenum);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface, EGLint, EGLint *);
EGLBoolean eglSwapInterval(EGLDisplay, EGLint);
EGLint eglGetError(void);

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat, GLclampf;
typedef char GLchar;
typedef unsigned char GLboolean, GLubyte;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_ZERO 0
#define GL_ONE 1
#define GL_NEVER 0x0200
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE0 0x84C0
#define GL_MAX_VERTEX_ATTRIBS 0x8869
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
void glActiveTexture(GLenum);
void glAttachShader(GLuint, GLuint);
void glBindAttribLocation(GLuint, GLuint, const GLchar *);
void glBindBuffer(GLenum, GLuint);
void glBindFramebuffer(GLenum, GLuint);
void glBindRenderbuffer(GLenum, GLuint);
void glBindTexture(GLenum, GLuint);
void glBlendEquation(GLenum);
void glBlendFunc(GLenum, GLenum);
void glBufferData(GLenum, GLsizeiptr, const void *, GLenum);
GLenum glCheckFramebufferStatus(GLenum);
void glClear(GLbitfield);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClearDepthf(GLfloat);
void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean);
void glCompileShader(GLuint);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum);
void glCullFace(GLenum);
void glDeleteShader(GLuint);
void glDeleteTextures(GLsizei, const GLuint *);
void glDepthFunc(GLenum);
void glDepthMask(GLboolean);
void glDisable(GLenum);
void glDisableVertexAttribArray(GLuint);
void glDrawArrays(GLenum, GLint, GLsizei);
void glEnable(GLenum);
void glEnableVertexAttribArray(GLuint);
void glFinish(void);
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
void glFrontFace(GLenum);
void glGenBuffers(GLsizei, GLuint *);
void glGenFramebuffers(GLsizei, GLuint *);
void glGenRenderbuffers(GLsizei, GLuint *);
void glGenTextures(GLsizei, GLuint *);
GLenum glGetError(void);
void glGetIntegerv(GLenum, GLint *);
void glGetProgramInfoLog(GLuint, GLsizei, GLsizei *, GLchar *);
void glGetProgramiv(GLuint, GLenum, GLint *);
void glGetShaderInfoLog(GLuint, GLsizei, GLsizei *, GLchar *);
void glGetShaderiv(GLuint, GLenum, GLint *);
const GLubyte *glGetString(GLenum);
GLint glGetUniformLocation(GLuint, const GLchar *);
void glLinkProgram(GLuint);
void glPixelStorei(GLenum, GLint);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
void glScissor(GLint, GLint, GLsizei, GLsizei);
void glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void glTexParameteri(GLenum, GLenum, GLint);
void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
void glUniform1i(GLint, GLint);
void glUniform2f(GLint, GLfloat, GLfloat);
void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
void glUniform4fv(GLint, GLsizei, const GLfloat *);
void glUseProgram(GLuint);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
void glViewport(GLint, GLint, GLsizei, GLsizei);
