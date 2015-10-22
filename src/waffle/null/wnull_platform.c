// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

#include "api_priv.h"

#include "linux_platform.h"

#include "wcore_error.h"

#include "wegl_config.h"
#include "wegl_context.h"
#include "wegl_platform.h"
#include "wegl_util.h"

#include "wgbm_config.h"
#include "wgbm_platform.h"

#include "wnull_context.h"
#include "wnull_display.h"
#include "wnull_platform.h"
#include "wnull_window.h"

static const struct wcore_platform_vtbl wnull_platform_vtbl;

struct wcore_platform*
wnull_platform_create(void)
{
    struct wnull_platform *self = wcore_calloc(sizeof(*self));
    if (self == NULL)
        return NULL;

    if (!wgbm_platform_init(&self->wgbm) ||
        !self->wgbm.wegl.eglCreateImageKHR ||
        !self->wgbm.wegl.eglDestroyImageKHR) {
        wgbm_platform_destroy(&self->wgbm.wegl.wcore);
        return NULL;
    }

    setenv("EGL_PLATFORM", "surfaceless", true);

    self->wgbm.wegl.wcore.vtbl = &wnull_platform_vtbl;
    return &self->wgbm.wegl.wcore;
}

static bool
wnull_platform_destroy(struct wcore_platform *wc_self)
{
    struct wnull_platform *self =
        wnull_platform(wgbm_platform(wegl_platform(wc_self)));

    if (!self)
        return true;

    bool ok = wgbm_platform_teardown(&self->wgbm);
    free(self);
    return ok;
}

static union waffle_native_context*
wnull_context_get_native(struct wcore_context *wc_ctx)
{
    struct wnull_display *dpy = wnull_display(wc_ctx->display);
    struct wegl_context *ctx = wegl_context(wc_ctx);
    union waffle_native_context *n_ctx;

    WCORE_CREATE_NATIVE_UNION(n_ctx, null);
    if (!n_ctx)
        return NULL;

    wnull_display_fill_native(dpy, &n_ctx->null->display);
    n_ctx->null->egl_context = ctx->egl;

    return n_ctx;
}

static struct wnull_display*
current_display()
{
    struct wnull_platform *plat =
        wnull_platform(wgbm_platform(wegl_platform(api_platform)));
    assert(plat);
    struct wnull_display *dpy = plat->current_display;
    assert(dpy);
    return dpy;
}

static void
BindFramebuffer(GLenum target, GLuint framebuffer)
{
    struct wnull_display *dpy = current_display();

    if (dpy->current_context) {
        dpy->current_context->glBindFramebuffer(target, framebuffer);
        if (framebuffer) {
            dpy->user_fb = true;
        } else {
            dpy->user_fb = false;
            if (dpy->current_window)
                wnull_window_prepare_draw_buffer(dpy->current_window);
        }
    }
}

static void
FramebufferTexture2D(GLenum target,
                     GLenum attachment,
                     GLenum textarget,
                     GLuint texture,
                     GLint level)
{
    struct wnull_display *dpy = current_display();

    if (!dpy->current_context)
        return;
    if (!dpy->user_fb) {
        printf("don't call glFramebufferTexture2D on framebuffer 0\n");
        // ideally we would generate a GL_INVALID_OPERATION error here
        return;
    }
    dpy->current_context->glFramebufferTexture2D(target,
                                                 attachment,
                                                 textarget,
                                                 texture,
                                                 level);
}

static void
FramebufferRenderbuffer(GLenum target,
                        GLenum attachment,
                        GLenum renderbuffertarget,
                        GLuint renderbuffer)
{
    struct wnull_display *dpy = current_display();

    if (!dpy->current_context)
        return;
    if (!dpy->user_fb) {
        printf("don't call glFramebufferRenderbuffer on framebuffer 0\n");
        // ideally we would generate a GL_INVALID_OPERATION error here
        return;
    }
    dpy->current_context->glFramebufferRenderbuffer(target,
                                                    attachment,
                                                    renderbuffertarget,
                                                    renderbuffer);
}

static bool
wnull_dl_can_open(struct wcore_platform *wc_self,
                 int32_t waffle_dl)
{
    // for now platform null is limited to gles2
    struct wgbm_platform *self = wgbm_platform(wegl_platform(wc_self));
    return waffle_dl == WAFFLE_DL_OPENGL_ES2 &&
        linux_platform_dl_can_open(self->linux, waffle_dl);
}

static void*
wnull_dl_sym(struct wcore_platform *wc_self,
            int32_t waffle_dl,
            const char *name)
{
    // for now platform null is limited to gles2
    if (waffle_dl != WAFFLE_DL_OPENGL_ES2)
        return NULL;

    // Intercept glBindFramebuffer(target, 0) so it restores framebuffer
    // operations to the null platform framebuffer.
    if (!strcmp(name, "glBindFramebuffer"))
        return BindFramebuffer;

    // Intercept these because a program that calls them on framebuffer 0
    // can mess up our framebuffers.  It's a GL_INVALID_OPERATION error to
    // call these on framebuffer 0 but some programs might do it.
    if (!strcmp(name, "glFramebufferTexture2D"))
        return FramebufferTexture2D;
    if (!strcmp(name, "glFramebufferRenderbuffer"))
        return FramebufferRenderbuffer;

    struct wgbm_platform *plat = wgbm_platform(wegl_platform(wc_self));
    return linux_platform_dl_sym(plat->linux, waffle_dl, name);
}

static const struct wcore_platform_vtbl wnull_platform_vtbl = {
    .destroy = wnull_platform_destroy,

    .make_current = wnull_make_current,
    .get_proc_address = wegl_get_proc_address,
    .dl_can_open = wnull_dl_can_open,
    .dl_sym = wnull_dl_sym,

    .display = {
        .connect = wnull_display_connect,
        .destroy = wnull_display_destroy,
        .supports_context_api = wnull_display_supports_context_api,
        .get_native = wnull_display_get_native,
    },

    .config = {
        .choose = wegl_config_choose,
        .destroy = wegl_config_destroy,
        .get_native = wgbm_config_get_native,
    },

    .context = {
        .create = wnull_context_create,
        .destroy = wnull_context_destroy,
        .get_native = wnull_context_get_native,
    },

    .window = {
        .create = wnull_window_create,
        .destroy = wnull_window_destroy,
        .show = wnull_window_show,
        .swap_buffers = wnull_window_swap_buffers,
        .get_native = wnull_window_get_native,
    },
};
