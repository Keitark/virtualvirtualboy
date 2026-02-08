#include <android/keycodes.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libretro_vb_core.h"
#include "log.h"
#include "renderer_gl.h"
#include "xr_stereo_renderer.h"

namespace {

constexpr auto kFrameTarget = std::chrono::milliseconds(20);  // ~50 FPS for VB content.
constexpr int kRomReloadFrames = 120;

struct PendingRom {
    std::mutex mutex;
    std::vector<uint8_t> bytes;
    std::string name;
    bool ready = false;
};

PendingRom gPendingRom;

bool TakePendingRom(std::vector<uint8_t>& outBytes, std::string& outName) {
    std::scoped_lock lock(gPendingRom.mutex);
    if (!gPendingRom.ready || gPendingRom.bytes.empty()) {
        return false;
    }

    outBytes = std::move(gPendingRom.bytes);
    outName = std::move(gPendingRom.name);
    gPendingRom.ready = false;
    gPendingRom.bytes.clear();
    gPendingRom.name.clear();
    return true;
}

class App {
public:
    explicit App(android_app* app) : app_(app) {}

    void onCmd(const int32_t cmd) {
        switch (cmd) {
            case APP_CMD_START:
                running_ = true;
                break;
            case APP_CMD_RESUME:
                resumed_ = true;
                break;
            case APP_CMD_PAUSE:
                resumed_ = false;
                break;
            case APP_CMD_STOP:
                running_ = false;
                break;
            case APP_CMD_INIT_WINDOW:
                if (!core_.isInitialized()) {
                    core_.initialize();
                }
                if (!xrRenderer_.initialized()) {
                    const bool xrOk = xrRenderer_.initialize(app_->activity);
                    LOGI("OpenXR init: %d", xrOk ? 1 : 0);
                    if (!xrOk && !std::string(xrRenderer_.lastError()).empty()) {
                        LOGW("OpenXR fallback reason: %s", xrRenderer_.lastError());
                    }
                }
                if (!xrRenderer_.initialized() && !renderer_.initialized() && app_->window != nullptr) {
                    renderer_.initialize(app_->window);
                }
                tryLoadDefaultRom();
                break;
            case APP_CMD_TERM_WINDOW:
                renderer_.shutdown();
                break;
            case APP_CMD_DESTROY:
                shutdown();
                break;
            default:
                break;
        }
    }

    int32_t onInput(const AInputEvent* event) {
        if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY) {
            return 0;
        }

        const int32_t keyCode = AKeyEvent_getKeyCode(event);
        const bool pressed = AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN;

        switch (keyCode) {
            case AKEYCODE_DPAD_LEFT:
                input_.left = pressed;
                return 1;
            case AKEYCODE_DPAD_RIGHT:
                input_.right = pressed;
                return 1;
            case AKEYCODE_DPAD_UP:
                input_.up = pressed;
                return 1;
            case AKEYCODE_DPAD_DOWN:
                input_.down = pressed;
                return 1;
            case AKEYCODE_BUTTON_A:
                input_.a = pressed;
                return 1;
            case AKEYCODE_BUTTON_B:
                input_.b = pressed;
                return 1;
            case AKEYCODE_BUTTON_L1:
                input_.l = pressed;
                return 1;
            case AKEYCODE_BUTTON_R1:
                input_.r = pressed;
                return 1;
            case AKEYCODE_BUTTON_START:
                input_.start = pressed;
                return 1;
            case AKEYCODE_BUTTON_SELECT:
                input_.select = pressed;
                return 1;
            case AKEYCODE_BUTTON_X:
                if (pressed) {
                    requestRomPicker();
                }
                return 1;
            default:
                return 0;
        }
    }

    void tick() {
        if (!running_ || !resumed_) {
            return;
        }

        const auto frameStart = std::chrono::steady_clock::now();
        if (xrRenderer_.initialized()) {
            xrRenderer_.pollEvents();
            if (xrRenderer_.exitRequested()) {
                LOGW("OpenXR requested exit");
                running_ = false;
                return;
            }
        }

        std::vector<uint8_t> pickedRom;
        std::string pickedName;
        if (TakePendingRom(pickedRom, pickedName)) {
            if (core_.loadRomFromBytes(pickedRom.data(), pickedRom.size(), pickedName)) {
                LOGI("ROM loaded from picker: %s", pickedName.c_str());
                pickerRequested_ = false;
            } else {
                LOGE("Picker ROM load failed: %s", core_.lastError().c_str());
            }
        }

        if (!core_.isRomLoaded()) {
            if (reloadCounter_ <= 0) {
                tryLoadDefaultRom();
                reloadCounter_ = kRomReloadFrames;
            } else {
                reloadCounter_--;
            }
        } else {
            core_.setInputState(input_);
            core_.runFrame();
            if (core_.hasFrame()) {
                if (xrRenderer_.initialized()) {
                    xrRenderer_.updateFrame(
                        core_.framePixels().data(), core_.frameWidth(), core_.frameHeight());
                    const bool xrRendered = xrRenderer_.renderFrame();
                    if (!xrRendered && renderer_.initialized()) {
                        renderer_.updateFrame(
                            core_.framePixels().data(), core_.frameWidth(), core_.frameHeight());
                        renderer_.render();
                    }
                } else if (renderer_.initialized()) {
                    renderer_.updateFrame(
                        core_.framePixels().data(), core_.frameWidth(), core_.frameHeight());
                    renderer_.render();
                }
            }
        }

        const auto frameElapsed = std::chrono::steady_clock::now() - frameStart;
        if (frameElapsed < kFrameTarget) {
            std::this_thread::sleep_for(kFrameTarget - frameElapsed);
        }
    }

    void shutdown() {
        xrRenderer_.shutdown();
        renderer_.shutdown();
        core_.shutdown();
    }

private:
    void tryLoadDefaultRom() {
        if (!core_.isInitialized()) {
            return;
        }

        std::vector<std::string> candidates = {
            "/sdcard/Download/test.vb",
            "/sdcard/Download/test.vboy",
            "/sdcard/Download/rom.vb",
        };

        if (app_->activity != nullptr && app_->activity->externalDataPath != nullptr) {
            const std::string base = app_->activity->externalDataPath;
            candidates.emplace_back(base + "/test.vb");
            candidates.emplace_back(base + "/rom.vb");
        }

        for (const auto& candidate : candidates) {
            if (core_.loadRomFromFile(candidate)) {
                LOGI("ROM loaded from %s", candidate.c_str());
                return;
            }
        }

        if (!core_.lastError().empty()) {
            LOGW("ROM not loaded yet. Last error: %s", core_.lastError().c_str());
        }

        if (!pickerRequested_) {
            requestRomPicker();
        }
    }

    void requestRomPicker() {
        if (pickerRequested_ || app_ == nullptr || app_->activity == nullptr ||
            app_->activity->vm == nullptr || app_->activity->clazz == nullptr) {
            return;
        }

        JNIEnv* env = nullptr;
        bool attached = false;
        if (app_->activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (app_->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                LOGE("Failed to attach JNI thread for ROM picker");
                return;
            }
            attached = true;
        }

        jclass activityClass = env->GetObjectClass(app_->activity->clazz);
        if (activityClass == nullptr) {
            if (attached) {
                app_->activity->vm->DetachCurrentThread();
            }
            return;
        }

        jmethodID openPickerMethod = env->GetMethodID(activityClass, "openRomPicker", "()V");
        if (openPickerMethod != nullptr) {
            env->CallVoidMethod(app_->activity->clazz, openPickerMethod);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            } else {
                pickerRequested_ = true;
                LOGI("Requested ROM picker");
            }
        }
        env->DeleteLocalRef(activityClass);

        if (attached) {
            app_->activity->vm->DetachCurrentThread();
        }
    }

    android_app* app_ = nullptr;
    LibretroVbCore core_;
    GlRenderer renderer_;
    XrStereoRenderer xrRenderer_;
    VbInputState input_;

    bool running_ = false;
    bool resumed_ = false;
    int reloadCounter_ = 0;
    bool pickerRequested_ = false;
};

void HandleAppCmd(android_app* app, int32_t cmd) {
    auto* instance = reinterpret_cast<App*>(app->userData);
    if (instance != nullptr) {
        instance->onCmd(cmd);
    }
}

int32_t HandleInput(android_app* app, AInputEvent* event) {
    auto* instance = reinterpret_cast<App*>(app->userData);
    if (instance == nullptr) {
        return 0;
    }
    return instance->onInput(event);
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_keitark_virtualvirtualboy_MainActivity_nativeOnRomSelected(
    JNIEnv* env, jobject /*thiz*/, jbyteArray data, jstring displayName) {
    if (data == nullptr) {
        return;
    }

    const jsize size = env->GetArrayLength(data);
    if (size <= 0) {
        return;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(bytes.data()));

    std::string name = "picked.vb";
    if (displayName != nullptr) {
        const char* chars = env->GetStringUTFChars(displayName, nullptr);
        if (chars != nullptr) {
            name = chars;
            env->ReleaseStringUTFChars(displayName, chars);
        }
    }

    std::scoped_lock lock(gPendingRom.mutex);
    gPendingRom.bytes = std::move(bytes);
    gPendingRom.name = std::move(name);
    gPendingRom.ready = true;
}

void android_main(struct android_app* app) {
    App appInstance(app);
    app->userData = &appInstance;
    app->onAppCmd = HandleAppCmd;
    app->onInputEvent = HandleInput;

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;

        const int timeoutMs = 0;
        while (ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&source)) >=
               0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            if (app->destroyRequested != 0) {
                appInstance.shutdown();
                return;
            }
        }

        appInstance.tick();
    }
}
