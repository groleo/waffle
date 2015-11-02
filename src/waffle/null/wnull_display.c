// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "wcore_error.h"

#include "wgbm_display.h"

#include "wnull_buffer.h"
#include "wnull_context.h"
#include "wnull_display.h"
#include "wnull_platform.h"

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

#if 0
#include <errno.h>
#include <stdio.h>
#define prt(...) fprintf(stderr, __VA_ARGS__)
#else
#define prt(...)
#endif

struct drm_display {
    struct gbm_device *gbm_device;
    drmModeConnectorPtr conn;
    drmModeModeInfoPtr mode;
    drmModeCrtcPtr crtc;
    int32_t width;
    int32_t height;
    bool setcrtc_done;
    struct slbuf *scanout[2]; // front & back
    struct slbuf *screen_buffer; // on screen
    struct slbuf *pending_buffer; // scheduled flip to this
    bool flip_pending;
};

struct ctx_win {
    struct wnull_context *ctx;
    struct wnull_window *win;
};

static drmModeModeInfoPtr
choose_mode(drmModeConnectorPtr conn)
{
    drmModeModeInfoPtr mode = NULL;
    assert(conn);
    assert(conn->connection == DRM_MODE_CONNECTED);
    // use first preferred mode if any, else end up with last mode in list
    for (int i = 0; i < conn->count_modes; ++i) {
        mode = conn->modes + i;
        if (mode->type & DRM_MODE_TYPE_PREFERRED)
            break;
    }
    return mode;
}

static int
choose_crtc(int fd, unsigned count_crtcs, drmModeConnectorPtr conn)
{
    for (int i = 0; i < conn->count_encoders; ++i) {
        drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoders[i]);
        unsigned b = enc->possible_crtcs;
        drmModeFreeEncoder(enc);
        for (int j = 0; b && j < count_crtcs; b >>= 1, ++j) {
            if (b & 1)
                return j;
        }
    }
    return -1;
}

static void
drm_display_destroy(struct drm_display *self, struct wgbm_platform *plat)
{
    for (int i = 0; i < ARRAY_SIZE(self->scanout); ++i)
        slbuf_destroy(self->scanout[i]);

    drmModeFreeConnector(self->conn);
    drmModeFreeCrtc(self->crtc);

    if (self->gbm_device) {
        int fd = plat->gbm_device_get_fd(self->gbm_device);
        plat->gbm_device_destroy(self->gbm_device);
        close(fd);
    }

    free(self);
}

static struct drm_display *
drm_display_create(int fd, struct wgbm_platform *plat)
{
    struct drm_display *drm = wcore_calloc(sizeof(*drm));
    if (!drm)
        return NULL;

    dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL);
    drm->gbm_device = plat->gbm_create_device(fd);
    if (!drm->gbm_device) {
        wcore_errorf(WAFFLE_ERROR_UNKNOWN, "gbm_create_device failed");
        goto error;
    }

    drm->conn = NULL;
    drmModeResPtr mr = drmModeGetResources(fd);
    if (!mr) {
        wcore_errorf(WAFFLE_ERROR_UNKNOWN,
                     "no display on device (is it a render node?");
        goto error;
    }

    bool monitor_connected = false;
    for (int i = 0; !drm->crtc && i < mr->count_connectors; ++i) {
        drmModeFreeConnector(drm->conn);
        drm->conn = drmModeGetConnector(fd, mr->connectors[i]);
        if (!drm->conn || drm->conn->connection != DRM_MODE_CONNECTED)
            continue;
        monitor_connected = true;
        drm->mode = choose_mode(drm->conn);
        if (!drm->mode)
            continue;
        int n = choose_crtc(fd, mr->count_crtcs, drm->conn);
        if (n < 0)
            continue;
        drm->crtc = drmModeGetCrtc(fd, mr->crtcs[n]);
    }
    drmModeFreeResources(mr);

    if (drm->crtc) {
        drm->width = drm->mode->hdisplay;
        drm->height = drm->mode->vdisplay;
        return drm;
    }

    if (!monitor_connected) {
        prt("headless\n");
        // arbitrary size, so programs that request fullscreen windows can work
        drm->width = 1280;
        drm->height = 1024;
        return drm;
    }

error:
    drm_display_destroy(drm, plat);
    return NULL;
}

bool
wnull_display_destroy(struct wcore_display *wc_self)
{
    struct wnull_display *self = wnull_display(wc_self);

    if (!self)
        return true;

    if (self->drm)
        drm_display_destroy(self->drm, 
                            wgbm_platform(wegl_platform(wc_self->platform)));

    bool ok = wegl_display_teardown(&self->wegl);

    free(self->cur);
    free(self);
    prt("destroy display %p\n", self);
    return ok;
}

static int kms_device_fd()
{
    bool has_conn = false;
    for (int i = 0; i < 8; ++i) {
        char path[99];
        sprintf(path, "/dev/dri/card%d", i);
        prt("trying %s\n", path);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            drmModeResPtr mr = drmModeGetResources(fd);
            if (mr) {
                has_conn = mr->count_connectors > 0;
                drmModeFreeResources(mr);
            }
            if (has_conn) {
                prt("using %s\n", path);
                return fd;
            }
            close(fd);
        }
    }
    return -1;
}

struct wcore_display*
wnull_display_connect(struct wcore_platform *wc_plat,
                      const char *name)
{
    struct wgbm_platform *plat = wgbm_platform(wegl_platform(wc_plat));

    struct wnull_display *self = wcore_calloc(sizeof(*self));
    if (!self)
        return NULL;

    int fd;
    if (name != NULL)
        fd = open(name, O_RDWR | O_CLOEXEC);
    else
        fd = kms_device_fd();

    if (fd < 0) {
        wcore_errorf(WAFFLE_ERROR_UNKNOWN, "open drm file for gbm failed");
        goto error;
    }

    self->drm = drm_display_create(fd, plat);
    if (!self->drm)
        goto error;

    if (!wegl_display_init(&self->wegl, wc_plat, (intptr_t)EGL_DEFAULT_DISPLAY))
        goto error;

    self->param.width = self->drm->width;
    self->param.height = self->drm->height;
    self->param.color = true;
    self->param.gbm_device = self->drm->gbm_device;
    self->param.egl_display = self->wegl.egl;

#define ASSIGN(type, name, args) self->func.name = plat->name;
    GBM_FUNCTIONS(ASSIGN);
#undef ASSIGN

#define ASSIGN(type, name, args) self->func.name = plat->wegl.name;
    EGL_FUNCTIONS(ASSIGN);
#undef ASSIGN

    prt("create display %p\n",self);
    return &self->wegl.wcore;

error:
    wnull_display_destroy(&self->wegl.wcore);
    return NULL;
}

bool
wnull_display_supports_context_api(struct wcore_display *wc_dpy,
                                   int32_t waffle_context_api)
{
    struct wegl_display *dpy = wegl_display(wc_dpy);
    struct wcore_platform *wc_plat = dpy->wcore.platform;

    switch (waffle_context_api) {
        case WAFFLE_CONTEXT_OPENGL_ES2:
            return dpy->EXT_image_dma_buf_import &&
                wc_plat->vtbl->dl_can_open(wc_plat, WAFFLE_DL_OPENGL_ES2);
        case WAFFLE_CONTEXT_OPENGL:
        case WAFFLE_CONTEXT_OPENGL_ES1:
        case WAFFLE_CONTEXT_OPENGL_ES3:
            break;
        default:
            wcore_error_internal("waffle_context_api has bad value %#x",
                                 waffle_context_api);
    }
    return false;
}

void
wnull_display_get_size(struct wnull_display *self,
                       int32_t *width, int32_t *height)
{
    *width = self->drm->width;
    *height = self->drm->height;
}

void
wnull_display_fill_native(struct wnull_display *self,
                          struct waffle_null_display *n_dpy)
{
    n_dpy->gbm_device = self->drm->gbm_device;
    n_dpy->egl_display = self->wegl.egl;
}

union waffle_native_display*
wnull_display_get_native(struct wcore_display *wc_self)
{
    struct wnull_display *self = wnull_display(wc_self);
    union waffle_native_display *n_dpy;

    WCORE_CREATE_NATIVE_UNION(n_dpy, null);
    if (n_dpy == NULL)
        return NULL;

    wnull_display_fill_native(self, n_dpy->null);

    return n_dpy;
}

struct gbm_device*
wnull_display_get_gbm_device(struct wnull_display *self)
{
    return self->drm->gbm_device;
}

void
wnull_display_forget_buffer(struct wnull_display *self, struct slbuf *buf)
{
    struct drm_display *dpy = self->drm;

    if (dpy->screen_buffer == buf)
        dpy->screen_buffer = NULL;
    if (dpy->pending_buffer == buf)
        dpy->pending_buffer = NULL;
}

// This must be called when context is going to change to 'ctx' but before
// actually changing the context (calling eglMakeCurrent).
//
// Keep track of which context is current and maintain a list of which
// context/window pairs have been current.
// This lets us answer two questions:
// 1) Is it the first time the given pair will be current together?
// 2) Which windows have been current with the current context?
//
// The pair (ctx,win) is added to the list, if not already there.
// *first is set to true if it wasn't already there.
// *old_win_ptr is pointed at a NULL-terminated array of windows which were
// ever current with the current context.
// Finally the current context is set to 'ctx.'
//
// Caller responsible for freeing *old_win_ptr.
// Return false for failure and do not modify *old_win_ptr.
bool
wnull_display_make_current(struct wnull_display *self,
                           struct wnull_context *ctx,
                           struct wnull_window *win,
                           bool *first,
                           struct wnull_window ***old_win_ptr)
{
    assert(self->num_cur <= self->len_cur);
    prt("make_current dpy %p ctx %p win %p\n", self, ctx, win);
    prt("ctx/win list before:\n"); for (int i = 0; i < self->num_cur; ++i) prt("  %p/%p\n", self->cur[i].ctx, self->cur[i].win);

    struct wnull_window **old_win;
    if (self->current_context) {
        // allocate the most we might use
        // i.e. length of our list plus one for the terminator
        old_win = wcore_calloc((self->num_cur + 1) * sizeof(old_win[0]));
        if (!old_win)
            return false;
        *old_win_ptr = old_win;

        // Clean up any GL resources the display may have created
        // in the outgoing context.
        for (int i = 0; i < ARRAY_SIZE(self->drm->scanout); ++i)
            slbuf_free_gl_resources(self->drm->scanout[i]);
    }

    // search for given pair; build list of windows found with current context
    *first = true;
    for (int i = 0; i < self->num_cur; ++i) {
        assert(self->cur[i].ctx);
        assert(self->cur[i].win);
        if (self->cur[i].ctx == ctx && self->cur[i].win == win)
            *first = false;
        if (self->cur[i].ctx == self->current_context) {
            *old_win++ = self->cur[i].win;
            *old_win = NULL;
        }
    }

    if (ctx && *first) {
        assert(win);
        // add to list
        if (self->num_cur == self->len_cur) {
            // grow list
            self->len_cur += 5;
            self->cur = realloc(self->cur,
                                self->len_cur * sizeof(self->cur[0]));
            if (!self->cur)
                return false;
        }
        // add at end
        self->cur[self->num_cur].ctx = ctx;
        self->cur[self->num_cur].win = win;
        ++self->num_cur;
    }

    if (ctx) {
#define ASSIGN(type, name, args) self->func.name = ctx->name;
        GL_FUNCTIONS(ASSIGN);
#undef ASSIGN
    }

    prt("ctx/win list after:\n"); for (int i = 0; i < self->num_cur; ++i) prt("  %p/%p\n", self->cur[i].ctx, self->cur[i].win);
    self->current_context = ctx;
    self->current_window = win;
    struct wnull_platform *plat =
        wnull_platform(wgbm_platform(wegl_platform(self->wegl.wcore.platform)));
    plat->current_display = self;

    assert(self->num_cur <= self->len_cur);

    return true;
}

// Remove entries from the list of context/window pairs whose
// context == 'ctx' or whose window == 'win.'
void
wnull_display_clean(struct wnull_display *self,
                    struct wnull_context *ctx,
                    struct wnull_window *win)
{
    prt("cleaning dpy %p ctx %p win %p\n", self, ctx, win);
    prt("ctx/win list before:\n"); for (int i = 0; i < self->num_cur; ++i) prt("  %p/%p\n", self->cur[i].ctx, self->cur[i].win);
    for (int i = 0; i < self->num_cur;) {
        assert(self->cur[i].ctx);
        assert(self->cur[i].win);
        if (self->cur[i].ctx == ctx || self->cur[i].win == win)
            self->cur[i] = self->cur[--self->num_cur];
        else
            ++i;
    }
    prt("ctx/win list after:\n"); for (int i = 0; i < self->num_cur; ++i) prt("  %p/%p\n", self->cur[i].ctx, self->cur[i].win);
}

static void
page_flip_handler(int fd,
                  unsigned int sequence,
                  unsigned int tv_sec,
                  unsigned int tv_usec,
                  void *user_data)
{
    struct drm_display *dpy = (struct drm_display *) user_data;

    assert(dpy->flip_pending);
    dpy->flip_pending = false;

    if (dpy->screen_buffer) {
        // the buffer that was on screen isn't now
        slbuf_set_display(dpy->screen_buffer, NULL);
        dpy->screen_buffer = NULL;
    }

    if (dpy->pending_buffer) {
        // the buffer that was pending is now on screen
        dpy->screen_buffer = dpy->pending_buffer;
        dpy->pending_buffer = NULL;
    }
}

bool
wnull_display_present_buffer(struct wnull_display *self,
                             struct slbuf *buf,
                             bool (*copier)(struct slbuf *, struct slbuf *),
                             bool wait_for_vsync)
{
    struct drm_display *dpy = self->drm;

    if (!dpy->crtc)
        // no monitor
        return true;

    int fd = self->func.gbm_device_get_fd(dpy->gbm_device);
    struct pollfd pfd = { fd, POLLIN };
    if (poll(&pfd, 1, 0) < 0) {
        prt("poll failed %d\n", errno);
        return false;
    }
    bool wont_block = pfd.revents & POLLIN;

    if (dpy->flip_pending && (wait_for_vsync || wont_block)) {
        prt("waiting for flip ");
        if (wont_block) prt("but shouldn't take long");
        prt("\n");
        drmEventContext event = {
            .version = DRM_EVENT_CONTEXT_VERSION,
            .page_flip_handler = page_flip_handler,
        };
        drmHandleEvent(fd, &event);
        assert(!dpy->flip_pending);
    }

    if (dpy->flip_pending) {
        // Do not present 'buf' because an earler buffer is pending and we
        // don't want to wait.
        prt("will not show %p\n", buf);
        // Without a flush here it seems like the pipeline gets backlogged
        // and animation can be jerky.
        slbuf_flush(buf);
        return true;
    }

    struct slbuf *show = buf;

    if (copier) {
        self->param.gbm_flags = GBM_BO_USE_SCANOUT;
        if (copier == slbuf_copy_gl)
            self->param.gbm_flags |= GBM_BO_USE_RENDERING;

        //TODO if format changes we should probably recreate scanout buffers
        self->param.gbm_format = slbuf_gbm_format(buf);
        show = slbuf_get_buffer(dpy->scanout,
                                ARRAY_SIZE(dpy->scanout),
                                &self->param,
                                &self->func);
        if (!show) {
            prt("no back buffer\n");
            return false;
        }

        prt("copy %p to %p\n", buf, show);
        slbuf_finish(buf);
        if (!copier(show, buf)) {
            prt("copy failed\n");
            return false;
        }
    }

    slbuf_finish(show);

    uint32_t fb;
    if (!slbuf_get_drmfb(show, &fb))
        return false;

    if (!dpy->setcrtc_done) {
        if (drmModeSetCrtc(fd, dpy->crtc->crtc_id, fb, 0, 0,
                           &dpy->conn->connector_id, 1, dpy->mode)) {
            prt("drm setcrtc failed %d\n", errno);
            return false;
        }
        dpy->setcrtc_done = true;
        dpy->screen_buffer = show;
    } else {
        if (drmModePageFlip(fd, dpy->crtc->crtc_id, fb,
                            DRM_MODE_PAGE_FLIP_EVENT, dpy)) {
            prt("drm page flip failed %d", errno);
            return false;
        }
        prt("scheduled flip to %p\n", show);
        dpy->flip_pending = true;
        dpy->pending_buffer = show;
    }

    slbuf_set_display(show, self);
    return true;
}
