// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#define _POSIX_C_SOURCE 200112 // glib feature macro for unsetenv()

#include <stdlib.h>
#include <dlfcn.h>

#include "wcore_error.h"

#include "linux_platform.h"

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
    struct wgbm_platform *self = wcore_calloc(sizeof(*self));
    if (self == NULL)
        return NULL;

    if (!wgbm_platform_init(self) ||
        !self->wegl.eglCreateImageKHR ||
        !self->wegl.eglDestroyImageKHR) {
        wgbm_platform_destroy(&self->wegl.wcore);
        return NULL;
    }

    setenv("EGL_PLATFORM", "null", true);

    self->wegl.wcore.vtbl = &wnull_platform_vtbl;
    return &self->wegl.wcore;
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


static const struct wcore_platform_vtbl wnull_platform_vtbl = {
    .destroy = wgbm_platform_destroy,

    .make_current = wnull_make_current,
    .get_proc_address = wegl_get_proc_address,
    .dl_can_open = wgbm_dl_can_open,
    .dl_sym = wgbm_dl_sym,

    .display = {
        .connect = wnull_display_connect,
        .destroy = wnull_display_destroy,
        .supports_context_api = wegl_display_supports_context_api,
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
