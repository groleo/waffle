// Copyright 2015 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.txt file or at
// https://developers.google.com/open-source/licenses/bsd

#include <assert.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include <gbm.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <i915_drm.h>

#include <drm_fourcc.h>

#include "wcore_util.h"
#include "wnull_buffer.h"

#if 0
#include <stdio.h>
#define prt(...) fprintf(stderr, __VA_ARGS__)
#else
#define prt(...)
#endif

#define MIN(a,b) ((a)<(b)?(a):(b))

#define CHECK_GL_ERROR if (gl_error(f->glGetError(), __LINE__)) goto gl_error;

static bool
gl_error(GLenum error, int line)
{
    if (error == GL_NO_ERROR)
        return false;
    prt("gl error %x @ line %d\n", (int) error, line);
    return true;
}

struct slbuf {
    struct slbuf_param *p;
    struct slbuf_func *f;

    struct gbm_bo *bo;
    int dmabuf;

    uint32_t drmfb;
    bool has_drmfb;

    EGLImageKHR image;

    GLuint glfb;
    GLuint color;
    GLuint depth_stencil;
    GLuint texture;

    struct wnull_display *display; // display on which we are showing or pending
};

static GLuint program = 0;
static GLuint vertex_shader = 0;
static GLuint fragment_shader = 0;

static int
slbuf_drmfd(struct slbuf *self)
{
    assert(self->p->gbm_device);
    return self->f->gbm_device_get_fd(self->p->gbm_device);
}

static struct gbm_bo*
slbuf_get_bo(struct slbuf *self)
{
    if (!self->bo)
        self->bo = self->f->gbm_bo_create(self->p->gbm_device,
                                          self->p->width,
                                          self->p->height,
                                          self->p->gbm_format,
                                          self->p->gbm_flags);
    return self->bo;
}

static void
slbuf_free_bo(struct slbuf *self)
{
    if (self->bo)
        self->f->gbm_bo_destroy(self->bo);
}

static uint32_t
slbuf_stride(struct slbuf *self)
{
    struct gbm_bo *bo = slbuf_get_bo(self);
    assert(bo);
    return self->f->gbm_bo_get_stride(bo);
}

static uint32_t
slbuf_handle(struct slbuf *self)
{
    struct gbm_bo *bo = slbuf_get_bo(self);
    assert(bo);
    return self->f->gbm_bo_get_handle(bo).u32;
}

bool
slbuf_get_drmfb(struct slbuf *self, uint32_t *fb)
{
    if (!self->has_drmfb) {
        if (drmModeAddFB(slbuf_drmfd(self),
                          self->p->width,
                          self->p->height,
                          24, 32,
                          slbuf_stride(self),
                          slbuf_handle(self),
                          &self->drmfb)) {
            prt("drmModeAddFB failed\n");
            return false;
        }
        self->has_drmfb = true;
    }
    *fb = self->drmfb;
    return true;
}

static void
slbuf_free_drmfb(struct slbuf *self)
{
    if (self->has_drmfb) {
        drmModeRmFB(slbuf_drmfd(self), self->drmfb);
        self->has_drmfb = false;
    }
}

static int
slbuf_get_dmabuf(struct slbuf *self)
{
    if (self->dmabuf < 0) {
        struct gbm_bo *bo = slbuf_get_bo(self);
        if (bo)
            self->dmabuf = self->f->gbm_bo_get_fd(bo);
    }
    return self->dmabuf;
}

static void
slbuf_free_dmabuf(struct slbuf *self)
{
    if (self->dmabuf >= 0) {
        close(self->dmabuf);
        self->dmabuf = -1;
    }
}

static EGLImageKHR
slbuf_get_image(struct slbuf *self)
{
    if (self->image == EGL_NO_IMAGE_KHR) {
        int fd = slbuf_get_dmabuf(self);
        if (fd < 0)
            goto done;

        const EGLint attr[] = {
            EGL_WIDTH, self->p->width,
            EGL_HEIGHT, self->p->height,
            EGL_LINUX_DRM_FOURCC_EXT, slbuf_drm_format(self),
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, slbuf_stride(self),
            EGL_NONE,
        };

        self->image = self->f->eglCreateImageKHR(self->p->egl_display,
                                                 EGL_NO_CONTEXT,
                                                 EGL_LINUX_DMA_BUF_EXT,
                                                 NULL,
                                                 attr);

    }
done:
    return self->image;
}

static void
slbuf_free_image(struct slbuf *self)
{
    if (self->image != EGL_NO_IMAGE_KHR) {
        self->f->eglDestroyImageKHR(self->p->egl_display, self->image);
        self->image = EGL_NO_IMAGE_KHR;
    }
}

// Return the gl framebuffer id if any, else zero.
GLuint
slbuf_check_glfb(struct slbuf *self)
{
    return self ? self->glfb : 0;
}

static GLuint
slbuf_get_glfb(struct slbuf *self)
{
    struct slbuf_func *f = self->f;

    assert(self->p->color || self->p->depth || self->p->stencil);
    prt("slbuf %p get gl fb\n", self);
    if (!self->glfb) {
        GLint save_rb;
        f->glGetIntegerv(GL_RENDERBUFFER_BINDING, &save_rb);
        CHECK_GL_ERROR
        GLint save_fb;
        f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &save_fb);
        CHECK_GL_ERROR
        f->glGenFramebuffers(1, &self->glfb);
        CHECK_GL_ERROR
        f->glBindFramebuffer(GL_FRAMEBUFFER, self->glfb);
        CHECK_GL_ERROR

        if (self->p->color) {
            f->glGenRenderbuffers(1, &self->color);
            CHECK_GL_ERROR
            f->glBindRenderbuffer(GL_RENDERBUFFER, self->color);
            CHECK_GL_ERROR
            EGLImageKHR image = slbuf_get_image(self);
            if (image == EGL_NO_IMAGE_KHR)
                return 0;
            //XXX should check for extension GL_OES_EGL_image
            f->glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
                                                      image);
            CHECK_GL_ERROR
            f->glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                         GL_COLOR_ATTACHMENT0_EXT,
                                         GL_RENDERBUFFER,
                                         self->color);
            CHECK_GL_ERROR
        }

        if (self->p->depth || self->p->stencil) {
            f->glGenRenderbuffers(1, &self->depth_stencil);
            CHECK_GL_ERROR
            f->glBindRenderbuffer(GL_RENDERBUFFER, self->depth_stencil);
            CHECK_GL_ERROR
            f->glRenderbufferStorage(GL_RENDERBUFFER,
                                     self->p->depth_stencil_format,
                                     self->p->width,
                                     self->p->height);
            CHECK_GL_ERROR
        }

        if (self->p->depth) {
            f->glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                         GL_DEPTH_ATTACHMENT,
                                         GL_RENDERBUFFER,
                                         self->depth_stencil);
            CHECK_GL_ERROR
        }

        if (self->p->stencil) {
            f->glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                         GL_STENCIL_ATTACHMENT,
                                         GL_RENDERBUFFER,
                                         self->depth_stencil);
            CHECK_GL_ERROR
        }

        prt("slbuf %p fb %u color %u depth/stencil %u\n", self, self->glfb, self->color, self->depth_stencil);

        GLenum fb_status = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        CHECK_GL_ERROR
        if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
            prt("incomplete fb\n");
            return 0;
        }

        f->glBindRenderbuffer(GL_RENDERBUFFER, save_rb);
        CHECK_GL_ERROR
        f->glBindFramebuffer(GL_FRAMEBUFFER, save_fb);
        CHECK_GL_ERROR
    }

    return self->glfb;

gl_error:
    return 0;
}

static void
slbuf_free_glfb(struct slbuf *self)
{
    struct slbuf_func *f = self->f;

    prt("cleanup fb %u\n",self->glfb);
    f->glDeleteFramebuffers(1, &self->glfb);
    CHECK_GL_ERROR
    f->glDeleteRenderbuffers(1, &self->color);
    CHECK_GL_ERROR
    f->glDeleteRenderbuffers(1, &self->depth_stencil);
    CHECK_GL_ERROR
    self->glfb = 0;
gl_error:
    return;
}

void
slbuf_finish(struct slbuf *self)
{
    if (self->glfb)
        self->f->glFinish();
}

void
slbuf_flush(struct slbuf *self)
{
    if (self->glfb)
        self->f->glFlush();
}

static GLuint
shader(struct slbuf_func *f, GLenum type, const char *src)
{
    GLuint shader = f->glCreateShader(type);
    CHECK_GL_ERROR
    assert(shader);
    f->glShaderSource(shader, 1, &src, NULL);
    CHECK_GL_ERROR

    GLchar buf[999];
    GLsizei len;
    GLint compiled;

    f->glCompileShader(shader);
    CHECK_GL_ERROR
    f->glGetShaderInfoLog(shader, sizeof(buf), &len, buf);
    CHECK_GL_ERROR
    prt("shader log: %s\n", buf);
    f->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    CHECK_GL_ERROR
    assert(compiled);
    return shader;

gl_error:
    return 0;
}

// NOTE: changes the current program
static GLuint
slbuf_get_program(struct slbuf *self)
{
    if (!program) {
        struct slbuf_func *f = self->f;
        const GLchar *vertex_source =
            "attribute vec2 pos;"
            "varying vec2 texcoord;"
            "void main() {"
            "    gl_Position = vec4(pos.x*2.-1., 1.-pos.y*2., 0, 1);"
            "    texcoord = pos; }";
        const GLchar *fragment_source =
            "uniform sampler2D tex;"
            "precision mediump float;"
            "varying vec2 texcoord;"
            "void main() { gl_FragColor = texture2D(tex, texcoord); }";

        vertex_shader = shader(self->f, GL_VERTEX_SHADER, vertex_source);
        fragment_shader = shader(self->f, GL_FRAGMENT_SHADER, fragment_source);

        program = f->glCreateProgram();
        CHECK_GL_ERROR
        f->glAttachShader(program, vertex_shader);
        CHECK_GL_ERROR
        f->glAttachShader(program, fragment_shader);
        CHECK_GL_ERROR
        f->glBindAttribLocation(program, 0, "pos");
        CHECK_GL_ERROR
        f->glLinkProgram(program);
        CHECK_GL_ERROR
        GLint linked;
        f->glGetProgramiv(program, GL_LINK_STATUS, &linked);
        CHECK_GL_ERROR
        assert(linked);
        GLint tex = f->glGetUniformLocation(program, "tex");
        CHECK_GL_ERROR
        f->glUseProgram(program);
        CHECK_GL_ERROR
        f->glUniform1i(tex, 0);
        CHECK_GL_ERROR
    }

    return program;

gl_error:
    return 0;
}

static void
slbuf_free_program(struct slbuf *self)
{
    if (program) {
        struct slbuf_func *f = self->f;

        f->glDeleteProgram(program);
        CHECK_GL_ERROR
        f->glDeleteShader(vertex_shader);
        CHECK_GL_ERROR
        f->glDeleteShader(fragment_shader);
        CHECK_GL_ERROR
        program = 0;
    }
gl_error:
    return;
}

// Return the contents of given buffer as a texture.
// NOTE: changes:
//         GL_FRAMEBUFFER binding,
//         GL_TEXTURE_2D binding in the active texture unit.
static GLuint
slbuf_get_texture_copy(struct slbuf *self)
{
    struct slbuf_func *f = self->f;

    if (!self->texture) {
        f->glGenTextures(1, &self->texture);
        CHECK_GL_ERROR
        f->glBindTexture(GL_TEXTURE_2D, self->texture);
        CHECK_GL_ERROR
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        CHECK_GL_ERROR
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        CHECK_GL_ERROR
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        CHECK_GL_ERROR
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        CHECK_GL_ERROR
    }

    assert(self->glfb);
    prt("copy texture from fb %u\n", self->glfb);
    f->glBindFramebuffer(GL_FRAMEBUFFER, self->glfb);
    CHECK_GL_ERROR
    f->glBindTexture(GL_TEXTURE_2D, self->texture);
    CHECK_GL_ERROR
    f->glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, self->p->width, self->p->height, 0);
    CHECK_GL_ERROR

    return self->texture;

gl_error:
    return 0;
}

//XXX untested
static GLuint
slbuf_get_texture_image(struct slbuf *self)
{
    if (!self->texture) {
        struct slbuf_func *f = self->f;

        f->glGenTextures(1, &self->texture);
        CHECK_GL_ERROR
        f->glBindTexture(GL_TEXTURE_2D, self->texture);
        CHECK_GL_ERROR
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                        self->p->width, self->p->height, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        CHECK_GL_ERROR
        f->glBindTexture(GL_TEXTURE_2D, self->texture);
        CHECK_GL_ERROR
        f->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, slbuf_get_image(self));
        CHECK_GL_ERROR
    }
    return self->texture;

gl_error:
    return 0;
}

static void
slbuf_free_texture(struct slbuf *self)
{
    if (self->texture) {
        self->f->glDeleteTextures(1, &self->texture);
        self->texture = 0;
    }
}

static struct slbuf*
slbuf_create(struct slbuf_param *param, struct slbuf_func *func)
{
    struct slbuf *buf = wcore_calloc(sizeof(*buf));
    if (!buf)
        return NULL;

    buf->p = param;
    buf->f = func;
    buf->dmabuf = -1;
    buf->image = EGL_NO_IMAGE_KHR;
    return buf;
}

void
slbuf_destroy(struct slbuf *self)
{
    if (!self)
        return;
    slbuf_free_texture(self);
    slbuf_free_glfb(self);
    slbuf_free_program(self);
    slbuf_free_image(self);
    slbuf_free_dmabuf(self);
    slbuf_free_drmfb(self);
    slbuf_free_bo(self);
    free(self);
}

void
slbuf_free_gl_resources(struct slbuf *self)
{
    if (!self)
        return;
    slbuf_free_texture(self);
    slbuf_free_glfb(self);
    slbuf_free_program(self);
}

bool
slbuf_bind_fb(struct slbuf *self)
{
    if (!(self->p->color || self->p->depth || self->p->stencil))
        return true;

    GLuint fb = slbuf_get_glfb(self);
    if (!fb)
        return false;

    struct slbuf_func *f = self->f;
    f->glBindFramebuffer(GL_FRAMEBUFFER, fb);
    CHECK_GL_ERROR
    return true;

gl_error:
    return false;
}

static bool
slbuf_available(struct slbuf *self)
{
    return self->display == NULL;
}

void
slbuf_set_display(struct slbuf *self, struct wnull_display *display)
{
    self->display = display;
}

uint32_t
slbuf_gbm_format(struct slbuf *self)
{
    return self->p->gbm_format;
}

uint32_t
slbuf_drm_format(struct slbuf *self)
{
    switch(slbuf_gbm_format(self)) {
        case GBM_FORMAT_XRGB8888:
            return DRM_FORMAT_XRGB8888;
        case GBM_FORMAT_ARGB8888:
            return DRM_FORMAT_ARGB8888;
        case GBM_FORMAT_XRGB2101010:
            return DRM_FORMAT_XRGB2101010;
        case GBM_FORMAT_ARGB2101010:
            return DRM_FORMAT_ARGB2101010;
        case GBM_FORMAT_RGB565:
            return DRM_FORMAT_RGB565;
    }
    assert(!"unexpected gbm format");
    return 0;
}

// Return the first buffer in 'array' into which we can draw (because it
// is not currently, nor pending to go, on screen).
// If there is no available buffer but there is an empty (NULL) slot in
// the array, a new buffer will be created with the given parameters and
// function table.
struct slbuf*
slbuf_get_buffer(struct slbuf *array[],
                 unsigned len,
                 struct slbuf_param *param,
                 struct slbuf_func *func)
{
    for (unsigned i = 0; i < len; ++i) {
        if (!array[i])
            return array[i] = slbuf_create(param, func);
        else if (slbuf_available(array[i]))
            return array[i];
    }
    return NULL;
}

bool
slbuf_copy_i915(struct slbuf *dst, struct slbuf *src)
{
    struct drm_i915_gem_get_tiling dst_tiling = {
        .handle = slbuf_handle(dst),
    };
    struct drm_i915_gem_get_tiling src_tiling = {
        .handle = slbuf_handle(src),
    };

    int dst_fd = slbuf_drmfd(dst);
    int src_fd = slbuf_drmfd(src);

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

    unsigned dst_step = slbuf_stride(dst) * rows;
    unsigned src_step = slbuf_stride(src) * rows;
    unsigned copy_size = MIN(dst_step, src_step);
    // round up so as not to omit a partly filled tile at the end
    unsigned num_copy = (MIN(dst->p->height, src->p->height) + rows - 1) / rows;

    void *tmp = malloc(copy_size);
    if (!tmp)
        return false;

    struct drm_i915_gem_pread pread = {
        .handle = slbuf_handle(src),
        .size = copy_size,
        .offset = 0,
        .data_ptr = (uint64_t) (uintptr_t) tmp,
    };

    struct drm_i915_gem_pwrite pwrite = {
        .handle = slbuf_handle(dst),
        .size = copy_size,
        .offset = 0,
        .data_ptr = (uint64_t) (uintptr_t) tmp,
    };

    // blit on gpu must be faster than this, but seems complicated to do
    bool ok = true;
    for (unsigned i = 0; ok && i < num_copy; ++i) {
        ok = !(drmIoctl(src_fd, DRM_IOCTL_I915_GEM_PREAD, &pread) ||
               drmIoctl(dst_fd, DRM_IOCTL_I915_GEM_PWRITE, &pwrite));
        pread.offset += src_step;
        pwrite.offset += dst_step;
    }
    free(tmp);
    return ok;
}

struct gl_values {
    GLfloat clear_color[4];
    GLint fb;
    GLint program;
    GLint blend;
    GLint cull;
    GLint depth;
    GLint scissor;
    GLint stencil;
    GLint active_texture;
    GLint texture0;
    GLint array_buffer;
    GLint viewport[4];
    struct {
        GLint buffer_binding;
        GLint enabled;
        GLint size;
        GLint type;
        GLint normalized;
        GLint stride;
        const GLvoid *pointer;
    } attrib0;
};

static void
get_gl_values(struct slbuf_func *f, struct gl_values *c) {
    f->glGetFloatv(GL_COLOR_CLEAR_VALUE, c->clear_color);
    CHECK_GL_ERROR
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &c->fb);
    CHECK_GL_ERROR
    f->glGetIntegerv(GL_CURRENT_PROGRAM, &c->program);
    CHECK_GL_ERROR
    f->glGetIntegerv(GL_BLEND, &c->blend);
    CHECK_GL_ERROR
    f->glGetIntegerv(GL_CULL_FACE, &c->cull);
    CHECK_GL_ERROR
    f->glGetIntegerv(GL_DEPTH_TEST, &c->depth);
    CHECK_GL_ERROR
    f->glGetIntegerv(GL_SCISSOR_TEST, &c->scissor);
    CHECK_GL_ERROR
    f->glGetIntegerv(GL_STENCIL_TEST, &c->stencil);
    CHECK_GL_ERROR
    f->glGetIntegerv(GL_ACTIVE_TEXTURE, &c->active_texture);
    CHECK_GL_ERROR

    if (c->active_texture != GL_TEXTURE0) {
        f->glActiveTexture(GL_TEXTURE0);
        CHECK_GL_ERROR
    }
    f->glGetIntegerv(GL_TEXTURE_BINDING_2D, &c->texture0);
    CHECK_GL_ERROR
    if (c->active_texture != GL_TEXTURE0) {
        f->glActiveTexture(c->active_texture);
        CHECK_GL_ERROR
    }

    f->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &c->array_buffer);
    CHECK_GL_ERROR

    f->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                                         &c->attrib0.buffer_binding);
    CHECK_GL_ERROR
    f->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                                         &c->attrib0.enabled);
    CHECK_GL_ERROR
    f->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE,
                                         &c->attrib0.size);
    CHECK_GL_ERROR
    f->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                                         &c->attrib0.stride);
    CHECK_GL_ERROR
    f->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE,
                                         &c->attrib0.type);
    CHECK_GL_ERROR
    f->glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                                         &c->attrib0.normalized);
    CHECK_GL_ERROR
    f->glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                    (GLvoid **)&c->attrib0.pointer);

    CHECK_GL_ERROR
    f->glGetIntegerv(GL_VIEWPORT, c->viewport);
    CHECK_GL_ERROR

gl_error:
    return;
}

static void
able(struct slbuf_func *f, GLenum cap, GLboolean val)
{
    if (val)
        f->glEnable(cap);
    else
        f->glDisable(cap);
}

static void
set_gl_values(struct slbuf_func *f, const struct gl_values *c)
{
    f->glClearColor(c->clear_color[0],
                    c->clear_color[1],
                    c->clear_color[2],
                    c->clear_color[3]);
    CHECK_GL_ERROR
    f->glBindFramebuffer(GL_FRAMEBUFFER, c->fb);
    CHECK_GL_ERROR
    f->glUseProgram(c->program);
    CHECK_GL_ERROR
    able(f, GL_BLEND, c->blend);
    CHECK_GL_ERROR
    able(f, GL_CULL_FACE, c->cull);
    CHECK_GL_ERROR
    able(f, GL_DEPTH_TEST, c->depth);
    CHECK_GL_ERROR
    able(f, GL_SCISSOR_TEST, c->scissor);
    CHECK_GL_ERROR
    able(f, GL_STENCIL_TEST, c->stencil);
    CHECK_GL_ERROR

    f->glActiveTexture(GL_TEXTURE0);
    CHECK_GL_ERROR
    f->glBindTexture(GL_TEXTURE_2D, c->texture0);
    CHECK_GL_ERROR
    if (c->texture0 != GL_TEXTURE0) {
        f->glActiveTexture(c->active_texture);
        CHECK_GL_ERROR
    }

    f->glBindBuffer(GL_ARRAY_BUFFER, c->attrib0.buffer_binding);
    CHECK_GL_ERROR
    f->glVertexAttribPointer(0,
                             c->attrib0.size,
                             c->attrib0.type,
                             c->attrib0.normalized,
                             c->attrib0.stride,
                             c->attrib0.pointer);
    CHECK_GL_ERROR

    if (c->attrib0.enabled)
        f->glEnableVertexAttribArray(0);
    else
        f->glDisableVertexAttribArray(0);
    CHECK_GL_ERROR

    f->glBindBuffer(GL_ARRAY_BUFFER, c->array_buffer);
    CHECK_GL_ERROR

    f->glViewport(c->viewport[0], c->viewport[1],
                  c->viewport[2], c->viewport[3]);
    CHECK_GL_ERROR

gl_error:
    return;
}

// Draw a quad into 'dst' using 'src' as a texture.
//TODO Maybe it would be better to use another context to do our rendering.
//     We could avoid saving and restoring state.
bool
slbuf_copy_gl(struct slbuf *dst, struct slbuf *src)
{
    struct slbuf_func *f = dst->f;
    struct gl_values gl_save;

    GLuint dst_fb = slbuf_get_glfb(dst);
    if (!dst_fb)
        return false;

    get_gl_values(f, &gl_save);

    GLuint program = slbuf_get_program(dst);
    if (!program)
        return false;

    f->glActiveTexture(GL_TEXTURE0);
    CHECK_GL_ERROR

    GLuint texture = slbuf_get_texture_copy(src);
    if (!texture)
        return false;

    static const GLfloat vertex[] = {
        0,1,
        1,1,
        0,0,
        1,0,
    };
    const struct gl_values my_values = {
        .program = program,
        .active_texture = GL_TEXTURE0,
        .texture0 = texture,
        .fb = dst_fb,
        .viewport = { 0, 0, src->p->width, src->p->height },
        .attrib0 = { 0, true, 2, GL_FLOAT, GL_FALSE, 0, vertex },
    };
    set_gl_values(f, &my_values);

    f->glClear(GL_COLOR_BUFFER_BIT);
    CHECK_GL_ERROR
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL_ERROR

    set_gl_values(f, &gl_save);
    return true;

gl_error:
    return false;
}
