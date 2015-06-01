// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "wcore_util.h"
#include "wegl_context.h"

#define GL_FUNCTIONS(f) \
f(void  , glActiveTexture                       , (GLenum texture)) \
f(void  , glAttachShader                        , (GLuint program, GLuint shader)) \
f(void  , glBindAttribLocation                  , (GLuint program, GLuint index, const GLchar *name))\
f(void  , glBindBuffer                          , (GLenum target, GLuint buffer)) \
f(void  , glBindFramebuffer                     , (GLenum target, GLuint framebuffer)) \
f(void  , glBindRenderbuffer                    , (GLenum target, GLuint renderbuffer)) \
f(void  , glBindTexture                         , (GLenum target, GLuint texture)) \
f(void  , glBlendFunc                           , (GLenum sfactor, GLenum dfactor)) \
f(void  , glBufferData                          , (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
f(GLenum, glCheckFramebufferStatus              , (GLenum target)) \
f(void  , glClearColor                          , (GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)) \
f(void  , glClear                               , (GLbitfield mask)) \
f(void  , glColorMask                           , (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)) \
f(void  , glCompileShader                       , (GLuint shader)) \
f(void  , glCopyTexImage2D                      , (GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border)) \
f(GLuint, glCreateProgram                       , (void)) \
f(GLuint, glCreateShader                        , (GLenum type)) \
f(void  , glDeleteBuffers                       , (GLsizei n, const GLuint *buffers)) \
f(void  , glDeleteFramebuffers                  , (GLsizei n, const GLuint *framebuffers)) \
f(void  , glDeleteProgram                       , (GLuint program)) \
f(void  , glDeleteRenderbuffers                 , (GLsizei n, const GLuint *framebuffers)) \
f(void  , glDeleteShader                        , (GLuint shader)) \
f(void  , glDeleteTextures                      , (GLsizei n, const GLuint *textures)) \
f(void  , glDisable                             , (GLenum cap)) \
f(void  , glDisableVertexAttribArray            , (GLuint index)) \
f(void  , glDrawArrays                          , (GLenum mode, GLint first, GLsizei count)) \
f(void  , glEGLImageTargetRenderbufferStorageOES, (GLenum target, GLeglImageOES image)) \
f(void  , glEGLImageTargetTexture2DOES          , (GLenum target, GLeglImageOES image)) \
f(void  , glEnable                              , (GLenum cap)) \
f(void  , glEnableVertexAttribArray             , (GLuint index)) \
f(void  , glFinish                              , ()) \
f(void  , glFlush                               , ()) \
f(void  , glFramebufferRenderbuffer             , (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)) \
f(void  , glFramebufferTexture2D                , (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
f(void  , glGenBuffers                          , (GLsizei n, GLuint *buffers)) \
f(void  , glGenFramebuffers                     , (GLsizei n, GLuint *framebuffers)) \
f(void  , glGenRenderbuffers                    , (GLsizei n, GLuint *renderbuffers)) \
f(void  , glGenTextures                         , (GLsizei n, GLuint *textures)) \
f(GLenum, glGetError                            , ()) \
f(void  , glGetFloatv                           , (GLenum pname, GLfloat *data)) \
f(void  , glGetFramebufferAttachmentParameteriv , (GLenum target, GLenum attachment, GLenum pname, GLint *params)) \
f(void  , glGetIntegerv                         , (GLenum pname, GLint *data)) \
f(void  , glGetProgramiv                        , (GLuint program, GLenum pname, GLint *params)) \
f(void  , glGetShaderInfoLog                    , (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
f(void  , glGetShaderiv                         , (GLuint shader, GLenum pname, GLint *params)) \
f(void  , glGetTexParameteriv                   , (GLenum target, GLenum pname, GLint *params)) \
f(GLint , glGetUniformLocation                  , (GLuint program, const GLchar *name)) \
f(void  , glGetVertexAttribiv                   , (GLuint index, GLenum pname, GLint *params)) \
f(void  , glGetVertexAttribPointerv             , (GLuint index, GLenum pname, void **pointer)) \
f(void  , glLinkProgram                         , (GLuint program)) \
f(void  , glRenderbufferStorage                 , (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)) \
f(void  , glScissor                             , (GLint x, GLint y, GLsizei width, GLsizei height)) \
f(void  , glShaderSource                        , (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length)) \
f(void  , glTexImage2D                          , (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)) \
f(void  , glTexParameteri                       , (GLenum target, GLenum pname, GLint param)) \
f(void  , glUniform1i                           , (GLint location, GLint v0)) \
f(void  , glUseProgram                          , (GLuint program)) \
f(void  , glVertexAttribPointer                 , (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
f(void  , glViewport                            , (GLint x, GLint y, GLsizei width, GLsizei height)) \


struct wnull_context {
    struct wegl_context wegl;
#define DECLARE(type, name, args) type (*name) args;
    GL_FUNCTIONS(DECLARE)
#undef DECLARE
};

static inline struct wnull_context*
wnull_context(struct wcore_context *wc_self)
{
    if (wc_self) {
        struct wegl_context *wegl_self = container_of(wc_self, struct wegl_context, wcore);
        return container_of(wegl_self, struct wnull_context, wegl);
    } else {
        return NULL;
    }
}

struct wcore_context*
wnull_context_create(struct wcore_platform *wc_plat,
                     struct wcore_config *wc_config,
                     struct wcore_context *wc_share_ctx);

bool
wnull_context_destroy(struct wcore_context *wc_ctx);
