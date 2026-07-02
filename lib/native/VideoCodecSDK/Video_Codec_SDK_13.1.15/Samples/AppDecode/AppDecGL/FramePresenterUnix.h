/*
* Copyright 2017-2026 NVIDIA Corporation.  All rights reserved.
*
* Please refer to the NVIDIA end user license agreement (EULA) associated
* with this source code for terms and conditions that govern your use of
* this software. Any use, reproduction, disclosure, or distribution of
* this software and related documentation outside the terms of the EULA
* is strictly prohibited.
*
*/
#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glx.h>
#ifdef HAVE_WAYLAND_SUPPORT
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"
#endif
#include <iostream>
#include <thread>
#include <queue>
#include <cuda.h>
#include <cudaGL.h>
#include "../Utils/NvCodecUtils.h"
#include "FramePresenter.h"

#define BUFFER_COUNT 2

class FramePresenterUnix : public FramePresenter {

public:
    FramePresenterUnix(int w, int h);
    ~FramePresenterUnix();

    void initWindowSystem() override;
    void initOpenGLResources() override;
    void releaseWindowSystem() override;
    bool isVendorNvidia() override;
    void Render() override;
    bool GetDeviceFrameBuffer(CUdeviceptr*, int *) override;
    void ReleaseDeviceFrameBuffer() override;
    int getWidth() override { return this->width; }
    int getHeight() { return this->height; }

    bool windowShouldClose = false;
#ifdef HAVE_WAYLAND_SUPPORT
    bool surfaceConfigured = false;
    void registry_handle_global(struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    void resizeEglWindow(int32_t width, int32_t height);
#endif
    
    ConcurrentQueue<int> frameFeeder;

private:
    void initGLX();
    void releaseGLX();
    void RenderGLX();
#ifdef HAVE_WAYLAND_SUPPORT
    void initWayland();
    void releaseWayland();
    void RenderWayland();
#endif
    
    void mapBufferObject(CUdeviceptr* dpBuffer);
    void unmapBufferObject();
    
    void setDimensions(unsigned int w, unsigned int h) {
        this->width = w;
        this->height = h;
    }

    CUgraphicsResource cuResource[BUFFER_COUNT];
    GLuint pbo[BUFFER_COUNT];
    GLuint tex[BUFFER_COUNT];
    GLuint program;
    GLsync frameSync[BUFFER_COUNT] = {nullptr, nullptr};

    Display *display = nullptr;
    Window win = 0;
    GLXContext ctx = 0;
    GLXContext shared_ctx = 0;
    Colormap cmap = 0;

#ifdef HAVE_WAYLAND_SUPPORT
    struct wl_display *wl_display = nullptr;
    struct wl_registry *wl_registry = nullptr;
    struct wl_compositor *wl_compositor = nullptr;
    struct wl_surface *wl_surface = nullptr;
    struct xdg_wm_base *xdg_wm_base = nullptr;
    struct xdg_surface *xdg_surface = nullptr;
    struct xdg_toplevel *xdg_toplevel = nullptr;
    struct wl_egl_window *wl_egl_window = nullptr;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLContext egl_shared_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;

    bool bIsWayland = false;
#endif
    CUcontext cuContext;
    int currentFrame = 0;
    
    NvThread renderingThread;
};
