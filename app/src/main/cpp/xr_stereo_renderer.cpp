#include "xr_stereo_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>

#include "log.h"

namespace {

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

constexpr char kVertexShader[] =
    "attribute vec3 aPos;\n"
    "attribute vec2 aUv;\n"
    "uniform mat4 uMvp;\n"
    "varying vec2 vUv;\n"
    "void main() {\n"
    "  vUv = aUv;\n"
    "  gl_Position = uMvp * vec4(aPos, 1.0);\n"
    "}\n";

constexpr char kFragmentShader[] =
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "uniform sampler2D uWorldTex;\n"
    "uniform vec2 uUvScale;\n"
    "uniform vec2 uUvOffset;\n"
    "uniform float uUseWorldMask;\n"
    "uniform float uLayerWorld;\n"
    "varying vec2 vUv;\n"
    "void main() {\n"
    "  vec2 uv = vUv * uUvScale + uUvOffset;\n"
    "  uv = clamp(uv, vec2(0.0), vec2(1.0));\n"
    "  if (uUseWorldMask > 0.5) {\n"
    "    float worldV = floor(texture2D(uWorldTex, uv).r * 255.0 + 0.5);\n"
    "    if (abs(worldV - uLayerWorld) > 0.5) {\n"
    "      discard;\n"
    "    }\n"
    "  }\n"
    "  vec4 c = texture2D(uTex, uv);\n"
    "  float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
    "  gl_FragColor = vec4(l, l * 0.08, l * 0.03, 1.0);\n"
    "}\n";

constexpr float kMinScreenScale = 0.20f;
constexpr float kMaxScreenScale = 1.00f;
constexpr float kMinStereoConvergence = -0.08f;
constexpr float kMaxStereoConvergence = 0.08f;
constexpr int kVipEyeWidth = 384;
constexpr int kVipEyeHeight = 224;
constexpr float kLayerNearZ = 1.2f;
constexpr float kLayerFarZ = 3.8f;
constexpr float kDepthFallbackZ = 2.2f;
constexpr float kClassicAnchoredZ = 2.2f;

struct Mat4 {
    float m[16] = {};
};

Mat4 Mat4Identity() {
    Mat4 out{};
    out.m[0] = 1.0f;
    out.m[5] = 1.0f;
    out.m[10] = 1.0f;
    out.m[15] = 1.0f;
    return out;
}

Mat4 Mat4Multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out.m[(c * 4) + r] = a.m[(0 * 4) + r] * b.m[(c * 4) + 0] +
                                 a.m[(1 * 4) + r] * b.m[(c * 4) + 1] +
                                 a.m[(2 * 4) + r] * b.m[(c * 4) + 2] +
                                 a.m[(3 * 4) + r] * b.m[(c * 4) + 3];
        }
    }
    return out;
}

Mat4 Mat4Translation(const float x, const float y, const float z) {
    Mat4 out = Mat4Identity();
    out.m[12] = x;
    out.m[13] = y;
    out.m[14] = z;
    return out;
}

Mat4 Mat4Scale(const float x, const float y, const float z) {
    Mat4 out{};
    out.m[0] = x;
    out.m[5] = y;
    out.m[10] = z;
    out.m[15] = 1.0f;
    return out;
}

Mat4 Mat4RotationX(const float radians) {
    Mat4 out = Mat4Identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    out.m[5] = c;
    out.m[6] = s;
    out.m[9] = -s;
    out.m[10] = c;
    return out;
}

Mat4 Mat4RotationY(const float radians) {
    Mat4 out = Mat4Identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    out.m[0] = c;
    out.m[2] = -s;
    out.m[8] = s;
    out.m[10] = c;
    return out;
}

Mat4 Mat4PerspectiveFromFov(const XrFovf& fov, const float nearZ, const float farZ) {
    const float tanLeft = std::tan(fov.angleLeft);
    const float tanRight = std::tan(fov.angleRight);
    const float tanDown = std::tan(fov.angleDown);
    const float tanUp = std::tan(fov.angleUp);
    const float tanWidth = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown;

    Mat4 out{};
    out.m[0] = 2.0f / tanWidth;
    out.m[5] = 2.0f / tanHeight;
    out.m[8] = (tanRight + tanLeft) / tanWidth;
    out.m[9] = (tanUp + tanDown) / tanHeight;
    out.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    out.m[11] = -1.0f;
    out.m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
    return out;
}

Mat4 Mat4ViewFromPose(const XrPosef& pose) {
    // OpenXR pose orientation is camera->world. View needs world->camera, so use conjugate.
    const float x = -pose.orientation.x;
    const float y = -pose.orientation.y;
    const float z = -pose.orientation.z;
    const float w = pose.orientation.w;
    const float px = pose.position.x;
    const float py = pose.position.y;
    const float pz = pose.position.z;

    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    // Row-major rotation terms for world->camera view matrix.
    const float r00 = 1.0f - 2.0f * (yy + zz);
    const float r01 = 2.0f * (xy - wz);
    const float r02 = 2.0f * (xz + wy);
    const float r10 = 2.0f * (xy + wz);
    const float r11 = 1.0f - 2.0f * (xx + zz);
    const float r12 = 2.0f * (yz - wx);
    const float r20 = 2.0f * (xz - wy);
    const float r21 = 2.0f * (yz + wx);
    const float r22 = 1.0f - 2.0f * (xx + yy);

    Mat4 out = Mat4Identity();
    out.m[0] = r00;
    out.m[1] = r10;
    out.m[2] = r20;
    out.m[4] = r01;
    out.m[5] = r11;
    out.m[6] = r21;
    out.m[8] = r02;
    out.m[9] = r12;
    out.m[10] = r22;
    out.m[12] = -(r00 * px + r01 * py + r02 * pz);
    out.m[13] = -(r10 * px + r11 * py + r12 * pz);
    out.m[14] = -(r20 * px + r21 * py + r22 * pz);
    return out;
}

GLuint CompileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char infoLog[512] = {};
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        LOGE("XR shader compile error: %s", infoLog);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint CreateProgram(const char* vertex, const char* fragment) {
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment);
    if (vs == 0 || fs == 0) {
        if (vs != 0) {
            glDeleteShader(vs);
        }
        if (fs != 0) {
            glDeleteShader(fs);
        }
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "aPos");
    glBindAttribLocation(program, 1, "aUv");
    glLinkProgram(program);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char infoLog[512] = {};
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        LOGE("XR program link error: %s", infoLog);
        glDeleteProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

XrPosef IdentityPose() {
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    return pose;
}

}  // namespace

bool XrStereoRenderer::setError(const char* context, const XrResult result) {
    char buffer[256] = {};
    std::snprintf(buffer, sizeof(buffer), "%s failed (XrResult=%d)", context, result);
    lastError_ = buffer;
    LOGE("%s", lastError_.c_str());
    return false;
}

bool XrStereoRenderer::setErrorMessage(const char* message) {
    lastError_ = message;
    LOGE("%s", lastError_.c_str());
    return false;
}

void XrStereoRenderer::setPresentationConfig(const float screenScale, const float stereoConvergence) {
    screenScale_ = std::clamp(screenScale, kMinScreenScale, kMaxScreenScale);
    stereoConvergence_ =
        std::clamp(stereoConvergence, kMinStereoConvergence, kMaxStereoConvergence);
}

void XrStereoRenderer::setDepthMetadataEnabled(const bool enabled) {
    if (enabled && !depthMetadataEnabled_) {
        // Re-capture world anchor at next frame when entering depth mode.
        headOriginSet_ = false;
    }
    depthMetadataEnabled_ = enabled;
}

void XrStereoRenderer::setWorldAnchoredEnabled(const bool enabled) {
    if (enabled && !worldAnchoredEnabled_) {
        // Re-capture world anchor at next frame when entering anchored mode.
        headOriginSet_ = false;
    }
    worldAnchoredEnabled_ = enabled;
}

void XrStereoRenderer::resetWorldAnchor() {
    headOriginSet_ = false;
}

void XrStereoRenderer::setWalkthroughOffset(const float x, const float y, const float z) {
    walkThroughOffset_.x = std::clamp(x, -30.0f, 30.0f);
    walkThroughOffset_.y = std::clamp(y, -30.0f, 30.0f);
    walkThroughOffset_.z = std::clamp(z, -30.0f, 30.0f);
}

void XrStereoRenderer::setWalkthroughRotation(const float yaw, const float pitch) {
    walkThroughYaw_ = yaw;
    walkThroughPitch_ = std::clamp(pitch, -1.2f, 1.2f);
}

bool XrStereoRenderer::makeCurrent() {
    if (eglDisplay_ == EGL_NO_DISPLAY || eglContext_ == EGL_NO_CONTEXT ||
        eglSurface_ == EGL_NO_SURFACE) {
        return false;
    }
    return eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_) == EGL_TRUE;
}

bool XrStereoRenderer::initializeLoader() {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    xrGetInstanceProcAddr(
        XR_NULL_HANDLE,
        "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));

    if (initializeLoader == nullptr) {
        return true;
    }

    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = activity_->vm;
    loaderInfo.applicationContext = activity_->clazz;

    const XrResult result =
        initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
    if (XR_FAILED(result)) {
        return setError("xrInitializeLoaderKHR", result);
    }
    return true;
}

bool XrStereoRenderer::createInstance() {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> extensions(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, extensions.data());

    auto hasExtension = [&extensions](const char* extensionName) {
        for (const auto& ext : extensions) {
            if (std::strcmp(ext.extensionName, extensionName) == 0) {
                return true;
            }
        }
        return false;
    };

    if (!hasExtension(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) ||
        !hasExtension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        return setErrorMessage("Required OpenXR extensions not available");
    }

    std::vector<const char*> enabledExtensions = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = activity_->vm;
    androidInfo.applicationActivity = activity_->clazz;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::snprintf(
        createInfo.applicationInfo.applicationName,
        XR_MAX_APPLICATION_NAME_SIZE,
        "virtualvirtualboy");
    std::snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "custom");
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();

    const XrResult result = xrCreateInstance(&createInfo, &instance_);
    if (XR_FAILED(result)) {
        return setError("xrCreateInstance", result);
    }
    return true;
}

bool XrStereoRenderer::createSystem() {
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult result = xrGetSystem(instance_, &systemInfo, &systemId_);
    if (XR_FAILED(result)) {
        return setError("xrGetSystem", result);
    }
    return true;
}

bool XrStereoRenderer::createEglContext() {
    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        return setErrorMessage("eglGetDisplay failed");
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(eglDisplay_, &major, &minor) != EGL_TRUE) {
        return setErrorMessage("eglInitialize failed");
    }

    constexpr std::array<EGLint, 15> configAttrs = {
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_DEPTH_SIZE,
        0,
        EGL_NONE,
    };

    EGLint configCount = 0;
    if (eglChooseConfig(eglDisplay_, configAttrs.data(), &eglConfig_, 1, &configCount) !=
            EGL_TRUE ||
        configCount < 1) {
        return setErrorMessage("eglChooseConfig failed");
    }

    constexpr std::array<EGLint, 5> pbufferAttrs = {
        EGL_WIDTH,
        16,
        EGL_HEIGHT,
        16,
        EGL_NONE,
    };
    eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufferAttrs.data());
    if (eglSurface_ == EGL_NO_SURFACE) {
        return setErrorMessage("eglCreatePbufferSurface failed");
    }

    constexpr std::array<EGLint, 3> ctxAttrs = {
        EGL_CONTEXT_CLIENT_VERSION,
        3,
        EGL_NONE,
    };
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, ctxAttrs.data());
    if (eglContext_ == EGL_NO_CONTEXT) {
        return setErrorMessage("eglCreateContext failed");
    }

    if (!makeCurrent()) {
        return setErrorMessage("eglMakeCurrent failed");
    }
    return true;
}

bool XrStereoRenderer::createSession() {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getGraphicsRequirements = nullptr;
    const XrResult procResult = xrGetInstanceProcAddr(
        instance_,
        "xrGetOpenGLESGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&getGraphicsRequirements));
    if (XR_FAILED(procResult) || getGraphicsRequirements == nullptr) {
        return setError("xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)", procResult);
    }

    XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    const XrResult reqResult =
        getGraphicsRequirements(instance_, systemId_, &graphicsRequirements);
    if (XR_FAILED(reqResult)) {
        return setError("xrGetOpenGLESGraphicsRequirementsKHR", reqResult);
    }

    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = eglDisplay_;
    graphicsBinding.config = eglConfig_;
    graphicsBinding.context = eglContext_;

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = systemId_;

    const XrResult createResult = xrCreateSession(instance_, &sessionInfo, &session_);
    if (XR_FAILED(createResult)) {
        return setError("xrCreateSession", createResult);
    }
    return true;
}

bool XrStereoRenderer::createInputActions() {
    if (instance_ == XR_NULL_HANDLE || session_ == XR_NULL_HANDLE) {
        return setErrorMessage("OpenXR input setup requires instance and session");
    }

    if (XR_FAILED(xrStringToPath(instance_, "/user/hand/left", &leftHandPath_)) ||
        XR_FAILED(xrStringToPath(instance_, "/user/hand/right", &rightHandPath_)) ||
        XR_FAILED(
            xrStringToPath(
                instance_, "/interaction_profiles/oculus/touch_controller", &oculusTouchProfilePath_)) ||
        XR_FAILED(
            xrStringToPath(
                instance_, "/interaction_profiles/khr/simple_controller", &khrSimpleProfilePath_))) {
        return setErrorMessage("xrStringToPath failed while initializing controller paths");
    }

    XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(actionSetInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "gameplay");
    std::snprintf(actionSetInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Gameplay");
    actionSetInfo.priority = 0;
    XrResult result = xrCreateActionSet(instance_, &actionSetInfo, &actionSet_);
    if (XR_FAILED(result)) {
        return setError("xrCreateActionSet", result);
    }

    const XrPath bothHands[] = {leftHandPath_, rightHandPath_};
    const XrPath leftHand[] = {leftHandPath_};
    const XrPath rightHand[] = {rightHandPath_};

    auto createAction = [this](
                            XrActionType type,
                            const char* actionName,
                            const char* localizedName,
                            const XrPath* subactionPaths,
                            uint32_t subactionPathCount,
                            XrAction& outAction) -> bool {
        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        actionInfo.actionType = type;
        std::snprintf(actionInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", actionName);
        std::snprintf(
            actionInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localizedName);
        actionInfo.countSubactionPaths = subactionPathCount;
        actionInfo.subactionPaths = subactionPaths;
        const XrResult create = xrCreateAction(actionSet_, &actionInfo, &outAction);
        if (XR_FAILED(create)) {
            return setError("xrCreateAction", create);
        }
        return true;
    };

    if (!createAction(
            XR_ACTION_TYPE_VECTOR2F_INPUT,
            "move",
            "Move",
            bothHands,
            2,
            moveAction_) ||
        !createAction(
            XR_ACTION_TYPE_FLOAT_INPUT,
            "left_squeeze",
            "Left Squeeze",
            leftHand,
            1,
            leftSqueezeAction_) ||
        !createAction(
            XR_ACTION_TYPE_FLOAT_INPUT,
            "right_squeeze",
            "Right Squeeze",
            rightHand,
            1,
            rightSqueezeAction_) ||
        !createAction(
            XR_ACTION_TYPE_FLOAT_INPUT,
            "left_trigger",
            "Left Trigger",
            leftHand,
            1,
            leftTriggerAction_) ||
        !createAction(
            XR_ACTION_TYPE_FLOAT_INPUT,
            "right_trigger",
            "Right Trigger",
            rightHand,
            1,
            rightTriggerAction_) ||
        !createAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "left_thumb_click",
            "Left Thumb Click",
            leftHand,
            1,
            leftThumbClickAction_) ||
        !createAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "right_thumb_click",
            "Right Thumb Click",
            rightHand,
            1,
            rightThumbClickAction_) ||
        !createAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_a",
            "Button A",
            rightHand,
            1,
            buttonAAction_) ||
        !createAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_b",
            "Button B",
            rightHand,
            1,
            buttonBAction_) ||
        !createAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_x",
            "Button X",
            leftHand,
            1,
            buttonXAction_) ||
        !createAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_y",
            "Button Y",
            leftHand,
            1,
            buttonYAction_) ||
        !createAction(
            XR_ACTION_TYPE_BOOLEAN_INPUT,
            "button_menu",
            "Button Menu",
            leftHand,
            1,
            menuAction_)) {
        return false;
    }

    if (!suggestInteractionBindings()) {
        return false;
    }

    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet_;
    result = xrAttachSessionActionSets(session_, &attachInfo);
    if (XR_FAILED(result)) {
        return setError("xrAttachSessionActionSets", result);
    }

    return true;
}

bool XrStereoRenderer::suggestInteractionBindings() {
    struct BindingCandidate {
        XrAction action;
        const char* path;
    };

    const BindingCandidate candidates[] = {
        {moveAction_, "/user/hand/left/input/thumbstick"},
        {moveAction_, "/user/hand/right/input/thumbstick"},
        {leftSqueezeAction_, "/user/hand/left/input/squeeze/value"},
        {rightSqueezeAction_, "/user/hand/right/input/squeeze/value"},
        {leftTriggerAction_, "/user/hand/left/input/trigger/value"},
        {rightTriggerAction_, "/user/hand/right/input/trigger/value"},
        {leftThumbClickAction_, "/user/hand/left/input/thumbstick/click"},
        {rightThumbClickAction_, "/user/hand/right/input/thumbstick/click"},
        {buttonAAction_, "/user/hand/right/input/a/click"},
        {buttonBAction_, "/user/hand/right/input/b/click"},
        {buttonXAction_, "/user/hand/left/input/x/click"},
        {buttonYAction_, "/user/hand/left/input/y/click"},
        {menuAction_, "/user/hand/left/input/menu/click"},
    };

    std::vector<XrActionSuggestedBinding> acceptedBindings;
    acceptedBindings.reserve(sizeof(candidates) / sizeof(candidates[0]));

    for (const auto& candidate : candidates) {
        XrPath path = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(instance_, candidate.path, &path))) {
            LOGW("OpenXR path rejected by runtime: %s", candidate.path);
            continue;
        }

        const XrActionSuggestedBinding oneBinding[] = {{candidate.action, path}};
        XrInteractionProfileSuggestedBinding oneSuggest{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        oneSuggest.interactionProfile = oculusTouchProfilePath_;
        oneSuggest.countSuggestedBindings = 1;
        oneSuggest.suggestedBindings = oneBinding;

        const XrResult testResult = xrSuggestInteractionProfileBindings(instance_, &oneSuggest);
        if (XR_FAILED(testResult)) {
            LOGW(
                "OpenXR binding rejected: %s (XrResult=%d)",
                candidate.path,
                static_cast<int>(testResult));
            continue;
        }

        acceptedBindings.push_back({candidate.action, path});
    }

    if (acceptedBindings.empty()) {
        return setErrorMessage("No usable OpenXR controller bindings accepted");
    }

    XrInteractionProfileSuggestedBinding suggested{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = oculusTouchProfilePath_;
    suggested.countSuggestedBindings = static_cast<uint32_t>(acceptedBindings.size());
    suggested.suggestedBindings = acceptedBindings.data();

    const XrResult finalResult = xrSuggestInteractionProfileBindings(instance_, &suggested);
    if (XR_FAILED(finalResult)) {
        return setError("xrSuggestInteractionProfileBindings(final)", finalResult);
    }

    return true;
}

bool XrStereoRenderer::createReferenceSpace() {
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace = IdentityPose();
    const XrResult result = xrCreateReferenceSpace(session_, &spaceInfo, &appSpace_);
    if (XR_FAILED(result)) {
        return setError("xrCreateReferenceSpace", result);
    }
    return true;
}

bool XrStereoRenderer::createSwapchains() {
    uint32_t viewCount = 0;
    XrResult result = xrEnumerateViewConfigurationViews(
        instance_,
        systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &viewCount,
        nullptr);
    if (XR_FAILED(result) || viewCount < 2) {
        return setError("xrEnumerateViewConfigurationViews(count)", result);
    }

    configViews_.assign(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(
        instance_,
        systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        viewCount,
        &viewCount,
        configViews_.data());
    if (XR_FAILED(result)) {
        return setError("xrEnumerateViewConfigurationViews(data)", result);
    }

    views_.assign(viewCount, {XR_TYPE_VIEW});
    eyeSwapchains_.resize(viewCount);

    uint32_t formatCount = 0;
    result = xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr);
    if (XR_FAILED(result) || formatCount == 0) {
        return setError("xrEnumerateSwapchainFormats(count)", result);
    }

    std::vector<int64_t> formats(formatCount);
    result = xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data());
    if (XR_FAILED(result)) {
        return setError("xrEnumerateSwapchainFormats(data)", result);
    }

    int64_t selectedFormat = formats.front();
    for (const int64_t format : formats) {
        if (format == GL_SRGB8_ALPHA8 || format == GL_RGBA8) {
            selectedFormat = format;
            break;
        }
    }

    for (uint32_t i = 0; i < viewCount; ++i) {
        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.format = selectedFormat;
        createInfo.sampleCount = configViews_[i].recommendedSwapchainSampleCount;
        createInfo.width = configViews_[i].recommendedImageRectWidth;
        createInfo.height = configViews_[i].recommendedImageRectHeight;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        result = xrCreateSwapchain(session_, &createInfo, &eyeSwapchains_[i].handle);
        if (XR_FAILED(result)) {
            return setError("xrCreateSwapchain", result);
        }

        eyeSwapchains_[i].width = static_cast<int32_t>(createInfo.width);
        eyeSwapchains_[i].height = static_cast<int32_t>(createInfo.height);

        uint32_t imageCount = 0;
        result = xrEnumerateSwapchainImages(
            eyeSwapchains_[i].handle, 0, &imageCount, nullptr);
        if (XR_FAILED(result) || imageCount == 0) {
            return setError("xrEnumerateSwapchainImages(count)", result);
        }

        eyeSwapchains_[i].images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        result = xrEnumerateSwapchainImages(
            eyeSwapchains_[i].handle,
            imageCount,
            &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(eyeSwapchains_[i].images.data()));
        if (XR_FAILED(result)) {
            return setError("xrEnumerateSwapchainImages(data)", result);
        }
    }

    return true;
}

bool XrStereoRenderer::createGlResources() {
    if (!makeCurrent()) {
        return setErrorMessage("XR GL context not current");
    }

    program_ = CreateProgram(kVertexShader, kFragmentShader);
    if (program_ == 0) {
        return setErrorMessage("Failed creating XR GL program");
    }

    uniformTexture_ = glGetUniformLocation(program_, "uTex");
    uniformWorldTexture_ = glGetUniformLocation(program_, "uWorldTex");
    uniformUvScale_ = glGetUniformLocation(program_, "uUvScale");
    uniformUvOffset_ = glGetUniformLocation(program_, "uUvOffset");
    uniformMvp_ = glGetUniformLocation(program_, "uMvp");
    uniformUseWorldMask_ = glGetUniformLocation(program_, "uUseWorldMask");
    uniformLayerWorld_ = glGetUniformLocation(program_, "uLayerWorld");

    glGenTextures(1, &emuTexture_);
    glBindTexture(GL_TEXTURE_2D, emuTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &worldTexture_);
    glBindTexture(GL_TEXTURE_2D, worldTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &depthRenderbuffer_);
    glGenFramebuffers(1, &framebuffer_);
    return true;
}

bool XrStereoRenderer::getBooleanActionState(XrAction action, XrPath subactionPath) const {
    if (session_ == XR_NULL_HANDLE || action == XR_NULL_HANDLE) {
        return false;
    }
    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = action;
    getInfo.subactionPath = subactionPath;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    const XrResult result = xrGetActionStateBoolean(session_, &getInfo, &state);
    if (XR_FAILED(result)) {
        return false;
    }
    return state.isActive && state.currentState;
}

float XrStereoRenderer::getFloatActionState(XrAction action, XrPath subactionPath) const {
    if (session_ == XR_NULL_HANDLE || action == XR_NULL_HANDLE) {
        return 0.0f;
    }
    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = action;
    getInfo.subactionPath = subactionPath;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    const XrResult result = xrGetActionStateFloat(session_, &getInfo, &state);
    if (XR_FAILED(result) || !state.isActive) {
        return 0.0f;
    }
    return state.currentState;
}

bool XrStereoRenderer::getVector2ActionState(
    XrAction action, XrVector2f& outValue, XrPath subactionPath) const {
    outValue = {0.0f, 0.0f};
    if (session_ == XR_NULL_HANDLE || action == XR_NULL_HANDLE) {
        return false;
    }
    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = action;
    getInfo.subactionPath = subactionPath;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    const XrResult result = xrGetActionStateVector2f(session_, &getInfo, &state);
    if (XR_FAILED(result) || !state.isActive) {
        return false;
    }
    outValue = state.currentState;
    return true;
}

void XrStereoRenderer::syncInput() {
    if (!sessionRunning_ || session_ == XR_NULL_HANDLE || actionSet_ == XR_NULL_HANDLE) {
        controllerState_ = ControllerState{};
        return;
    }

    XrActiveActionSet activeSet{};
    activeSet.actionSet = actionSet_;
    activeSet.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    const XrResult syncResult = xrSyncActions(session_, &syncInfo);
    if (XR_FAILED(syncResult)) {
        controllerState_ = ControllerState{};
        return;
    }

    constexpr float kDeadzone = 0.35f;
    constexpr float kTriggerPressThreshold = 0.45f;

    XrVector2f leftStick{};
    XrVector2f rightStick{};
    const bool leftActive = getVector2ActionState(moveAction_, leftStick, leftHandPath_);
    const bool rightActive = getVector2ActionState(moveAction_, rightStick, rightHandPath_);
    controllerState_.leftStickX = leftActive ? leftStick.x : 0.0f;
    controllerState_.leftStickY = leftActive ? leftStick.y : 0.0f;
    controllerState_.rightStickX = rightActive ? rightStick.x : 0.0f;
    controllerState_.rightStickY = rightActive ? rightStick.y : 0.0f;
    XrVector2f move = leftActive ? leftStick : rightStick;
    if (leftActive && rightActive) {
        if ((rightStick.x * rightStick.x + rightStick.y * rightStick.y) >
            (leftStick.x * leftStick.x + leftStick.y * leftStick.y)) {
            move = rightStick;
        }
    }

    controllerState_.left = move.x < -kDeadzone;
    controllerState_.right = move.x > kDeadzone;
    controllerState_.up = move.y > kDeadzone;
    controllerState_.down = move.y < -kDeadzone;

    controllerState_.a = getBooleanActionState(buttonAAction_);
    controllerState_.b = getBooleanActionState(buttonBAction_);
    controllerState_.x = getBooleanActionState(buttonXAction_);
    controllerState_.y = getBooleanActionState(buttonYAction_);

    const float leftTrigger = getFloatActionState(leftTriggerAction_, leftHandPath_);
    const float leftSqueeze = getFloatActionState(leftSqueezeAction_, leftHandPath_);
    const float rightTrigger = getFloatActionState(rightTriggerAction_, rightHandPath_);
    const float rightSqueeze = getFloatActionState(rightSqueezeAction_, rightHandPath_);
    controllerState_.leftGrip = leftSqueeze > kTriggerPressThreshold;
    controllerState_.rightGrip = rightSqueeze > kTriggerPressThreshold;
    controllerState_.l = leftTrigger > kTriggerPressThreshold;
    controllerState_.r = rightTrigger > kTriggerPressThreshold;

    const bool leftThumbClick = getBooleanActionState(leftThumbClickAction_, leftHandPath_);
    const bool rightThumbClick = getBooleanActionState(rightThumbClickAction_, rightHandPath_);
    const bool menuClick = getBooleanActionState(menuAction_, leftHandPath_);

    controllerState_.leftThumbClick = leftThumbClick;
    controllerState_.rightThumbClick = rightThumbClick;
    controllerState_.start = controllerState_.y || menuClick;
    controllerState_.select = controllerState_.x;
}

void XrStereoRenderer::destroyInputActions() {
    if (buttonAAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(buttonAAction_);
        buttonAAction_ = XR_NULL_HANDLE;
    }
    if (buttonBAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(buttonBAction_);
        buttonBAction_ = XR_NULL_HANDLE;
    }
    if (buttonXAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(buttonXAction_);
        buttonXAction_ = XR_NULL_HANDLE;
    }
    if (buttonYAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(buttonYAction_);
        buttonYAction_ = XR_NULL_HANDLE;
    }
    if (menuAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(menuAction_);
        menuAction_ = XR_NULL_HANDLE;
    }
    if (leftThumbClickAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(leftThumbClickAction_);
        leftThumbClickAction_ = XR_NULL_HANDLE;
    }
    if (rightThumbClickAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(rightThumbClickAction_);
        rightThumbClickAction_ = XR_NULL_HANDLE;
    }
    if (leftTriggerAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(leftTriggerAction_);
        leftTriggerAction_ = XR_NULL_HANDLE;
    }
    if (rightTriggerAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(rightTriggerAction_);
        rightTriggerAction_ = XR_NULL_HANDLE;
    }
    if (leftSqueezeAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(leftSqueezeAction_);
        leftSqueezeAction_ = XR_NULL_HANDLE;
    }
    if (rightSqueezeAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(rightSqueezeAction_);
        rightSqueezeAction_ = XR_NULL_HANDLE;
    }
    if (moveAction_ != XR_NULL_HANDLE) {
        xrDestroyAction(moveAction_);
        moveAction_ = XR_NULL_HANDLE;
    }
    if (actionSet_ != XR_NULL_HANDLE) {
        xrDestroyActionSet(actionSet_);
        actionSet_ = XR_NULL_HANDLE;
    }
    leftHandPath_ = XR_NULL_PATH;
    rightHandPath_ = XR_NULL_PATH;
    oculusTouchProfilePath_ = XR_NULL_PATH;
    khrSimpleProfilePath_ = XR_NULL_PATH;
    controllerState_ = ControllerState{};
}

bool XrStereoRenderer::beginSession() {
    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    const XrResult result = xrBeginSession(session_, &beginInfo);
    if (XR_FAILED(result)) {
        return setError("xrBeginSession", result);
    }
    headOriginSet_ = false;
    headOrigin_ = {};
    walkThroughOffset_ = {};
    walkThroughYaw_ = 0.0f;
    walkThroughPitch_ = 0.0f;
    layerDataReady_ = false;
    eyeLayers_[0].clear();
    eyeLayers_[1].clear();
    renderDebugState_ = {};
    renderDebugState_.xrActive = true;
    sessionRunning_ = true;
    return true;
}

void XrStereoRenderer::endSession() {
    if (!sessionRunning_) {
        return;
    }
    xrEndSession(session_);
    sessionRunning_ = false;
    renderDebugState_.xrActive = false;
}

void XrStereoRenderer::destroySwapchains() {
    for (auto& eye : eyeSwapchains_) {
        if (eye.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye.handle);
            eye.handle = XR_NULL_HANDLE;
        }
        eye.images.clear();
    }
    eyeSwapchains_.clear();
}

bool XrStereoRenderer::initialize(ANativeActivity* activity) {
    shutdown();
    activity_ = activity;
    if (activity_ == nullptr) {
        return setErrorMessage("XrStereoRenderer requires ANativeActivity");
    }

    if (!initializeLoader() || !createInstance() || !createSystem() || !createEglContext() ||
        !createSession() || !createInputActions() || !createReferenceSpace() || !createSwapchains() ||
        !createGlResources()) {
        shutdown();
        return false;
    }

    initialized_ = true;
    lastError_.clear();
    LOGI("OpenXR stereo renderer initialized");
    return true;
}

void XrStereoRenderer::pollEvents() {
    if (!initialized_) {
        return;
    }

    XrEventDataBuffer eventBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &eventBuffer) == XR_SUCCESS) {
        if (eventBuffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* stateChanged =
                reinterpret_cast<const XrEventDataSessionStateChanged*>(&eventBuffer);
            sessionState_ = stateChanged->state;

            switch (sessionState_) {
                case XR_SESSION_STATE_READY:
                    beginSession();
                    break;
                case XR_SESSION_STATE_STOPPING:
                    endSession();
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    exitRequested_ = true;
                    break;
                default:
                    break;
            }
        }
        eventBuffer = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }

    syncInput();
}

void XrStereoRenderer::updateFrame(const uint32_t* pixels, int width, int height) {
    if (!initialized_ || pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (!makeCurrent()) {
        return;
    }

    frameWidth_ = width;
    frameHeight_ = height;
    frameReady_ = true;
    sideBySideFrame_ = width >= (height * 2);

    glBindTexture(GL_TEXTURE_2D, emuTexture_);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

void XrStereoRenderer::updateDepthMetadata(
    const int8_t* disparity,
    const uint8_t* worldIds,
    const int16_t* sourceX,
    const int16_t* sourceY,
    const int width,
    const int height,
    const uint32_t frameId) {
    (void)sourceX;
    (void)sourceY;
    if (!initialized_ || disparity == nullptr || worldIds == nullptr || width <= 0 || height <= 0 ||
        !makeCurrent()) {
        metadataReady_ = false;
        layerDataReady_ = false;
        metadataWidth_ = 0;
        metadataHeight_ = 0;
        disparityUpload_.clear();
        eyeLayers_[0].clear();
        eyeLayers_[1].clear();
        return;
    }

    metadataWidth_ = width;
    metadataHeight_ = height;
    metadataFrameId_ = frameId;
    metadataReady_ = true;
    layerDataReady_ = width >= (kVipEyeWidth * 2) && height >= kVipEyeHeight;

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    disparityUpload_.assign(disparity, disparity + pixelCount);
    worldUpload_.assign(worldIds, worldIds + pixelCount);
    glBindTexture(GL_TEXTURE_2D, worldTexture_);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_LUMINANCE,
        width,
        height,
        0,
        GL_LUMINANCE,
        GL_UNSIGNED_BYTE,
        worldUpload_.data());

    eyeLayers_[0].clear();
    eyeLayers_[1].clear();
    if (!layerDataReady_) {
        return;
    }

    for (int eye = 0; eye < 2; ++eye) {
        std::array<int64_t, 32> disparitySum{};
        std::array<int32_t, 32> disparityCount{};
        for (int y = 0; y < kVipEyeHeight; ++y) {
            const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(width);
            const size_t eyeOffset = static_cast<size_t>(eye * kVipEyeWidth);
            for (int x = 0; x < kVipEyeWidth; ++x) {
                const size_t index = rowOffset + eyeOffset + static_cast<size_t>(x);
                const uint8_t worldId = worldIds[index];
                if (worldId >= 32) {
                    continue;
                }
                const int depthAbs = std::abs(static_cast<int>(disparity[index]));
                disparitySum[worldId] += depthAbs;
                disparityCount[worldId]++;
            }
        }

        auto& layers = eyeLayers_[eye];
        for (uint8_t worldId = 0; worldId < 32; ++worldId) {
            if (disparityCount[worldId] <= 0) {
                continue;
            }
            const float avgDisp =
                static_cast<float>(disparitySum[worldId]) / static_cast<float>(disparityCount[worldId]);
            const float closeness = std::clamp(avgDisp / 127.0f, 0.0f, 1.0f);
            const float z = kLayerFarZ - closeness * (kLayerFarZ - kLayerNearZ);
            layers.push_back({worldId, z});
        }

        std::sort(layers.begin(), layers.end(), [](const LayerInfo& a, const LayerInfo& b) {
            return a.z > b.z;  // far-to-near painter order
        });
    }
}

bool XrStereoRenderer::renderFrame() {
    if (!initialized_) {
        return false;
    }

    pollEvents();
    if (!sessionRunning_) {
        renderDebugState_.xrActive = false;
        return false;
    }
    renderDebugState_.xrActive = true;
    renderDebugState_.depthModeEnabled = depthMetadataEnabled_;
    renderDebugState_.overlayVisible = overlayVisible_;
    renderDebugState_.headOriginSet = headOriginSet_;
    renderDebugState_.usedLayerRendering = false;
    renderDebugState_.usedDepthFallback = false;
    renderDebugState_.usedClassic = false;
    renderDebugState_.frameShouldRender = false;
    renderDebugState_.metadataAligned = false;
    renderDebugState_.layerDataReady = layerDataReady_;
    renderDebugState_.relativeX = 0.0f;
    renderDebugState_.relativeY = 0.0f;
    renderDebugState_.relativeZ = 0.0f;

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrResult result = xrWaitFrame(session_, &waitInfo, &frameState);
    if (XR_FAILED(result)) {
        setError("xrWaitFrame", result);
        return false;
    }

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    result = xrBeginFrame(session_, &beginInfo);
    if (XR_FAILED(result)) {
        setError("xrBeginFrame", result);
        return false;
    }

    std::vector<XrCompositionLayerProjectionView> projectionViews;
    XrCompositionLayerProjection projectionLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    renderDebugState_.frameShouldRender = frameState.shouldRender;

    if (frameState.shouldRender) {
        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = appSpace_;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 0;
        result =
            xrLocateViews(session_, &locateInfo, &viewState, static_cast<uint32_t>(views_.size()),
                          &viewCount, views_.data());
        if (XR_FAILED(result)) {
            setError("xrLocateViews", result);
            viewCount = 0;
        }

        if (viewCount > 0) {
            projectionViews.resize(viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

            const float screenScale = std::clamp(screenScale_, kMinScreenScale, kMaxScreenScale);
            const float stereoConvergence =
                std::clamp(stereoConvergence_, kMinStereoConvergence, kMaxStereoConvergence);
            const GLfloat quadVertices[] = {
                -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
                1.0f,  -1.0f, 0.0f, 1.0f, 1.0f,
                -1.0f, 1.0f,  0.0f, 0.0f, 0.0f,
                1.0f,  1.0f,  0.0f, 1.0f, 0.0f,
            };

            if (!headOriginSet_) {
                XrVector3f headCenter = views_[0].pose.position;
                if (viewCount > 1) {
                    headCenter.x = (views_[0].pose.position.x + views_[1].pose.position.x) * 0.5f;
                    headCenter.y = (views_[0].pose.position.y + views_[1].pose.position.y) * 0.5f;
                    headCenter.z = (views_[0].pose.position.z + views_[1].pose.position.z) * 0.5f;
                }
                headOrigin_ = headCenter;
                headOriginSet_ = true;
                renderDebugState_.headOriginSet = true;
            }

            const XrVector3f worldAnchor = headOrigin_;
            renderDebugState_.relativeX = walkThroughOffset_.x;
            renderDebugState_.relativeY = walkThroughOffset_.y;
            renderDebugState_.relativeZ = walkThroughOffset_.z;

            for (uint32_t i = 0; i < viewCount && i < eyeSwapchains_.size(); ++i) {
                auto& eye = eyeSwapchains_[i];

                XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                uint32_t imageIndex = 0;
                result = xrAcquireSwapchainImage(eye.handle, &acquireInfo, &imageIndex);
                if (XR_FAILED(result)) {
                    setError("xrAcquireSwapchainImage", result);
                    continue;
                }

                XrSwapchainImageWaitInfo waitImageInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitImageInfo.timeout = XR_INFINITE_DURATION;
                result = xrWaitSwapchainImage(eye.handle, &waitImageInfo);
                if (XR_FAILED(result)) {
                    setError("xrWaitSwapchainImage", result);
                    continue;
                }

                if (makeCurrent()) {
                    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
                    glFramebufferTexture2D(
                        GL_FRAMEBUFFER,
                        GL_COLOR_ATTACHMENT0,
                        GL_TEXTURE_2D,
                        eye.images[imageIndex].image,
                        0);

                    if (depthRenderbuffer_ != 0 &&
                        (depthBufferWidth_ != eye.width || depthBufferHeight_ != eye.height)) {
                        glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer_);
                        glRenderbufferStorage(
                            GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, eye.width, eye.height);
                        depthBufferWidth_ = eye.width;
                        depthBufferHeight_ = eye.height;
                    }
                    if (depthRenderbuffer_ != 0) {
                        glFramebufferRenderbuffer(
                            GL_FRAMEBUFFER,
                            GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER,
                            depthRenderbuffer_);
                    }

                    glViewport(0, 0, eye.width, eye.height);
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                    if (frameReady_) {
                        glUseProgram(program_);
                        const bool metadataAligned =
                            metadataReady_ && metadataWidth_ == frameWidth_ &&
                            metadataHeight_ == frameHeight_;
                        renderDebugState_.metadataAligned = metadataAligned;
                        const bool useLayerRendering =
                            depthMetadataEnabled_ && metadataAligned && layerDataReady_ &&
                            sideBySideFrame_ && !overlayVisible_ &&
                            i < eyeLayers_.size() && !eyeLayers_[i].empty();

                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, emuTexture_);
                        glUniform1i(uniformTexture_, 0);
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, worldTexture_);
                        glUniform1i(uniformWorldTexture_, 1);
                        glActiveTexture(GL_TEXTURE0);

                        glVertexAttribPointer(
                            0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), quadVertices);
                        glEnableVertexAttribArray(0);
                        glVertexAttribPointer(
                            1,
                            2,
                            GL_FLOAT,
                            GL_FALSE,
                            5 * sizeof(GLfloat),
                            reinterpret_cast<const void*>(quadVertices + 3));
                        glEnableVertexAttribArray(1);

                        const Mat4 projection =
                            Mat4PerspectiveFromFov(views_[i].fov, 0.05f, 100.0f);
                        const Mat4 view = Mat4ViewFromPose(views_[i].pose);
                        const Mat4 walkRotation = Mat4Multiply(
                            Mat4RotationY(-walkThroughYaw_), Mat4RotationX(-walkThroughPitch_));
                        const Mat4 navigation = Mat4Multiply(
                            Mat4Translation(worldAnchor.x, worldAnchor.y, worldAnchor.z),
                            Mat4Multiply(
                                walkRotation,
                                Mat4Translation(
                                    -walkThroughOffset_.x,
                                    -walkThroughOffset_.y,
                                    -walkThroughOffset_.z)));

                        if (useLayerRendering) {
                            if (i == 0) {
                                renderDebugState_.usedLayerRendering = true;
                            }
                            glDisable(GL_DEPTH_TEST);
                            glUniform2f(uniformUvScale_, 0.5f, 1.0f);
                            glUniform2f(uniformUvOffset_, i == 0 ? 0.0f : 0.5f, 0.0f);
                            glUniform1f(uniformUseWorldMask_, 1.0f);

                            for (const auto& layer : eyeLayers_[i]) {
                                const float halfSize = screenScale * layer.z;
                                const Mat4 model = Mat4Multiply(
                                    navigation,
                                    Mat4Multiply(
                                        Mat4Translation(0.0f, 0.0f, -layer.z),
                                        Mat4Scale(halfSize, halfSize, 1.0f)));
                                const Mat4 viewModel = Mat4Multiply(view, model);
                                const Mat4 mvp = Mat4Multiply(projection, viewModel);
                                glUniformMatrix4fv(uniformMvp_, 1, GL_FALSE, mvp.m);
                                glUniform1f(uniformLayerWorld_, static_cast<float>(layer.worldId));
                                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                            }
                        } else if (depthMetadataEnabled_) {
                            if (i == 0) {
                                renderDebugState_.usedDepthFallback = true;
                            }
                            glDisable(GL_DEPTH_TEST);
                            const float halfSize = screenScale * kDepthFallbackZ;
                            const Mat4 model = Mat4Multiply(
                                navigation,
                                Mat4Multiply(
                                    Mat4Translation(0.0f, 0.0f, -kDepthFallbackZ),
                                    Mat4Scale(halfSize, halfSize, 1.0f)));
                            const Mat4 mvp = Mat4Multiply(projection, Mat4Multiply(view, model));
                            glUniformMatrix4fv(uniformMvp_, 1, GL_FALSE, mvp.m);
                            glUniform1f(uniformUseWorldMask_, 0.0f);
                            glUniform1f(uniformLayerWorld_, -1.0f);
                            if (sideBySideFrame_) {
                                glUniform2f(uniformUvScale_, 0.5f, 1.0f);
                                glUniform2f(uniformUvOffset_, i == 0 ? 0.0f : 0.5f, 0.0f);
                            } else {
                                glUniform2f(uniformUvScale_, 1.0f, 1.0f);
                                glUniform2f(uniformUvOffset_, 0.0f, 0.0f);
                            }
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        } else {
                            if (i == 0) {
                                renderDebugState_.usedClassic = true;
                            }
                            glDisable(GL_DEPTH_TEST);
                            if (worldAnchoredEnabled_) {
                                const float halfSize = screenScale * kClassicAnchoredZ;
                                const Mat4 model = Mat4Multiply(
                                    navigation,
                                    Mat4Multiply(
                                        Mat4Translation(0.0f, 0.0f, -kClassicAnchoredZ),
                                        Mat4Scale(halfSize, halfSize, 1.0f)));
                                const Mat4 mvp =
                                    Mat4Multiply(projection, Mat4Multiply(view, model));
                                glUniformMatrix4fv(uniformMvp_, 1, GL_FALSE, mvp.m);
                            } else {
                                const Mat4 modelScale = Mat4Scale(screenScale, screenScale, 1.0f);
                                glUniformMatrix4fv(uniformMvp_, 1, GL_FALSE, modelScale.m);
                            }
                            glUniform1f(uniformUseWorldMask_, 0.0f);
                            glUniform1f(uniformLayerWorld_, -1.0f);
                            if (sideBySideFrame_) {
                                const float leftOffset = stereoConvergence;
                                const float rightOffset = 0.5f - stereoConvergence;
                                glUniform2f(uniformUvScale_, 0.5f, 1.0f);
                                glUniform2f(
                                    uniformUvOffset_, i == 0 ? leftOffset : rightOffset, 0.0f);
                            } else {
                                glUniform2f(uniformUvScale_, 1.0f, 1.0f);
                                glUniform2f(uniformUvOffset_, 0.0f, 0.0f);
                            }
                            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        }
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                }

                XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(eye.handle, &releaseInfo);

                projectionViews[i].pose = views_[i].pose;
                projectionViews[i].fov = views_[i].fov;
                projectionViews[i].subImage.swapchain = eye.handle;
                projectionViews[i].subImage.imageRect.offset = {0, 0};
                projectionViews[i].subImage.imageRect.extent = {eye.width, eye.height};
            }

            projectionLayer.space = appSpace_;
            projectionLayer.viewCount = static_cast<uint32_t>(projectionViews.size());
            projectionLayer.views = projectionViews.data();
        }
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    const XrCompositionLayerBaseHeader* layers[1] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer)};
    if (!projectionViews.empty()) {
        endInfo.layerCount = 1;
        endInfo.layers = layers;
    } else {
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
    }

    result = xrEndFrame(session_, &endInfo);
    if (XR_FAILED(result)) {
        setError("xrEndFrame", result);
        return false;
    }

    return true;
}

void XrStereoRenderer::shutdown() {
    if (sessionRunning_) {
        endSession();
    }

    destroyInputActions();

    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    if (emuTexture_ != 0) {
        glDeleteTextures(1, &emuTexture_);
        emuTexture_ = 0;
    }
    if (worldTexture_ != 0) {
        glDeleteTextures(1, &worldTexture_);
        worldTexture_ = 0;
    }
    if (depthRenderbuffer_ != 0) {
        glDeleteRenderbuffers(1, &depthRenderbuffer_);
        depthRenderbuffer_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }

    destroySwapchains();

    if (appSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(appSpace_);
        appSpace_ = XR_NULL_HANDLE;
    }
    if (session_ != XR_NULL_HANDLE) {
        xrDestroySession(session_);
        session_ = XR_NULL_HANDLE;
    }
    if (instance_ != XR_NULL_HANDLE) {
        xrDestroyInstance(instance_);
        instance_ = XR_NULL_HANDLE;
    }

    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglContext_ != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay_, eglContext_);
        }
        if (eglSurface_ != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay_, eglSurface_);
        }
        eglTerminate(eglDisplay_);
    }

    eglDisplay_ = EGL_NO_DISPLAY;
    eglContext_ = EGL_NO_CONTEXT;
    eglSurface_ = EGL_NO_SURFACE;
    eglConfig_ = nullptr;

    configViews_.clear();
    views_.clear();
    lastError_.clear();

    initialized_ = false;
    sessionRunning_ = false;
    frameReady_ = false;
    metadataReady_ = false;
    depthMetadataEnabled_ = false;
    worldAnchoredEnabled_ = false;
    sideBySideFrame_ = false;
    exitRequested_ = false;
    frameWidth_ = 0;
    frameHeight_ = 0;
    metadataWidth_ = 0;
    metadataHeight_ = 0;
    metadataFrameId_ = 0;
    layerDataReady_ = false;
    depthBufferWidth_ = 0;
    depthBufferHeight_ = 0;
    headOriginSet_ = false;
    headOrigin_ = {};
    walkThroughOffset_ = {};
    walkThroughYaw_ = 0.0f;
    walkThroughPitch_ = 0.0f;
    worldUpload_.clear();
    disparityUpload_.clear();
    eyeLayers_[0].clear();
    eyeLayers_[1].clear();
    renderDebugState_ = {};
    controllerState_ = ControllerState{};
}

bool XrStereoRenderer::getControllerState(ControllerState& outState) const {
    if (!initialized_) {
        outState = ControllerState{};
        return false;
    }
    outState = controllerState_;
    return true;
}
