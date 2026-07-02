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

#include "FramePresenterUnix.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

#include "../Utils/NvCodecUtils.h"

#ifdef HAVE_WAYLAND_SUPPORT
// Wayland callbacks
static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    FramePresenterUnix *presenter = (FramePresenterUnix *)data;
    presenter->registry_handle_global(registry, name, interface, version);
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    FramePresenterUnix *presenter = (FramePresenterUnix *)data;
    xdg_surface_ack_configure(xdg_surface, serial);
    presenter->surfaceConfigured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states) {
    FramePresenterUnix *presenter = (FramePresenterUnix *)data;
    if (width > 0 && height > 0) {
        presenter->resizeEglWindow(width, height);
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    FramePresenterUnix *presenter = (FramePresenterUnix *)data;
    presenter->windowShouldClose = true;
    presenter->endOfDecoding = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    xdg_toplevel_configure,
    xdg_toplevel_close,
};

void FramePresenterUnix::registry_handle_global(struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, "wl_compositor") == 0) {
        wl_compositor = (struct wl_compositor *)wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        xdg_wm_base = (struct xdg_wm_base *)wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, this);
    }
}
#endif

FramePresenterUnix::FramePresenterUnix(int w, int h) {
    frameFeeder.setSize(BUFFER_COUNT);

    currentFrame = 0;
    endOfDecoding = false;
    endOfRendering = false;
    windowShouldClose = false;
#ifdef HAVE_WAYLAND_SUPPORT
    bIsWayland = false;
    surfaceConfigured = false;
#endif

    cuResource[0] = 0;
    cuResource[1] = 0;

    setDimensions(w, h);

    initWindowSystem();
    initOpenGLResources();

    for (int i = 0; i < BUFFER_COUNT; i++) {
        ck(cuGraphicsGLRegisterBuffer(&cuResource[i], pbo[i], CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD));
    }

#ifdef HAVE_WAYLAND_SUPPORT
    if (bIsWayland) {
        eglBindAPI(EGL_OPENGL_API);
        if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_shared_context)) {
            std::cerr << "Failed to make shared context current in constructor. Error: " << eglGetError() << std::endl;
        }
    }
#endif

    renderingThread = NvThread(std::thread(&FramePresenterUnix::Render, this));
}

bool FramePresenterUnix::isVendorNvidia() {
    char * vendor = (char*) glGetString(GL_VENDOR);
    bool result = false;
    if (vendor && !strcmp(vendor, "NVIDIA Corporation")) {
        result = true;
    }
    return result;
}

bool FramePresenterUnix::GetDeviceFrameBuffer(CUdeviceptr *dpFrame, int *pitch) {
    mapBufferObject(dpFrame);
    *pitch = getWidth()*4;
    return true;
}

void FramePresenterUnix::ReleaseDeviceFrameBuffer() {
    unmapBufferObject();
}

void FramePresenterUnix::mapBufferObject(CUdeviceptr* dpBuffer) {
    ck(cuGraphicsMapResources(1, &cuResource[currentFrame], 0));

    size_t nSize = 0;
    ck(cuGraphicsResourceGetMappedPointer(dpBuffer, &nSize, cuResource[currentFrame]));
}

void FramePresenterUnix::unmapBufferObject() {
    ck(cuGraphicsUnmapResources(1, &cuResource[currentFrame], 0));
    ck(cuCtxSynchronize());
    
    if (frameSync[currentFrame] != nullptr) {
        glDeleteSync(frameSync[currentFrame]);
    }
    frameSync[currentFrame] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    frameFeeder.push_back(currentFrame);
    currentFrame = (currentFrame + 1) % BUFFER_COUNT;
}

void FramePresenterUnix::Render() {
#ifdef HAVE_WAYLAND_SUPPORT
    if (bIsWayland) {
        RenderWayland();
    } else {
        RenderGLX();
    }
#else
    RenderGLX();
#endif
    
    endOfRendering = true;
}

#ifdef HAVE_WAYLAND_SUPPORT
void FramePresenterUnix::RenderWayland() {
    int w = getWidth();
    int h = getHeight();

    if (egl_display == EGL_NO_DISPLAY || egl_context == EGL_NO_CONTEXT || egl_surface == EGL_NO_SURFACE) {
        std::cerr << "EGL resources invalid!" << std::endl;
        return;
    }
    
    eglBindAPI(EGL_OPENGL_API);
    
    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        std::cerr << "Failed to make EGL context current! Error: " << eglGetError() << std::endl;
        return;
    }

    glViewport(0, 0, w, h);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0);

    int currentRender = 0;

    while (!endOfDecoding && !windowShouldClose) {
        while (frameFeeder.empty() && !endOfDecoding && !windowShouldClose) {
            wl_display_dispatch_pending(wl_display);
            wl_display_flush(wl_display);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        if (endOfDecoding || windowShouldClose) {
            break;
        }
        
        wl_display_dispatch_pending(wl_display);
        wl_display_flush(wl_display);

        currentRender = frameFeeder.front();

        if (frameSync[currentRender] != nullptr) {
            GLenum waitResult = glClientWaitSync(frameSync[currentRender], 
                                                  GL_SYNC_FLUSH_COMMANDS_BIT, 
                                                  GL_TIMEOUT_IGNORED);
            if (waitResult == GL_WAIT_FAILED) {
                std::cerr << "glClientWaitSync failed!" << std::endl;
            }
            glDeleteSync(frameSync[currentRender]);
            frameSync[currentRender] = nullptr;
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[currentRender]);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[currentRender]);
        glTexSubImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, 0);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);

        glActiveTexture(GL_TEXTURE0);
        
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[currentRender]);
        glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, program);
        glEnable(GL_FRAGMENT_PROGRAM_ARB);
        glDisable(GL_DEPTH_TEST);

        glBegin(GL_QUADS);
        glTexCoord2f(0, (GLfloat)h);
        glVertex2f(0, 0);
        glTexCoord2f((GLfloat)w, (GLfloat)h);
        glVertex2f(1, 0);
        glTexCoord2f((GLfloat)w, 0);
        glVertex2f(1, 1);
        glTexCoord2f(0, 0);
        glVertex2f(0, 1);
        glEnd();
        
        glDisable(GL_FRAGMENT_PROGRAM_ARB);
        
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

        eglGetError(); // Clear errors
        EGLBoolean swapResult = eglSwapBuffers(egl_display, egl_surface);
        
        wl_surface_commit(wl_surface);
        wl_display_flush(wl_display);
        
        if (!swapResult) {
            EGLint error = eglGetError();
            if (error != EGL_SUCCESS && error != EGL_BAD_SURFACE) {
                std::cerr << "eglSwapBuffers failed! Error: " << error << std::endl;
            }
        }

        frameFeeder.pop_front();
    }
    
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}
#endif

void FramePresenterUnix::RenderGLX() {
    int w = getWidth();
    int h = getHeight();

    glXMakeCurrent(display, win, shared_ctx);
    
    glViewport(0, 0, w, h);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0);

    int currentRender = 0;

    while (!endOfDecoding && !windowShouldClose) {
        currentRender = frameFeeder.front();

        if (frameSync[currentRender] != nullptr) {
            GLenum waitResult = glClientWaitSync(frameSync[currentRender], 
                                                  GL_SYNC_FLUSH_COMMANDS_BIT, 
                                                  GL_TIMEOUT_IGNORED);
            if (waitResult == GL_WAIT_FAILED) {
                std::cerr << "glClientWaitSync failed!" << std::endl;
            }
            glDeleteSync(frameSync[currentRender]);
            frameSync[currentRender] = nullptr;
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[currentRender]);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[currentRender]);
        glTexSubImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, 0);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);

        glActiveTexture(GL_TEXTURE0);
        glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, program);
        glEnable(GL_FRAGMENT_PROGRAM_ARB);
        glDisable(GL_DEPTH_TEST);

        glBegin(GL_QUADS);
        glTexCoord2f(0, (GLfloat)h);
        glVertex2f(0, 0);
        glTexCoord2f((GLfloat)w, (GLfloat)h);
        glVertex2f(1, 0);
        glTexCoord2f((GLfloat)w, 0);
        glVertex2f(1, 1);
        glTexCoord2f(0, 0);
        glVertex2f(0, 1);
        glEnd();
        
        glDisable(GL_FRAGMENT_PROGRAM_ARB);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

        glXSwapBuffers(display, win);
        frameFeeder.pop_front();
    }
    
    glXMakeCurrent(display, 0, 0);
}

void FramePresenterUnix::releaseWindowSystem() {
#ifdef HAVE_WAYLAND_SUPPORT
    if (bIsWayland) {
        releaseWayland();
    } else {
        releaseGLX();
    }
#else
    releaseGLX();
#endif
}

#ifdef HAVE_WAYLAND_SUPPORT
void FramePresenterUnix::releaseWayland() {
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl_display, egl_context);
    if (egl_shared_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, egl_shared_context);
    }
    eglDestroySurface(egl_display, egl_surface);
    eglTerminate(egl_display);

    if (wl_egl_window) wl_egl_window_destroy(wl_egl_window);
    if (xdg_toplevel) xdg_toplevel_destroy(xdg_toplevel);
    if (xdg_surface) xdg_surface_destroy(xdg_surface);
    if (xdg_wm_base) xdg_wm_base_destroy(xdg_wm_base);
    if (wl_surface) wl_surface_destroy(wl_surface);
    if (wl_compositor) wl_compositor_destroy(wl_compositor);
    if (wl_registry) wl_registry_destroy(wl_registry);
    if (wl_display) wl_display_disconnect(wl_display);
}
#endif

void FramePresenterUnix::releaseGLX() {
    glXMakeCurrent(display, 0, 0);
    glXDestroyContext(display, ctx);
    glXDestroyContext(display, shared_ctx);

    XDestroyWindow(display, win);
    XFreeColormap(display, cmap);
    XCloseDisplay(display);
}

void FramePresenterUnix::initWindowSystem() {
#ifdef HAVE_WAYLAND_SUPPORT
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display) {
        bIsWayland = true;
        initWayland();
    } else {
        bIsWayland = false;
        initGLX();
    }
#else
    initGLX();
#endif
}

#ifdef HAVE_WAYLAND_SUPPORT
void FramePresenterUnix::initWayland() {
    wl_display = wl_display_connect(NULL);
    if (!wl_display) {
        std::cerr << "Failed to connect to Wayland display" << std::endl;
        exit(1);
    }

    wl_registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(wl_registry, &registry_listener, this);
    wl_display_roundtrip(wl_display);

    if (!wl_compositor) {
        std::cerr << "Failed to find wl_compositor" << std::endl;
        exit(1);
    }
    if (!xdg_wm_base) {
        std::cerr << "Failed to find xdg_wm_base. Please ensure your compositor supports xdg-shell." << std::endl;
        exit(1);
    }

    wl_surface = wl_compositor_create_surface(wl_compositor);
    xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wl_surface);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, this);
    
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, this);
    xdg_toplevel_set_title(xdg_toplevel, "AppDecGL");
    
    wl_surface_commit(wl_surface);
    
    while (!surfaceConfigured) {
        wl_display_dispatch(wl_display);
    }

    egl_display = eglGetDisplay((EGLNativeDisplayType)wl_display);
    if (egl_display == EGL_NO_DISPLAY) {
        std::cerr << "Failed to get EGL display" << std::endl;
        exit(1);
    }

    if (!eglInitialize(egl_display, NULL, NULL)) {
        std::cerr << "Failed to initialize EGL" << std::endl;
        exit(1);
    }

    eglBindAPI(EGL_OPENGL_API);

    EGLint attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,  // No alpha to prevent Wayland compositor transparency
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_config;
    eglChooseConfig(egl_display, attributes, &config, 1, &num_config);
    if (num_config == 0) {
        std::cerr << "Failed to choose EGL config" << std::endl;
        exit(1);
    }

    egl_shared_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, NULL);
    if (egl_shared_context == EGL_NO_CONTEXT) {
        std::cerr << "Failed to create shared EGL context" << std::endl;
        exit(1);
    }

    egl_context = eglCreateContext(egl_display, config, egl_shared_context, NULL);
    if (egl_context == EGL_NO_CONTEXT) {
        std::cerr << "Failed to create EGL render context" << std::endl;
        exit(1);
    }
    
    int32_t window_width = width;
    int32_t window_height = height;
    
    if (window_width <= 0) window_width = 640;
    if (window_height <= 0) window_height = 480;
    
    wl_egl_window = wl_egl_window_create(wl_surface, window_width, window_height);
    if (!wl_egl_window) {
        std::cerr << "Failed to create wl_egl_window" << std::endl;
        exit(1);
    }
    
    egl_surface = eglCreateWindowSurface(egl_display, config, (EGLNativeWindowType)wl_egl_window, NULL);
    if (egl_surface == EGL_NO_SURFACE) {
        std::cerr << "Failed to create EGL surface. Error: " << eglGetError() << std::endl;
        exit(1);
    }

    if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_shared_context)) {
        std::cerr << "Warning: Failed to make shared context current without surface. Error: " << eglGetError() << std::endl;
        
        EGLint pbufferAttribs[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };
        EGLSurface pbuffer = eglCreatePbufferSurface(egl_display, config, pbufferAttribs);
        if (pbuffer != EGL_NO_SURFACE) {
             if (!eglMakeCurrent(egl_display, pbuffer, pbuffer, egl_shared_context)) {
                 std::cerr << "Failed to make shared context current with Pbuffer. Error: " << eglGetError() << std::endl;
                 eglDestroySurface(egl_display, pbuffer);
                 exit(1);
             }
             eglDestroySurface(egl_display, pbuffer);
        } else {
             std::cerr << "Failed to create fallback Pbuffer surface. Error: " << eglGetError() << std::endl;
             exit(1);
        }
    }
    
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    EGLBoolean swapResult = eglSwapBuffers(egl_display, egl_surface);
    if (!swapResult) {
        std::cerr << "Initial eglSwapBuffers failed! Error: " << eglGetError() << std::endl;
    }
    wl_surface_commit(wl_surface);
    wl_display_flush(wl_display);
    
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_shared_context);
}
#endif

void FramePresenterUnix::initGLX() {
    XInitThreads();

	XVisualInfo *visinfo;
	GLXFBConfig config;

    display = XOpenDisplay(NULL);
    if (display == NULL) {
		std::cout << "\nDisplay not found ! Make sure X server is running and DISPLAY environment variable set appropriately !\n";
		exit(1);
    }

    int configAttr[] = {
        GLX_CONFIG_CAVEAT   ,GLX_NONE,
        GLX_RENDER_TYPE     ,GLX_RGBA_BIT,
        GLX_RED_SIZE        , 8,
        GLX_GREEN_SIZE      , 8,
        GLX_BLUE_SIZE       , 8,
        GLX_ALPHA_SIZE      , 8,
        GLX_DEPTH_SIZE      , 24,
        GLX_STENCIL_SIZE    , 8,
        GLX_DOUBLEBUFFER    , True,
        None,
    };

    GLXFBConfig *configs = NULL;
    int numConfigs = 0;

	int screen = DefaultScreen(display);

    configs = glXChooseFBConfig(display, screen, configAttr, &numConfigs);
    if (numConfigs <= 0 || configs == NULL) {
        std::cout << "\nFailed to find a suitable GLXFBConfig!\n";
		exit(1);
    }

    config = configs[0];
    XFree(configs);

    visinfo = glXGetVisualFromFBConfig(display, config);
    if (!visinfo) {
        std::cout << "\nFailed to find a suitable visual!\n";
        exit(1);
    }

    Window root;
    XSetWindowAttributes wattr;
    int wattr_mask;

    root = RootWindow(display, screen);

    cmap = XCreateColormap(display, root, visinfo->visual, AllocNone);

    if (!cmap) {
        std::cout << "\nFailed to create colormap!\n";
        exit(1);
    }

    wattr_mask = CWBackPixmap | CWBorderPixel | CWColormap;
    wattr.background_pixmap = None;
    wattr.border_pixel = 0;
    wattr.bit_gravity = StaticGravity;
    wattr.colormap = cmap;

    int window_width = width > 0 ? width : 640;
    int window_height = height > 0 ? height : 480;
    win = XCreateWindow(display, root, 0, 0, window_width, window_height, 0,
                            visinfo->depth, InputOutput,
                            visinfo->visual, wattr_mask, &wattr);

    if (!win) {
        std::cout << "\nFailed to create window!\n";
		exit(1);
    }

	XMapWindow(display, win);

	ctx = glXCreateNewContext(display, config, GLX_RGBA_TYPE, 0, True);

	if (!ctx) {
		std::cout << "\nFailed to create GLX context !\n";
		exit(1);
	}

    shared_ctx = glXCreateNewContext(display, config, GLX_RGBA_TYPE, ctx, True);

	if (!shared_ctx) {
		std::cout << "\nFailed to create shared GLX context !\n";
		exit(1);
	}

    glXMakeCurrent(display, win, ctx);
}

void FramePresenterUnix::initOpenGLResources() {
    glewInit();

    glGenTextures(BUFFER_COUNT, tex);
    glGenBuffers(BUFFER_COUNT, pbo);

    for (int i=0; i<BUFFER_COUNT; i++) {
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pbo[i]);
        glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, width * height * 4, NULL, GL_DYNAMIC_DRAW_ARB);
        glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);

        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex[i]);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);
    }

    static const char *code =
        "!!ARBfp1.0\n"
        "TEX result.color, fragment.texcoord, texture[0], RECT; \n"
        "MOV result.color.a, 1.0; \n"
        "END";

    glGenProgramsARB(1, &program);
    glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, program);
    glProgramStringARB(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen(code), (GLubyte *)code);
    
    GLint errorPos;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &errorPos);
    if (errorPos != -1) {
        const GLubyte *errorString = glGetString(GL_PROGRAM_ERROR_STRING_ARB);
        std::cerr << "Fragment program error at position " << errorPos << ": " << errorString << std::endl;
    }
}

#ifdef HAVE_WAYLAND_SUPPORT
void FramePresenterUnix::resizeEglWindow(int32_t width, int32_t height) {
    if (wl_egl_window && bIsWayland) {
        wl_egl_window_resize(wl_egl_window, width, height, 0, 0);
    }
}
#endif

FramePresenterUnix::~FramePresenterUnix() {
    endOfDecoding = true;

    while (!endOfRendering) {
    }

    renderingThread.join();

#ifdef HAVE_WAYLAND_SUPPORT
    if (bIsWayland) {
        eglBindAPI(EGL_OPENGL_API);
        if (eglGetCurrentContext() != egl_shared_context) {
            eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_shared_context);
        }
    }
#endif

    for (int i = 0; i < BUFFER_COUNT; i++) {
        ck(cuGraphicsUnregisterResource(cuResource[i]));
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (frameSync[i] != nullptr) {
            glDeleteSync(frameSync[i]);
            frameSync[i] = nullptr;
        }
    }

    glDeleteBuffersARB(BUFFER_COUNT, pbo);
    glDeleteTextures(BUFFER_COUNT, tex);
    glDeleteProgramsARB(1, &program);

#ifdef HAVE_WAYLAND_SUPPORT
    if (bIsWayland) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
#endif

    releaseWindowSystem();
}
