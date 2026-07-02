////////////////////////////////////////////////////////////////////////////
//
// Copyright 2020-2026 NVIDIA Corporation.  All rights reserved.
//
// Please refer to the NVIDIA end user license agreement (EULA) associated
// with this source code for terms and conditions that govern your use of
// this software. Any use, reproduction, disclosure, or distribution of
// this software and related documentation outside the terms of the EULA
// is strictly prohibited.
//
////////////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string.h>
#include <X11/Xlib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "../Utils/Logger.h"
#include <GL/glew.h>
#include <GL/glut.h>
#include <GL/freeglut_ext.h>

void showErrorAndExit (const char* message)
{
    bool bThrowError = false;
    std::ostringstream oss;
    if (message)
    {
        bThrowError = true;
        oss << message << std::endl;
    }

    if (bThrowError)
    {
        throw std::invalid_argument(oss.str());
    }
    else
    {
        std::cout << oss.str();
        exit(0);
    }
}

// egl resources
static EGLDisplay    eglDisplay = EGL_NO_DISPLAY;
static EGLSurface    eglSurface = EGL_NO_SURFACE;
static EGLContext    eglContext = EGL_NO_CONTEXT;

// glut window handle
static int window;

// Initialization function to create a simple X window and associate EGLContext and EGLSurface with it
bool SetupEGLResources(int xpos, int ypos, int width, int height, const char *windowname)
{
    bool status = true;

    EGLint configAttrs[] =
    {
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_DEPTH_SIZE,      8,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLint contextAttrs[] =
    {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    EGLint pbufferAttribs[] = 
    {
        EGL_WIDTH,           16,
        EGL_HEIGHT,          16,
        EGL_NONE
    };

    EGLConfig* configList = NULL;
    EGLint configCount;

    EGLint numDevices;
    EGLint deviceIdx = -1;

    PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT =
      (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");

    if (!eglQueryDevicesEXT)
    {
        std::cout << "\nEGL : failed to get eglQueryDevicesEXT() symbol\n";
        return false;
    }

    PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT =
      (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");

    if (!eglQueryDeviceStringEXT)
    {
        std::cout << "\nEGL : failed to get eglQueryDeviceStringEXT() symbol\n";
        return false;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    if (!eglGetPlatformDisplayEXT)
    {
        std::cout << "\nEGL : failed to get eglGetPlatformDisplayEXT() symbol\n";
        return false;
    }

    eglQueryDevicesEXT(0, NULL, &numDevices);

    if (numDevices == 0)
    {
        std::cout << "\nEGL : failed to query EGL devices\n";
        return false;
    }

    EGLDeviceEXT *eglDevices = new EGLDeviceEXT[numDevices];

    if (!eglQueryDevicesEXT(numDevices, eglDevices, &numDevices))
    {
        std::cout << "\nEGL : failed to query EGL devices\n";
        delete[] eglDevices;
        return false;
    }

    for (EGLint i = 0; i < numDevices; ++i)
    {
        const char* vendorString = eglQueryDeviceStringEXT(eglDevices[i], EGL_VENDOR);
        if (vendorString && (strstr(vendorString, "NVIDIA") != NULL))
        {
            deviceIdx = i;
            break;
        }
    }

    if (deviceIdx == -1)
    {
        std::cout << "\nEGL : Nvidia EGL device not found\n";
        delete[] eglDevices;
        return false;
    }

    eglDisplay = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, eglDevices[deviceIdx], 0);

    if (eglDisplay == EGL_NO_DISPLAY)
    {
        std::cout << "\nEGL : failed to obtain display\n";
        delete[] eglDevices;
        return false;
    }

    delete[] eglDevices;

    if (!eglInitialize(eglDisplay, 0, 0))
    {
        std::cout << "\nEGL : failed to initialize\n";
        return false;
    }

    if (!eglChooseConfig(eglDisplay, configAttrs, NULL, 0, &configCount) || !configCount)
    {
        std::cout << "\nEGL : failed to return any matching configurations\n";
        return false;
    }

    configList = (EGLConfig*)malloc(configCount * sizeof(EGLConfig));

    if (!eglChooseConfig(eglDisplay, configAttrs, configList, configCount, &configCount) || !configCount)
    {
        std::cout << "\nEGL : failed to populate configuration list\n";
        status = false;
        goto end;
    }

    eglSurface = eglCreatePbufferSurface(eglDisplay, configList[0], pbufferAttribs);

    if (!eglSurface)
    {
        std::cout << "\nEGL : couldn't create window surface\n";
        status = false;
        goto end;
    }

    eglBindAPI(EGL_OPENGL_API);

    eglContext = eglCreateContext(eglDisplay, configList[0], NULL, contextAttrs);

    if (!eglContext)
    {
        std::cout << "\nEGL : couldn't create context\n";
        status = false;
        goto end;
    }

    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext))
    {
        std::cout << "\nEGL : couldn't make context/surface current\n";
        status = false;
        goto end;
    }

end:
    free(configList);
    return status;
}

// Cleanup function to destroy the window and context
void DestroyEGLResources()
{
    if (eglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (eglContext != EGL_NO_CONTEXT)
        {
            eglDestroyContext(eglDisplay, eglContext);
        }

        if (eglSurface != EGL_NO_SURFACE)
        {
            eglDestroySurface(eglDisplay, eglSurface);
        }

        eglTerminate(eglDisplay);
    }
}

bool SetupGLXResources()
{
    int argc = 1;
    char *argv[1] = {(char*)"dummy"};

    // Use glx context/surface
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGB | GLUT_SINGLE);
    glutInitWindowSize(16, 16);

    window = glutCreateWindow("AppEncGL");
    if (!window)
    {
        std::cout << "\nUnable to create GLUT window.\n" << std::endl;
        return false;
    }

    glutHideWindow();
    return true;
}

// Cleanup function to destroy glut resources
void DestroyGLXResources()
{
    glutDestroyWindow(window);
}

void GraphicsCloseWindow(const char* contextType)
{
    if (!strcmp(contextType, "egl"))
    {
        DestroyEGLResources();
    }
    else if (!strcmp(contextType, "glx"))
    {
        DestroyGLXResources();
    }
    else
    {
        std::cout << "\nInvalid context type specified.\n";
    }
}

void GraphicsSetupWindow(const char* contextType)
{
    if (!strcmp(contextType, "egl"))
    {
        // Use egl context/surface
        if (!SetupEGLResources(0, 0, 16, 16, "AppEncGL"))
        {
            showErrorAndExit("\nFailed to setup window.\n");
        }
    }
    else if (!strcmp(contextType, "glx"))
    {
        // Use glx context/surface
        if (!SetupGLXResources())
        {
            showErrorAndExit("\nFailed to setup window.\n");
        }
    }
    else
    {
        showErrorAndExit("\nInvalid context type specified.\n");
    }

    char * vendor = (char*) glGetString(GL_VENDOR);
    if (strcmp(vendor, "NVIDIA Corporation"))
    {
        GraphicsCloseWindow(contextType);
        showErrorAndExit("\nFailed to find NVIDIA libraries\n");
    }
}
