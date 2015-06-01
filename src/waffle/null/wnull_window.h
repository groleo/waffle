// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#pragma once

#include <stdbool.h>

struct wcore_config;
struct wcore_display;
struct wcore_platform;
struct wcore_window;

struct wnull_window;

struct wnull_window*
wnull_window(struct wcore_window *wc_self);

struct wcore_window*
wnull_window_create(struct wcore_platform *wc_plat,
                   struct wcore_config *wc_config,
                   int32_t width,
                   int32_t height,
                   const intptr_t attrib_list[]);

bool
wnull_window_destroy(struct wcore_window *wc_self);

bool
wnull_window_show(struct wcore_window *wc_self);

bool
wnull_window_swap_buffers(struct wcore_window *wc_self);

union waffle_native_window*
wnull_window_get_native(struct wcore_window *wc_self);

bool
wnull_make_current(struct wcore_platform *wc_plat,
                   struct wcore_display *wc_dpy,
                   struct wcore_window *wc_window,
                   struct wcore_context *wc_ctx);

bool
wnull_window_prepare_draw_buffer(struct wnull_window *self);
