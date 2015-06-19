// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#include <dlfcn.h>

#include "linux_platform.h"

#include "wcore_error.h"

#include "wgbm_platform.h"

#include "wnull_context.h"
#include "wnull_display.h"
#include "wnull_window.h"

#if 0
#include <stdio.h>
#define prt(...) fprintf(stderr, __VA_ARGS__)
#else
#define prt(...)
#endif

struct wcore_context*
wnull_context_create(struct wcore_platform *wc_plat,
                     struct wcore_config *wc_config,
                     struct wcore_context *wc_share_ctx)
{
    struct wgbm_platform *plat = wgbm_platform(wegl_platform(wc_plat));
    struct wnull_context *ctx = wcore_calloc(sizeof(*ctx));
    if (!ctx)
        return NULL;

    if (wc_config->attrs.samples > 0) {
        wcore_errorf(WAFFLE_ERROR_BAD_ATTRIBUTE,
                     "WAFFLE_PLATFORM_NULL does not support samples");
        goto fail;
    }

    if (wc_config->attrs.sample_buffers) {
        wcore_errorf(WAFFLE_ERROR_BAD_ATTRIBUTE,
                     "WAFFLE_PLATFORM_NULL does not support sample buffers");
        goto fail;
    }

    int32_t dl;
    switch (wc_config->attrs.context_api) {
        //XXX could some other APIs work?
        case WAFFLE_CONTEXT_OPENGL_ES2:
            dl = WAFFLE_DL_OPENGL_ES2;
            break;
        default:
            wcore_errorf(WAFFLE_ERROR_BAD_ATTRIBUTE,
                         "WAFFLE_PLATFORM_NULL api must be GLES2");
            goto fail;
    }

    bool ok = true;

#define LOOKUP(type, name, args) \
    ctx->name = linux_platform_dl_sym(plat->linux, dl, #name); \
    if (!ctx->name) \
        ctx->name = (void*)plat->wegl.eglGetProcAddress(#name); \
    ok &= ctx->name != NULL;
    GL_FUNCTIONS(LOOKUP)
#undef LOOKUP

    ok &= wegl_context_init(&ctx->wegl, wc_config, wc_share_ctx);
    if (!ok)
        goto fail;

    prt("create context %p\n", ctx);
    return &ctx->wegl.wcore;

fail:
    wnull_context_destroy(&ctx->wegl.wcore);
    return NULL;
}

bool
wnull_context_destroy(struct wcore_context *wc_ctx)
{
    bool result = true;

    if (wc_ctx) {
        struct wnull_context *self = wnull_context(wc_ctx);
        struct wnull_display *dpy = wnull_display(wc_ctx->display);
        prt("destroy context %p\n", self);

        if (self == dpy->current_context) {
            prt("destroying current context!  you suck!\n", self);
            wnull_make_current(wc_ctx->display->platform,
                               wc_ctx->display,
                               NULL, NULL);
        }

        result = wegl_context_teardown(&self->wegl);

        // tell the display this context is gone
        wnull_display_clean(dpy, self, NULL);

        free(self);
    }
    return result;
}
