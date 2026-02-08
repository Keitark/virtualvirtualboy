#pragma once

#include <android/native_activity.h>
#include <cstdint>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#ifndef XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

class XrStereoRenderer {
public:
    bool initialize(ANativeActivity* activity);
    void shutdown();

    void pollEvents();
    void updateFrame(const uint32_t* pixels, int width, int height);
    bool renderFrame();

    [[nodiscard]] bool initialized() const { return initialized_; }
    [[nodiscard]] bool sessionRunning() const { return sessionRunning_; }
    [[nodiscard]] bool exitRequested() const { return exitRequested_; }
    [[nodiscard]] const char* lastError() const { return lastError_.c_str(); }

private:
    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
    };

    bool setError(const char* context, XrResult result);
    bool setErrorMessage(const char* message);

    bool initializeLoader();
    bool createInstance();
    bool createSystem();
    bool createEglContext();
    bool createSession();
    bool createReferenceSpace();
    bool createSwapchains();
    bool createGlResources();
    bool beginSession();
    void endSession();
    void destroySwapchains();

    bool makeCurrent();

    ANativeActivity* activity_ = nullptr;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;

    std::vector<XrViewConfigurationView> configViews_;
    std::vector<XrView> views_;
    std::vector<EyeSwapchain> eyeSwapchains_;

    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    EGLConfig eglConfig_ = nullptr;

    GLuint framebuffer_ = 0;
    GLuint emuTexture_ = 0;
    GLuint program_ = 0;

    GLint uniformTexture_ = -1;
    GLint uniformUvScale_ = -1;
    GLint uniformUvOffset_ = -1;

    bool initialized_ = false;
    bool sessionRunning_ = false;
    bool frameReady_ = false;
    bool exitRequested_ = false;
    bool sideBySideFrame_ = false;
    int frameWidth_ = 0;
    int frameHeight_ = 0;

    std::string lastError_;
};
