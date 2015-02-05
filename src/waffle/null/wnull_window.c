// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <drm_fourcc.h>

#include "wcore_attrib_list.h"
#include "wcore_error.h"
#include "wcore_window.h"

#include "wegl_platform.h"
#include "wegl_util.h"

#include "wgbm_config.h"

#include "wnull_context.h"
#include "wnull_display.h"
#include "wnull_window.h"

#define ARRAY_END(a) ((a)+sizeof(a)/sizeof((a)[0]))

#if 0
#include <stdio.h>
#define prt(...) fprintf(stderr, __VA_ARGS__)
#else
#define prt(...)
#endif

enum window_part {
    WINDOW_PART_COLOR   = 0x01,
    WINDOW_PART_DEPTH   = 0x02,
    WINDOW_PART_STENCIL = 0x04,
};

struct window_buffer {
    struct wnull_display_buffer *dpy_buf;
    EGLImageKHR image;
    GLuint fb;
    GLuint color;
    GLuint depth_stencil;
};

struct wnull_window {
    uint32_t width;
    uint32_t height;
    unsigned parts;
    bool show;
    intptr_t show_method;
    uint32_t gbm_format;
    uint32_t drm_format;
    GLenum depth_stencil_format;
    struct window_buffer buffer[3];
    struct window_buffer *current;
    struct wcore_window wcore;
};

struct wnull_window*
wnull_window(struct wcore_window *wcore_self)
{
    if (wcore_self)
        return container_of(wcore_self, struct wnull_window, wcore);
    else
        return 0;
}

static void
window_buffer_destroy_fb(struct window_buffer *self,
                         struct wnull_context *ctx)
{
    prt("destroy fb %u\n",self->fb);
    ctx->glDeleteRenderbuffers(1, &self->color);
    ctx->glDeleteRenderbuffers(1, &self->depth_stencil);
    ctx->glDeleteFramebuffers(1, &self->fb);
    self->fb = 0;
}

// If there is a GL FB, 'ctx' is the context used to create it, otherwise
// 'ctx' is NULL.
static void
window_buffer_teardown(struct window_buffer *self,
                       struct wnull_display *dpy,
                       struct wnull_context *ctx)
{
    struct wegl_platform *plat = wegl_platform(dpy->wegl.wcore.platform);

    if (self->fb) {
        assert(ctx);
        window_buffer_destroy_fb(self, ctx);
    }
    if (self->image != EGL_NO_IMAGE_KHR) {
        plat->eglDestroyImageKHR(dpy->wegl.egl, self->image);
        self->image = EGL_NO_IMAGE_KHR;
    }
    wnull_display_buffer_destroy(self->dpy_buf);
    self->dpy_buf = 0;
}

#define GLCHECK { \
    GLenum e = ctx->glGetError(); \
    if (e != GL_NO_ERROR) { \
        prt(stderr, "gl error %x @ line %d\n", (int)e, __LINE__); \
        wcore_errorf(WAFFLE_ERROR_UNKNOWN, "gl error %x @ line %d\n", (int)e, __LINE__); \
        goto gl_error; \
    } \
}

static bool
wnull_window_prepare_current_buffer(struct wnull_window *self,
                                    struct wnull_display *dpy)
{
    if (!self->parts)
        return true;

    struct wegl_platform *plat = wegl_platform(dpy->wegl.wcore.platform);
    struct wnull_context *ctx = dpy->current_context;
    assert(ctx);
    GLint save_fb = -1;
    GLint save_rb = -1;

    if (!self->current)
        self->current = self->buffer;

    if (!self->current->dpy_buf)
        self->current->image = EGL_NO_IMAGE_KHR;

    if (!self->current->dpy_buf && (self->parts & WINDOW_PART_COLOR)) {

        uint32_t flags = GBM_BO_USE_RENDERING;
        // This flag should only be set when self->show == true and
        // self->show_method == FLIP, but set it unconditionally because:
        // 1) When show_method == COPY the buffer copy function
        // may not handle the case of copying non-scanout to scanout
        // due to different tiling.
        // 2) If show == false it may be that waffle_window_show()
        // hasn't been called yet.  The waffle api seems to allow calling
        // it after waffle_make_current() but that is too late for us so
        // set this now just in case we need it.
        flags |= GBM_BO_USE_SCANOUT;

        self->current->dpy_buf = wnull_display_buffer_create(dpy,
                                                             self->width,
                                                             self->height,
                                                             self->gbm_format,
                                                             flags,
                                                             ctx->glFinish);
        if (!self->current->dpy_buf) {
            wcore_errorf(WAFFLE_ERROR_UNKNOWN, "display buffer create failed");
            goto buf_error;
        }

        int dmabuf_fd;
        uint32_t stride;
        if (!wnull_display_buffer_dmabuf(self->current->dpy_buf,
                                         &dmabuf_fd,
                                         &stride)) {
            wcore_errorf(WAFFLE_ERROR_UNKNOWN, "dmabuf failed");
            goto buf_error;
        }

        EGLint attr[] = {
            EGL_WIDTH, self->width,
            EGL_HEIGHT, self->height,
            EGL_LINUX_DRM_FOURCC_EXT, self->drm_format,
            EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
            EGL_NONE,
        };
        self->current->image = plat->eglCreateImageKHR(dpy->wegl.egl,
                                                       EGL_NO_CONTEXT,
                                                       EGL_LINUX_DMA_BUF_EXT,
                                                       NULL,
                                                       attr);
        if (self->current->image == EGL_NO_IMAGE_KHR) {
            wcore_errorf(WAFFLE_ERROR_UNKNOWN, "eglCreateImageKHR failed");
            goto buf_error;
        }
    }

    ctx->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &save_fb);
    GLCHECK
    ctx->glGetIntegerv(GL_RENDERBUFFER_BINDING, &save_rb);
    GLCHECK

    if (self->current->fb) {
        prt("use fb %u color %u depth/stencil %u\n", self->current->fb, self->current->color, self->current->depth_stencil);
        ctx->glBindFramebuffer(GL_FRAMEBUFFER, self->current->fb);
        GLCHECK
    } else {
        ctx->glGenFramebuffers(1, &self->current->fb);
        GLCHECK
        ctx->glBindFramebuffer(GL_FRAMEBUFFER, self->current->fb);
        GLCHECK

        if (self->parts & WINDOW_PART_COLOR) {
            ctx->glGenRenderbuffers(1, &self->current->color);
            GLCHECK
            ctx->glBindRenderbuffer(GL_RENDERBUFFER, self->current->color);
            GLCHECK
            ctx->glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
                                                        self->current->image);
            GLCHECK
            ctx->glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                           GL_COLOR_ATTACHMENT0_EXT,
                                           GL_RENDERBUFFER,
                                           self->current->color);
            GLCHECK
        }

        if (self->parts & (WINDOW_PART_DEPTH | WINDOW_PART_STENCIL)) {
            ctx->glGenRenderbuffers(1, &self->current->depth_stencil);
            GLCHECK
            ctx->glBindRenderbuffer(GL_RENDERBUFFER, self->current->depth_stencil);
            GLCHECK
            ctx->glRenderbufferStorage(GL_RENDERBUFFER,
                                       self->depth_stencil_format,
                                       self->width,
                                       self->height);
            GLCHECK
        }

        if (self->parts & WINDOW_PART_DEPTH) {
            ctx->glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                           GL_DEPTH_ATTACHMENT,
                                           GL_RENDERBUFFER,
                                           self->current->depth_stencil);
            GLCHECK
        }

        if (self->parts & WINDOW_PART_STENCIL) {
            ctx->glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                           GL_STENCIL_ATTACHMENT,
                                           GL_RENDERBUFFER,
                                           self->current->depth_stencil);
            GLCHECK
        }

        prt("set up fb %u color %u depth/stencil %u\n", self->current->fb, self->current->color, self->current->depth_stencil);
        GLenum s = ctx->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        GLCHECK
        if (s != GL_FRAMEBUFFER_COMPLETE) {
            prt(stderr, "incomplete fb\n");
            goto gl_error;
        }
    }

    return true;

buf_error:
    window_buffer_teardown(self->current, dpy, ctx);

gl_error:
    if (save_fb >= 0)
        ctx->glBindFramebuffer(GL_FRAMEBUFFER, save_fb);
    if (save_rb >= 0)
        ctx->glBindFramebuffer(GL_RENDERBUFFER, save_rb);
    return false;
}

bool
wnull_window_destroy(struct wcore_window *wc_self)
{
    if (!wc_self)
        return true;

    struct wnull_window *self = wnull_window(wc_self);
    prt("window destroy %p\n", self);
    if (wc_self->display) {
        struct wnull_display *dpy = wnull_display(wc_self->display);
        struct window_buffer *buf;
        for (buf = self->buffer; buf != ARRAY_END(self->buffer); ++buf) {
            // If the buffer has a GL FB, that FB must exist in the current
            // context, because when contexts cease to be current we destroy
            // all their GL FBs.
            window_buffer_teardown(buf,
                                   dpy,
                                   buf->fb ? dpy->current_context : NULL);
        }

        // tell the display this window is gone
        wnull_display_clean(dpy, NULL, self);
    }

    free(self);
    return true;
}

static void
wnull_window_destroy_fbs(struct wnull_window *self,
                         struct wnull_context *ctx)
{
    struct window_buffer *buf;
    for (buf = self->buffer; buf != ARRAY_END(self->buffer); ++buf)
        window_buffer_destroy_fb(buf, ctx);
}

struct wcore_window*
wnull_window_create(struct wcore_platform *wc_plat,
                    struct wcore_config *wc_config,
                    int32_t width,
                    int32_t height,
                    const intptr_t attrib_list[])
{

    struct wnull_window *window = wcore_calloc(sizeof(*window));
    if (!window)
        return NULL;

#if 0
    //XXX EGL_PLATFORM_NULL is not providing EGL_NATIVE_VISUAL_ID
    // so we can't use this yet.
    window->gbm_format = wgbm_config_get_gbm_format(wc_plat,
                                                    wc_config->display,
                                                    wc_config);
#else
    if (wc_config->attrs.alpha_size <= 0)
        window->gbm_format = GBM_FORMAT_XRGB8888;
    else if (wc_config->attrs.alpha_size <= 8)
        window->gbm_format = GBM_FORMAT_ARGB8888;
    else {
        wcore_errorf(WAFFLE_ERROR_UNKNOWN, "unexpected alpha size");
        goto error;
    }
#endif

    switch(window->gbm_format) {
        case GBM_FORMAT_XRGB8888:
            window->drm_format = DRM_FORMAT_XRGB8888;
            break;
        case GBM_FORMAT_ARGB8888:
            window->drm_format = DRM_FORMAT_ARGB8888;
            break;
        case GBM_FORMAT_XRGB2101010:
            window->drm_format = DRM_FORMAT_XRGB2101010;
            break;
        case GBM_FORMAT_ARGB2101010:
            window->drm_format = DRM_FORMAT_ARGB2101010;
            break;
        case GBM_FORMAT_RGB565:
            window->drm_format = DRM_FORMAT_RGB565;
            break;
        default:
            wcore_errorf(WAFFLE_ERROR_UNKNOWN,
                         "unexpected gbm format %x",
                         window->gbm_format);
            goto error;
    }

    if (width == -1 && height ==-1)
        wnull_display_get_size(wnull_display(wc_config->display),
                               &width, &height);
    window->width = width;
    window->height = height;

    if (!wcore_attrib_list_get(attrib_list,
                               WAFFLE_WINDOW_NULL_SHOW_METHOD,
                               &window->show_method))
        window->show_method = WAFFLE_WINDOW_NULL_SHOW_METHOD_COPY;

    window->parts = 0;
    if (wc_config->attrs.rgba_size > 0)
        window->parts |= WINDOW_PART_COLOR;
    if (wc_config->attrs.depth_size > 0)
        window->parts |= WINDOW_PART_DEPTH;
    if (wc_config->attrs.stencil_size > 0)
        window->parts |= WINDOW_PART_STENCIL;

    if (wc_config->attrs.stencil_size)
        window->depth_stencil_format = GL_DEPTH24_STENCIL8_OES;
    else if (wc_config->attrs.depth_size <= 16)
        window->depth_stencil_format = GL_DEPTH_COMPONENT16;
    else if (wc_config->attrs.depth_size <= 24)
        window->depth_stencil_format = GL_DEPTH_COMPONENT24_OES;
    else
        window->depth_stencil_format = GL_DEPTH_COMPONENT32_OES;

    if (wcore_window_init(&window->wcore, wc_config))
        return &window->wcore;

error:
    wnull_window_destroy(&window->wcore);
    return NULL;
}

bool
wnull_window_show(struct wcore_window *wc_self)
{
    struct wnull_window *self = wnull_window(wc_self);
    self->show = true;
    return true;
}

bool
wnull_window_swap_buffers(struct wcore_window *wc_self)
{
    struct wnull_window *self = wnull_window(wc_self);
    struct wnull_display *dpy = wnull_display(wc_self->display);
    int ok = true;

    assert(self->current);
    assert(self->current->fb);
    GLint cur_fb;
    dpy->current_context->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur_fb);
    // if the user has bound their own framebuffer, bail out
    if (cur_fb && cur_fb != self->current->fb)
        return true;

    if (self->show && (self->parts & WINDOW_PART_COLOR)) {
        assert(self->current->dpy_buf);
        if (self->show_method == WAFFLE_WINDOW_NULL_SHOW_METHOD_FLIP)
            ok &= wnull_display_show_buffer(dpy, self->current->dpy_buf);
        else if (self->show_method == WAFFLE_WINDOW_NULL_SHOW_METHOD_COPY)
            ok &= wnull_display_copy_buffer(dpy, self->current->dpy_buf);
    }

    if (ok) {
        ++self->current;
        if (self->current == ARRAY_END(self->buffer))
            self->current = self->buffer;
        ok &= wnull_window_prepare_current_buffer(self, dpy);
    }

    return ok;
}

bool
wnull_make_current(struct wcore_platform *wc_plat,
                   struct wcore_display *wc_dpy,
                   struct wcore_window *wc_window,
                   struct wcore_context *wc_ctx)
{
    struct wegl_platform *plat = wegl_platform(wc_plat);
    struct wnull_display *dpy = wnull_display(wc_dpy);
    struct wnull_context *ctx = wnull_context(wc_ctx);
    struct wnull_context *old_ctx = dpy->current_context;
    struct wnull_window *win = wnull_window(wc_window);

    bool first; // first time the context/window pair will be current?
    struct wnull_window **old_win = NULL; // list of windows in old context
    if (!wnull_display_make_current(dpy, ctx, win, &first, &old_win))
        return false;

    // When the current context is changed to a different one we must
    // delete any framebuffers created in the first context as it may
    // not be seen again.
    if (old_ctx && old_ctx != ctx && old_win)
        for (struct wnull_window **w = old_win; *w; ++w)
            wnull_window_destroy_fbs(*w, old_ctx);
    free(old_win);

    if (!plat->eglMakeCurrent(dpy->wegl.egl,
                              EGL_NO_SURFACE,
                              EGL_NO_SURFACE,
                              ctx ? ctx->wegl.egl : EGL_NO_CONTEXT)) {
        wegl_emit_error(plat, "eglMakeCurrent");
        return false;
    }

    bool ok = true;
    if (ctx && win) {
        ok = wnull_window_prepare_current_buffer(win, dpy);
        if (ok && first) {
            prt("setting viewport\n");
            ctx->glViewport(0, 0, win->width, win->height);
            ctx->glScissor(0, 0, win->width, win->height);
        }
    }

    return ok;
}

union waffle_native_window*
wnull_window_get_native(struct wcore_window *wc_self)
{
    struct wnull_window *self = wnull_window(wc_self);
    struct wnull_display *dpy = wnull_display(wc_self->display);
    union waffle_native_window *n_window;

    WCORE_CREATE_NATIVE_UNION(n_window, null);
    if (n_window == NULL)
        return NULL;

    wnull_display_fill_native(dpy, &n_window->null->display);
    n_window->null->width = self->width;
    n_window->null->height = self->height;

    return n_window;
}
