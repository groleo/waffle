// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "wgbm_platform.h"
#include "wnull_context.h"

#define EGL_FUNCTIONS(f) \
f(EGLImageKHR, eglCreateImageKHR , (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list)) \
f(EGLBoolean , eglDestroyImageKHR, (EGLDisplay dpy, EGLImageKHR image)) \


struct slbuf_func {
#define DECLARE(type, name, args) type (*name) args;
    GBM_FUNCTIONS(DECLARE)
    EGL_FUNCTIONS(DECLARE)
    GL_FUNCTIONS(DECLARE)
#undef DECLARE
};

struct slbuf_param {
    uint32_t width;
    uint32_t height;
    bool color, depth, stencil;
    GLenum depth_stencil_format;

    struct gbm_device *gbm_device;
    uint32_t gbm_format;
    uint32_t gbm_flags;

    EGLDisplay egl_display;
};

struct wnull_display;
struct slbuf;

struct slbuf*
slbuf_get_buffer(struct slbuf *array[],
                 unsigned len,
                 struct slbuf_param *param,
                 struct slbuf_func *func);

void slbuf_destroy(struct slbuf *self);

void
slbuf_free_gl_resources(struct slbuf *self);

GLuint
slbuf_check_glfb(struct slbuf *self);

bool
slbuf_bind_fb(struct slbuf *self);

void
slbuf_set_display(struct slbuf *self, struct wnull_display *display);

bool
slbuf_copy_i915(struct slbuf *dst, struct slbuf *src);

bool
slbuf_copy_gl(struct slbuf *dst, struct slbuf *src);

void
slbuf_finish(struct slbuf *self);

void
slbuf_flush(struct slbuf *self);

bool
slbuf_get_drmfb(struct slbuf *self, uint32_t *fb);

uint32_t
slbuf_gbm_format(struct slbuf *self);

uint32_t
slbuf_drm_format(struct slbuf *self);
