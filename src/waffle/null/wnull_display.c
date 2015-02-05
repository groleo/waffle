// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <libudev.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <i915_drm.h>

#include "wcore_error.h"

#include "wgbm_display.h"
#include "wgbm_platform.h"

#include "wnull_display.h"

#define ARRAY_END(a) ((a)+sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))

#if 0
#include <stdio.h>
#define prt(...) fprintf(stderr, __VA_ARGS__)
#else
#define prt(...)
#endif

struct wnull_display_buffer {
    struct wgbm_platform *plat;
    struct gbm_bo *bo;
    uint32_t drm_fb;
    int drm_fd;
    int dmabuf_fd;
    void (*finish)();
};

struct drm_display {
    struct gbm_device *gbm_device;
    drmModeConnectorPtr conn;
    drmModeModeInfoPtr mode;
    drmModeCrtcPtr crtc;
    int32_t width;
    int32_t height;
    bool setcrtc_done;
    bool flip_pending;
    struct wnull_display_buffer scanout[2];
    struct wnull_display_buffer *current;
};


static void
wnull_display_buffer_teardown(struct wnull_display_buffer *buf)
{
    if (!buf || !buf->bo)
        return;
    if (buf->dmabuf_fd >= 0) {
        close(buf->dmabuf_fd);
        buf->dmabuf_fd = -1;
    }
    if (buf->drm_fd >= 0) {
        drmModeRmFB(buf->drm_fd, buf->drm_fb);
        buf->drm_fd = -1;
    }
    buf->plat->gbm_bo_destroy(buf->bo);
    buf->bo = NULL;
    prt("tore down display buffer %p\n",buf);
}

void
wnull_display_buffer_destroy(struct wnull_display_buffer *buf)
{
    wnull_display_buffer_teardown(buf);
    free(buf);
    prt("destroyed display buffer %p\n",buf);
}

static bool
wnull_display_buffer_init(struct wnull_display_buffer *self,
                          struct wnull_display *dpy,
                          int width, int height,
                          uint32_t format, uint32_t flags,
                          void (*finish)())
{
    if (self->bo)
        return true;

    self->plat = wgbm_platform(wegl_platform(dpy->wegl.wcore.platform));
    self->drm_fd = -1;
    self->dmabuf_fd = -1;
    self->finish = finish;

    if (width == -1 && height == -1) {
        width = dpy->drm->width;
        height = dpy->drm->height;
    }

    self->bo = self->plat->gbm_bo_create(dpy->drm->gbm_device,
                                         width, height,
                                         format, flags);
    if (self->bo) {
        prt("init-ed display buffer %p\n",self);
        return true;
    }

    wnull_display_buffer_teardown(self);
    return false;
}

struct wnull_display_buffer *
wnull_display_buffer_create(struct wnull_display *dpy,
                            int width, int height,
                            uint32_t format, uint32_t flags,
                            void (*finish)())
{
    struct wnull_display_buffer *buf = wcore_calloc(sizeof(*buf));
    if (!buf)
        return NULL;

    if (wnull_display_buffer_init(buf, dpy, width, height, format, flags, finish)) {
        prt("created display buffer %p\n",buf);
        return buf;
    }

    wnull_display_buffer_destroy(buf);
    return NULL;
}

bool
wnull_display_buffer_dmabuf(struct wnull_display_buffer *buf,
                            int *fd,
                            uint32_t *stride)
{
    if (!buf->bo)
        return false;
    if (buf->dmabuf_fd < 0)
        buf->dmabuf_fd = buf->plat->gbm_bo_get_fd(buf->bo);
    if (buf->dmabuf_fd < 0)
        return false;
    if (fd)
        *fd = buf->dmabuf_fd;
    if (stride)
        *stride = buf->plat->gbm_bo_get_stride(buf->bo);
    return true;
}

static void
page_flip_handler(int fd,
                  unsigned int sequence,
                  unsigned int tv_sec,
                  unsigned int tv_usec,
                  void *user_data)
{
    struct drm_display *drm = (struct drm_display *) user_data;
    assert(drm->flip_pending);
    drm->flip_pending = false;
}

bool
wnull_display_show_buffer(struct wnull_display *dpy,
                          struct wnull_display_buffer *buf)
{
    struct drm_display *drm = dpy->drm;
    struct wgbm_platform *plat = buf->plat;
    int fd = plat->gbm_device_get_fd(drm->gbm_device);

    prt("showing %p\n", buf);
    if (!drm->crtc)
        return true;

    assert(buf->bo);
    if (buf->drm_fd < 0) {
        if (drmModeAddFB(fd,
                         plat->gbm_bo_get_width(buf->bo),
                         plat->gbm_bo_get_height(buf->bo),
                         24, 32,
                         plat->gbm_bo_get_stride(buf->bo),
                         plat->gbm_bo_get_handle(buf->bo).u32,
                         &buf->drm_fb)) {
            wcore_errorf(WAFFLE_ERROR_UNKNOWN,
                         "drm addfb failed: errno=%d",
                         errno);
            return false;
        }
        buf->drm_fd = fd;
    }

    if (!drm->setcrtc_done) {
        if (drmModeSetCrtc(fd, drm->crtc->crtc_id, buf->drm_fb, 0, 0,
                           &drm->conn->connector_id, 1, drm->mode)) {
            wcore_errorf(WAFFLE_ERROR_UNKNOWN,
                         "drm setcrtc failed: errno=%d",
                         errno);
            return false;
        }
        drm->setcrtc_done = true;
    }

    // wait for pending flip, if any
    if (drm->flip_pending) {
        drmEventContext event = {
            .version = DRM_EVENT_CONTEXT_VERSION,
            .page_flip_handler = page_flip_handler,
        };
        drmHandleEvent(fd, &event);
    }
    assert(!drm->flip_pending);

    // do this after waiting for flip because during that wait
    // rendering could proceed, making this wait shorter
    if (buf->finish)
        buf->finish();

    drm->flip_pending = true;
    if (drmModePageFlip(fd, drm->crtc->crtc_id, buf->drm_fb,
                        DRM_MODE_PAGE_FLIP_EVENT, drm)) {
        wcore_errorf(WAFFLE_ERROR_UNKNOWN,
                     "drm page flip failed: errno=%d",
                     errno);
        return false;
    }

    return true;
}

//XXX i915-specific
static bool
buffer_copy(struct wnull_display_buffer *dst,
            struct wnull_display_buffer *src)
{
    assert(dst->bo);
    assert(src->bo);

    struct drm_i915_gem_get_tiling dst_tiling = {
        .handle = dst->plat->gbm_bo_get_handle(dst->bo).u32,
    };
    struct drm_i915_gem_get_tiling src_tiling = {
        .handle = src->plat->gbm_bo_get_handle(src->bo).u32,
    };
    int dst_fd = dst->plat->gbm_device_get_fd(dst->plat->gbm_bo_get_device(dst->bo));
    int src_fd = src->plat->gbm_device_get_fd(src->plat->gbm_bo_get_device(src->bo));

    if (drmIoctl(dst_fd, DRM_IOCTL_I915_GEM_GET_TILING, &dst_tiling) ||
        drmIoctl(src_fd, DRM_IOCTL_I915_GEM_GET_TILING, &src_tiling))
        return false;

    if (dst_tiling.tiling_mode != src_tiling.tiling_mode)
        return false;

    unsigned rows;
    switch (dst_tiling.tiling_mode) {
        case I915_TILING_NONE:
            rows = 1;
            break;
        case I915_TILING_X:
            rows = 8;
            break;
        default:
            return false;
    }

    unsigned dst_step = dst->plat->gbm_bo_get_stride(dst->bo) * rows;
    unsigned src_step = src->plat->gbm_bo_get_stride(src->bo) * rows;
    unsigned copy_size = MIN(src_step, dst_step);
    // round up, not down, or we miss the last partly filled tile
    unsigned num_copy = (MIN(src->plat->gbm_bo_get_height(src->bo),
                             dst->plat->gbm_bo_get_height(dst->bo)) + rows - 1)
                             / rows;

    void *tmp = malloc(copy_size);
    if (!tmp)
        return false;

    struct drm_i915_gem_pread pread = {
        .handle = src->plat->gbm_bo_get_handle(src->bo).u32,
        .size = copy_size,
        .offset = 0,
        .data_ptr = (uint64_t) (uintptr_t) tmp,
    };

    struct drm_i915_gem_pwrite pwrite = {
        .handle = dst->plat->gbm_bo_get_handle(dst->bo).u32,
        .size = copy_size,
        .offset = 0,
        .data_ptr = (uint64_t) (uintptr_t) tmp,
    };

    // blit on gpu must be faster than this, but seems complicated to do
    bool ok = true;
    for (int i = 0; ok && i < num_copy; ++i) {
        ok = !(drmIoctl(src_fd, DRM_IOCTL_I915_GEM_PREAD, &pread) ||
               drmIoctl(dst_fd, DRM_IOCTL_I915_GEM_PWRITE, &pwrite));
        pread.offset += src_step;
        pwrite.offset += dst_step;
    }
    free(tmp);
    return ok;
}

bool
wnull_display_copy_buffer(struct wnull_display *dpy,
                          struct wnull_display_buffer *buf)
{
    struct drm_display *drm = dpy->drm;

    prt("copying %p\n", buf);
    if (!drm->current)
        drm->current = drm->scanout;

    if (!wnull_display_buffer_init(drm->current,
                                   dpy,
                                   -1, -1,
                                   buf->plat->gbm_bo_get_format(buf->bo),
                                   GBM_BO_USE_SCANOUT,
                                   NULL))
        return false;

    assert(buf->finish);
    buf->finish();

    if (!buffer_copy(drm->current, buf)) {
        prt("copy failed %p\n", buf);
        return false;
    }

    if ( !wnull_display_show_buffer(dpy, drm->current)) {
        prt("show failed %p\n", buf);
        return false;
    }

    ++drm->current;
    if (drm->current == ARRAY_END(drm->scanout))
        drm->current = drm->scanout;
    return true;
}

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
    drmModeEncoderPtr enc = 0;
    for (int i = 0; i < conn->count_encoders; ++i) {
        drmModeFreeEncoder(enc);
        enc = drmModeGetEncoder(fd, conn->encoders[i]);
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
    struct wnull_display_buffer *buf;
    for (buf = self->scanout; buf < ARRAY_END(self->scanout); ++buf)
        wnull_display_buffer_teardown(buf);

    if (self->gbm_device) {
        int fd = plat->gbm_device_get_fd(self->gbm_device);
        plat->gbm_device_destroy(self->gbm_device);
        close(fd);
    }

    drmModeFreeConnector(self->conn);
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
    for (int i = 0; i < mr->count_connectors; ++i) {
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
        if (drm->crtc) {
            drm->width = drm->mode->hdisplay;
            drm->height = drm->mode->vdisplay;
            return drm;
        }
    }

    if (!monitor_connected) {
        prt("headless\n");
        assert(!drm->crtc);
        // arbitrary
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
        fd = wgbm_get_default_fd_for_pattern("card[0-9]*");

    if (fd < 0) {
        wcore_errorf(WAFFLE_ERROR_UNKNOWN, "open drm file for gbm failed");
        goto error;
    }

    self->drm = drm_display_create(fd, plat);
    if (!self->drm)
        goto error;

    if (!wegl_display_init(&self->wegl, wc_plat,
                           (intptr_t) self->drm->gbm_device))
        goto error;

    prt("create display %p\n",self);
    return &self->wegl.wcore;

error:
    wnull_display_destroy(&self->wegl.wcore);
    return NULL;
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

struct ctx_win {
    struct wnull_context *ctx;
    struct wnull_window *win;
};

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
// Return false for failure and do not modify the output parameters.
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
        }
        // add at end
        self->cur[self->num_cur].ctx = ctx;
        self->cur[self->num_cur].win = win;
        ++self->num_cur;
    }

    prt("ctx/win list after:\n"); for (int i = 0; i < self->num_cur; ++i) prt("  %p/%p\n", self->cur[i].ctx, self->cur[i].win);
    self->current_context = ctx;

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
