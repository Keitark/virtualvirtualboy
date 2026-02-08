#include "xr_stereo_renderer.h"

#include <algorithm>
#include <array>
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
    "attribute vec2 aPos;\n"
    "attribute vec2 aUv;\n"
    "varying vec2 vUv;\n"
    "void main() {\n"
    "  vUv = aUv;\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

constexpr char kFragmentShader[] =
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uUvScale;\n"
    "uniform vec2 uUvOffset;\n"
    "varying vec2 vUv;\n"
    "void main() {\n"
    "  vec2 uv = vUv * uUvScale + uUvOffset;\n"
    "  vec4 c = texture2D(uTex, uv);\n"
    "  float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
    "  gl_FragColor = vec4(l, l * 0.08, l * 0.03, 1.0);\n"
    "}\n";

constexpr float kMinScreenScale = 0.20f;
constexpr float kMaxScreenScale = 1.00f;
constexpr float kMinStereoConvergence = -0.08f;
constexpr float kMaxStereoConvergence = 0.08f;

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
    uniformUvScale_ = glGetUniformLocation(program_, "uUvScale");
    uniformUvOffset_ = glGetUniformLocation(program_, "uUvOffset");

    glGenTextures(1, &emuTexture_);
    glBindTexture(GL_TEXTURE_2D, emuTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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
    XrVector2f move = leftActive ? leftStick : rightStick;
    if (leftActive && rightActive) {
        if ((rightStick.x * rightStick.x + rightStick.y * rightStick.y) >
            (leftStick.x * leftStick.x + leftStick.y * leftStick.y)) {
            move = rightStick;
        }
    }

    controllerState_.left = move.x < -kDeadzone;
    controllerState_.right = move.x > kDeadzone;
    controllerState_.up = move.y < -kDeadzone;
    controllerState_.down = move.y > kDeadzone;

    controllerState_.a = getBooleanActionState(buttonAAction_);
    controllerState_.b = getBooleanActionState(buttonBAction_);
    controllerState_.x = getBooleanActionState(buttonXAction_);
    controllerState_.y = getBooleanActionState(buttonYAction_);

    const float leftTrigger = getFloatActionState(leftTriggerAction_, leftHandPath_);
    const float leftSqueeze = getFloatActionState(leftSqueezeAction_, leftHandPath_);
    const float rightTrigger = getFloatActionState(rightTriggerAction_, rightHandPath_);
    const float rightSqueeze = getFloatActionState(rightSqueezeAction_, rightHandPath_);
    const float leftPress = leftTrigger > leftSqueeze ? leftTrigger : leftSqueeze;
    const float rightPress = rightTrigger > rightSqueeze ? rightTrigger : rightSqueeze;
    controllerState_.l = leftPress > kTriggerPressThreshold;
    controllerState_.r = rightPress > kTriggerPressThreshold;

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
    sessionRunning_ = true;
    return true;
}

void XrStereoRenderer::endSession() {
    if (!sessionRunning_) {
        return;
    }
    xrEndSession(session_);
    sessionRunning_ = false;
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

bool XrStereoRenderer::renderFrame() {
    if (!initialized_) {
        return false;
    }

    pollEvents();
    if (!sessionRunning_) {
        return false;
    }

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
            const GLfloat vertices[] = {
                -screenScale, -screenScale, 0.0f, 1.0f,
                screenScale,  -screenScale, 1.0f, 1.0f,
                -screenScale, screenScale,  0.0f, 0.0f,
                screenScale,  screenScale,  1.0f, 0.0f,
            };

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

                    glViewport(0, 0, eye.width, eye.height);
                    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);

                    if (frameReady_) {
                        glUseProgram(program_);
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, emuTexture_);
                        glUniform1i(uniformTexture_, 0);

                        if (sideBySideFrame_) {
                            const float leftOffset = stereoConvergence;
                            const float rightOffset = 0.5f - stereoConvergence;
                            glUniform2f(uniformUvScale_, 0.5f, 1.0f);
                            glUniform2f(uniformUvOffset_, i == 0 ? leftOffset : rightOffset, 0.0f);
                        } else {
                            glUniform2f(uniformUvScale_, 1.0f, 1.0f);
                            glUniform2f(uniformUvOffset_, 0.0f, 0.0f);
                        }

                        glVertexAttribPointer(
                            0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), vertices);
                        glEnableVertexAttribArray(0);
                        glVertexAttribPointer(
                            1,
                            2,
                            GL_FLOAT,
                            GL_FALSE,
                            4 * sizeof(GLfloat),
                            reinterpret_cast<const void*>(vertices + 2));
                        glEnableVertexAttribArray(1);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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
    sideBySideFrame_ = false;
    exitRequested_ = false;
    frameWidth_ = 0;
    frameHeight_ = 0;
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
