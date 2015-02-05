// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "waffle_null.h"

#include "wegl_display.h"

struct wcore_platform;
struct wgbm_platform;
struct wnull_window;
struct wnull_display_buffer;

struct wnull_display {
    struct wnull_context *current_context;
    struct wegl_display wegl;

    struct drm_display *drm;

    struct ctx_win *cur; // list of context/window pairs which have been current
    int num_cur; // number of items in list
    int len_cur; // length of array
};

static inline struct wnull_display*
wnull_display(struct wcore_display *wc_self)
{
    if (wc_self) {
        struct wegl_display *wegl_self = container_of(wc_self, struct wegl_display, wcore);
        return container_of(wegl_self, struct wnull_display, wegl);
    } else {
        return NULL;
    }
}

struct wcore_display*
wnull_display_connect(struct wcore_platform *wc_plat,
                      const char *name);

bool
wnull_display_destroy(struct wcore_display *wc_self);

void
wnull_display_get_size(struct wnull_display *self,
                       int32_t *width, int32_t *height);

union waffle_native_display*
wnull_display_get_native(struct wcore_display *wc_self);

void
wnull_display_fill_native(struct wnull_display *self,
                          struct waffle_null_display *n_dpy);

bool
wnull_display_make_current(struct wnull_display *self,
                           struct wnull_context *ctx,
                           struct wnull_window *win,
                           bool *first,
                           struct wnull_window ***old_win);

void
wnull_display_clean(struct wnull_display *self,
                    struct wnull_context *ctx,
                    struct wnull_window *win);

struct wnull_display_buffer *
wnull_display_buffer_create(struct wnull_display *dpy,
                            int width, int height,
                            uint32_t format, uint32_t flags,
                            void (*finish)());

void
wnull_display_buffer_destroy(struct wnull_display_buffer *self);

bool
wnull_display_buffer_dmabuf(struct wnull_display_buffer *self,
                            int *fd,
                            uint32_t *stride);

bool
wnull_display_show_buffer(struct wnull_display *self,
                          struct wnull_display_buffer *buf);

bool
wnull_display_copy_buffer(struct wnull_display *self,
                          struct wnull_display_buffer *buf);
