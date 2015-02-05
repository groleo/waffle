// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <stdbool.h>

#include "wcore_util.h"
#include "wegl_context.h"

typedef void *EGLImageKHR;
typedef void *GLeglImageOES;
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;

#define WNULL_GL_FUNCTIONS(f) \
    f(void  , glBindFramebuffer                     , (GLenum target, GLuint framebuffer)) \
    f(void  , glBindRenderbuffer                    , (GLenum target, GLuint renderbuffer)) \
    f(GLenum, glCheckFramebufferStatus              , (GLenum target)) \
    f(void  , glDeleteFramebuffers                  , (GLsizei n, const GLuint *framebuffers)) \
    f(void  , glDeleteRenderbuffers                 , (GLsizei n, const GLuint *framebuffers)) \
    f(void  , glEGLImageTargetRenderbufferStorageOES, (GLenum target, GLeglImageOES image)) \
    f(void  , glFinish                              , ()) \
    f(void  , glFramebufferRenderbuffer             , (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)) \
    f(void  , glGenFramebuffers                     , (GLsizei n, GLuint *framebuffers)) \
    f(void  , glGenRenderbuffers                    , (GLsizei n, GLuint *renderbuffers)) \
    f(GLenum, glGetError                            , ()) \
    f(void  , glGetIntegerv                         , (GLenum pname, GLint *data)) \
    f(void  , glRenderbufferStorage                 , (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)) \
    f(void  , glScissor                             , (GLint x, GLint y, GLsizei width, GLsizei height)) \
    f(void  , glViewport                            , (GLint x, GLint y, GLsizei width, GLsizei height))

struct wnull_context {
    struct wegl_context wegl;
#define DECLARE(type, name, args) type (*name) args;
    WNULL_GL_FUNCTIONS(DECLARE)
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
