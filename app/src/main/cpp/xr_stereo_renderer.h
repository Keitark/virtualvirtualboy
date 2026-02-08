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
    struct ControllerState {
        bool left = false;
        bool right = false;
        bool up = false;
        bool down = false;
        bool a = false;
        bool b = false;
        bool x = false;
        bool y = false;
        bool l = false;
        bool r = false;
        bool leftThumbClick = false;
        bool rightThumbClick = false;
        bool start = false;
        bool select = false;
    };

    bool initialize(ANativeActivity* activity);
    void shutdown();

    void pollEvents();
    void updateFrame(const uint32_t* pixels, int width, int height);
    bool renderFrame();
    bool getControllerState(ControllerState& outState) const;
    void setPresentationConfig(float screenScale, float stereoConvergence);
    [[nodiscard]] float screenScale() const { return screenScale_; }
    [[nodiscard]] float stereoConvergence() const { return stereoConvergence_; }

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
    bool createInputActions();
    bool suggestInteractionBindings();
    bool createReferenceSpace();
    bool createSwapchains();
    bool createGlResources();
    bool beginSession();
    void endSession();
    void destroySwapchains();
    void destroyInputActions();
    void syncInput();

    bool makeCurrent();

    bool getBooleanActionState(XrAction action, XrPath subactionPath = XR_NULL_PATH) const;
    float getFloatActionState(XrAction action, XrPath subactionPath = XR_NULL_PATH) const;
    bool getVector2ActionState(
        XrAction action, XrVector2f& outValue, XrPath subactionPath = XR_NULL_PATH) const;

    ANativeActivity* activity_ = nullptr;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace appSpace_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrPath leftHandPath_ = XR_NULL_PATH;
    XrPath rightHandPath_ = XR_NULL_PATH;
    XrPath oculusTouchProfilePath_ = XR_NULL_PATH;
    XrPath khrSimpleProfilePath_ = XR_NULL_PATH;

    XrAction moveAction_ = XR_NULL_HANDLE;
    XrAction leftSqueezeAction_ = XR_NULL_HANDLE;
    XrAction rightSqueezeAction_ = XR_NULL_HANDLE;
    XrAction leftTriggerAction_ = XR_NULL_HANDLE;
    XrAction rightTriggerAction_ = XR_NULL_HANDLE;
    XrAction leftThumbClickAction_ = XR_NULL_HANDLE;
    XrAction rightThumbClickAction_ = XR_NULL_HANDLE;
    XrAction buttonAAction_ = XR_NULL_HANDLE;
    XrAction buttonBAction_ = XR_NULL_HANDLE;
    XrAction buttonXAction_ = XR_NULL_HANDLE;
    XrAction buttonYAction_ = XR_NULL_HANDLE;
    XrAction menuAction_ = XR_NULL_HANDLE;

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
    float screenScale_ = 0.68f;
    float stereoConvergence_ = 0.016f;
    ControllerState controllerState_{};

    std::string lastError_;
};
