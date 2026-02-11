#pragma once

#include <android/native_activity.h>
#include <array>
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
        bool leftGrip = false;
        bool rightGrip = false;
        float leftStickX = 0.0f;
        float leftStickY = 0.0f;
        float rightStickX = 0.0f;
        float rightStickY = 0.0f;
        bool leftThumbClick = false;
        bool rightThumbClick = false;
        bool start = false;
        bool select = false;
    };

    struct RenderDebugState {
        bool xrActive = false;
        bool frameShouldRender = false;
        bool depthModeEnabled = false;
        bool metadataAligned = false;
        bool layerDataReady = false;
        bool overlayVisible = false;
        bool usedLayerRendering = false;
        bool usedDepthFallback = false;
        bool usedClassic = false;
        bool headOriginSet = false;
        float relativeX = 0.0f;
        float relativeY = 0.0f;
        float relativeZ = 0.0f;
    };

    bool initialize(ANativeActivity* activity);
    void shutdown();

    void pollEvents();
    void updateFrame(const uint32_t* pixels, int width, int height);
    void updateDepthMetadata(
        const int8_t* disparity,
        const uint8_t* worldIds,
        const int16_t* sourceX,
        const int16_t* sourceY,
        int width,
        int height,
        uint32_t frameId);
    bool renderFrame();
    bool getControllerState(ControllerState& outState) const;
    void setPresentationConfig(float screenScale, float stereoConvergence);
    void setDepthMetadataEnabled(bool enabled);
    void setWorldAnchoredEnabled(bool enabled);
    void resetWorldAnchor();
    void setOverlayVisible(bool visible) { overlayVisible_ = visible; }
    void setWalkthroughOffset(float x, float y, float z);
    void setWalkthroughRotation(float yaw, float pitch);
    [[nodiscard]] float screenScale() const { return screenScale_; }
    [[nodiscard]] float stereoConvergence() const { return stereoConvergence_; }
    [[nodiscard]] RenderDebugState renderDebugState() const { return renderDebugState_; }

    [[nodiscard]] bool initialized() const { return initialized_; }
    [[nodiscard]] bool sessionRunning() const { return sessionRunning_; }
    [[nodiscard]] bool exitRequested() const { return exitRequested_; }
    [[nodiscard]] const char* lastError() const { return lastError_.c_str(); }

private:
    struct LayerInfo {
        uint8_t worldId = 0xFF;
        float z = 2.0f;
    };

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
    GLuint worldTexture_ = 0;
    GLuint depthRenderbuffer_ = 0;
    GLuint program_ = 0;

    GLint uniformTexture_ = -1;
    GLint uniformWorldTexture_ = -1;
    GLint uniformUvScale_ = -1;
    GLint uniformUvOffset_ = -1;
    GLint uniformMvp_ = -1;
    GLint uniformUseWorldMask_ = -1;
    GLint uniformLayerWorld_ = -1;

    bool initialized_ = false;
    bool sessionRunning_ = false;
    bool frameReady_ = false;
    bool metadataReady_ = false;
    bool exitRequested_ = false;
    bool sideBySideFrame_ = false;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    int metadataWidth_ = 0;
    int metadataHeight_ = 0;
    uint32_t metadataFrameId_ = 0;
    float screenScale_ = 0.68f;
    float stereoConvergence_ = 0.016f;
    bool depthMetadataEnabled_ = false;
    bool worldAnchoredEnabled_ = false;
    bool overlayVisible_ = false;
    bool layerDataReady_ = false;
    int depthBufferWidth_ = 0;
    int depthBufferHeight_ = 0;
    bool headOriginSet_ = false;
    XrVector3f headOrigin_{};
    XrVector3f walkThroughOffset_{};
    float walkThroughYaw_ = 0.0f;
    float walkThroughPitch_ = 0.0f;
    ControllerState controllerState_{};
    std::vector<uint8_t> worldUpload_;
    std::vector<int8_t> disparityUpload_;
    std::array<std::vector<LayerInfo>, 2> eyeLayers_;
    RenderDebugState renderDebugState_{};

    std::string lastError_;
};
