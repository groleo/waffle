// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#include "wcore_attrib_list.h"
#include "wcore_error.h"
#include "wcore_window.h"

#include "wegl_platform.h"
#include "wegl_util.h"

#include "wgbm_config.h"

#include "wnull_buffer.h"
#include "wnull_context.h"
#include "wnull_display.h"
#include "wnull_platform.h"
#include "wnull_window.h"

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

#if 0
#include <stdio.h>
#define prt(...) fprintf(stderr, __VA_ARGS__)
#else
#define prt(...)
#endif

struct wnull_window {
    struct wcore_window wcore;
    bool show;
    intptr_t vsync_wait;

    struct slbuf_param param;
    struct slbuf_func func;

    struct slbuf *buf[3];
    struct slbuf *drawbuf;
    bool (*buf_copy)(struct slbuf*, struct slbuf*);
};

struct wnull_window*
wnull_window(struct wcore_window *wcore_self)
{
    if (wcore_self)
        return container_of(wcore_self, struct wnull_window, wcore);
    else
        return 0;
}

// True if the current fb is zero or one of ours.
static bool
wnull_window_system_fb(struct wnull_window *self)
{
    struct slbuf_func *f = &self->func;
    GLint current_fb;

    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fb);
    if (current_fb == 0)
        return true;

    for (unsigned i = 0; i < ARRAY_SIZE(self->buf); ++i)
        if (slbuf_check_glfb(self->buf[i]) == current_fb)
            return true;

    return false;
}

bool
wnull_window_prepare_draw_buffer(struct wnull_window *self)
{
    struct slbuf *draw = slbuf_get_buffer(self->buf,
                                          ARRAY_SIZE(self->buf),
                                          &self->param,
                                          &self->func);
    if (!draw)
        return false;

    // Don't bind our framebuffer unless the current binding is zero
    // (i.e. default/"window system") or one of our buffers.
    // This is not an error, so return true.
    // This is to avoid breaking user code which binds a framebuffer
    // and does not expect swap_buffers or make_current to change it.
    if (!wnull_window_system_fb(self))
        return true;

    if (!slbuf_bind_fb(draw))
        return false;

    self->drawbuf = draw;
    return true;
}

bool
wnull_window_destroy(struct wcore_window *wc_self)
{
    if (!wc_self)
        return true;

    struct wnull_window *self = wnull_window(wc_self);
    struct wnull_display *dpy = wnull_display(wc_self->display);

    for (int i = 0; i < ARRAY_SIZE(self->buf); ++i) {
        // tell display buffer is gone
        wnull_display_forget_buffer(dpy, self->buf[i]);
        slbuf_destroy(self->buf[i]);
    }

    // tell display window is gone
    wnull_display_clean(dpy, NULL, self);

    free(self);
    return true;
}

// to facilitate testing
static void
env_override(intptr_t *copy, intptr_t *wait)
{
    static char *over = NULL;
    if (!over) over = getenv("MODE");
    if (!over || !over[0] || !over[1]) return;

    switch (over[0]) {
        case 'n': *copy = WAFFLE_WINDOW_NULL_SHOW_METHOD_FLIP; break;
        case 'i': *copy = WAFFLE_WINDOW_NULL_SHOW_METHOD_COPY_I915; break;
        case 'g': *copy = WAFFLE_WINDOW_NULL_SHOW_METHOD_COPY_GL; break;
    }

    switch (over[1]) {
        case 'y': *wait = true; break;
        case 'n': *wait = false; break;
    }
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

    prt("window create %p\n", window);
    if (!wcore_window_init(&window->wcore, wc_config))
        goto error;

#if 0
    // EGL_PLATFORM_NULL is not providing EGL_NATIVE_VISUAL_ID
    // so we can't use this.
    window->gbm_format = wgbm_config_get_gbm_format(wc_plat,
                                                    wc_config->display,
                                                    wc_config);
#else
    if (wc_config->attrs.alpha_size <= 0)
        window->param.gbm_format = GBM_FORMAT_XRGB8888;
    else if (wc_config->attrs.alpha_size <= 8)
        window->param.gbm_format = GBM_FORMAT_ARGB8888;
    else {
        wcore_errorf(WAFFLE_ERROR_UNKNOWN, "unexpected alpha size");
        goto error;
    }
#endif

    struct wnull_display *dpy = wnull_display(wc_config->display);
    if (width == -1 && height == -1)
        wnull_display_get_size(dpy, &width, &height);
    window->param.width = width;
    window->param.height = height;

    window->param.color = wc_config->attrs.rgba_size > 0;
    window->param.depth = wc_config->attrs.depth_size > 0;
    window->param.stencil = wc_config->attrs.stencil_size > 0;

    if (wc_config->attrs.stencil_size)
        window->param.depth_stencil_format = GL_DEPTH24_STENCIL8_OES;
    else if (wc_config->attrs.depth_size <= 16)
        window->param.depth_stencil_format = GL_DEPTH_COMPONENT16;
    else if (wc_config->attrs.depth_size <= 24)
        window->param.depth_stencil_format = GL_DEPTH_COMPONENT24_OES;
    else
        window->param.depth_stencil_format = GL_DEPTH_COMPONENT32_OES;

    //TODO maybe open our own device here, maybe a render node
    window->param.gbm_device = wnull_display_get_gbm_device(dpy);

    if (!wcore_attrib_list_get(attrib_list,
                               WAFFLE_WINDOW_NULL_VSYNC_WAIT,
                               &window->vsync_wait))
        window->vsync_wait = true;

    intptr_t show_method;
    if (!wcore_attrib_list_get(attrib_list,
                               WAFFLE_WINDOW_NULL_SHOW_METHOD,
                               &show_method))
        show_method = WAFFLE_WINDOW_NULL_SHOW_METHOD_COPY_GL;

    env_override(&show_method, &window->vsync_wait);

    if (window->vsync_wait)
        prt("vsync wait: yes\n");
    else
        prt("vsync wait: no\n");

    window->param.gbm_flags = GBM_BO_USE_RENDERING;
    switch (show_method) {
        case WAFFLE_WINDOW_NULL_SHOW_METHOD_FLIP:
            prt("copy type: none (direct scanout)\n");
            // Enable scanout from our buffers.
            window->param.gbm_flags |= GBM_BO_USE_SCANOUT;
            window->buf_copy = NULL;
            break;
        case WAFFLE_WINDOW_NULL_SHOW_METHOD_COPY_I915:
            prt("copy type: i915\n");
            // Scanout will be from buffers to which we copy our buffers.
            // That copy code may not work when the source and destination
            // buffers have different flags, so set the scanout flag on our
            // buffers even though we won't scan out from them.
            //TODO Remove this if the copy code gets fixed.
            window->param.gbm_flags |= GBM_BO_USE_SCANOUT;
            window->buf_copy = slbuf_copy_i915;
            break;
        case WAFFLE_WINDOW_NULL_SHOW_METHOD_COPY_GL:
            prt("copy type: gl\n");
            window->buf_copy = slbuf_copy_gl;
            break;
    }
    window->param.egl_display = dpy->wegl.egl;

    struct wgbm_platform *plat = wgbm_platform(wegl_platform(wc_plat));
#define ASSIGN(type, name, args) window->func.name = plat->name;
    GBM_FUNCTIONS(ASSIGN);
#undef ASSIGN

#define ASSIGN(type, name, args) window->func.name = plat->wegl.name;
    EGL_FUNCTIONS(ASSIGN);
#undef ASSIGN

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
    int ok = true;

    if (self->show && self->drawbuf && self->param.color) {
        struct wnull_display *dpy = wnull_display(wc_self->display);

        ok &= wnull_display_present_buffer(dpy,
                                           self->drawbuf,
                                           self->buf_copy,
                                           self->vsync_wait);
        ok &= wnull_window_prepare_draw_buffer(self);
    }

    return ok;
}

// This deletes all framebuffers belonging to the window.
// If one of those was bound, the binding reverts to zero, which suits us
// very well, since if this window gets used again we need the binding to
// be zero before we can change it to one of our framebuffers.
// See wnull_window_prepare_draw_buffer().
static void
wnull_window_free_gl_resources(struct wnull_window *self)
{
    for (int i = 0; i < ARRAY_SIZE(self->buf); ++i)
        slbuf_free_gl_resources(self->buf[i]);
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

    if (ctx == dpy->current_context && win == dpy->current_window)
        return true;

    bool first; // first time the context/window pair will be current?
    struct wnull_window **old_win = NULL; // list of windows in old context
    if (!wnull_display_make_current(dpy, ctx, win, &first, &old_win))
        return false;

    // When the current context is changed to a different one we must
    // clean up any gl resources used in the first context as it may
    // not be seen again.
    if (old_ctx && old_ctx != ctx && old_win)
        for (struct wnull_window **w = old_win; *w; ++w)
            wnull_window_free_gl_resources(*w);
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
#define ASSIGN(type, name, args) win->func.name = ctx->name;
        GL_FUNCTIONS(ASSIGN);
#undef ASSIGN
        ok = wnull_window_prepare_draw_buffer(win);
        if (ok && first) {
            prt("setting viewport\n");
            // For compatibility with eglMakeCurrent and glxMakeCurrent,
            // set viewport and scissor only the first time this
            // context/window becomes current.
            ctx->glViewport(0, 0, win->param.width, win->param.height);
            ctx->glScissor(0, 0, win->param.width, win->param.height);
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
    n_window->null->width = self->param.width;
    n_window->null->height = self->param.height;

    return n_window;
}
