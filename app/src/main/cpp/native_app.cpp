#include <android/keycodes.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audio_player.h"
#include "libretro_vb_core.h"
#include "log.h"
#include "renderer_gl.h"
#include "xr_stereo_renderer.h"

namespace {

constexpr auto kFrameTarget = std::chrono::milliseconds(20);  // ~50 FPS for VB content.
constexpr int kRomReloadFrames = 120;
constexpr float kDefaultScreenScale = 0.62f;
constexpr float kDefaultStereoConvergence = -0.04f;
constexpr float kMinScreenScale = 0.20f;
constexpr float kMaxScreenScale = 1.00f;
constexpr float kMinStereoConvergence = -0.08f;
constexpr float kMaxStereoConvergence = 0.08f;
constexpr float kScreenScaleStep = 0.03f;
constexpr float kStereoConvergenceStep = 0.004f;
constexpr float kWalkOffsetStep = 0.022f;
constexpr float kWalkOffsetLimit = 30.0f;
constexpr float kWalkYawStep = 0.045f;
constexpr float kWalkPitchStep = 0.035f;
constexpr float kWalkPitchLimit = 1.20f;
constexpr float kWalkStickDeadzone = 0.18f;
constexpr char kPresentationSettingsFile[] = "presentation_settings.cfg";
constexpr int kStandbyFrameWidth = 768;
constexpr int kStandbyFrameHeight = 384;
constexpr auto kInfoHintBlinkPeriod = std::chrono::milliseconds(500);

struct PendingRom {
    std::mutex mutex;
    std::vector<uint8_t> bytes;
    std::string name;
    bool ready = false;
};

PendingRom gPendingRom;

struct PickerSignal {
    std::mutex mutex;
    bool dismissed = false;
};

PickerSignal gPickerSignal;

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

bool TakePickerDismissedSignal() {
    std::scoped_lock lock(gPickerSignal.mutex);
    if (!gPickerSignal.dismissed) {
        return false;
    }
    gPickerSignal.dismissed = false;
    return true;
}

using Glyph = std::array<uint8_t, 7>;

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kTextScale = 2;
constexpr int kTextSpacing = 1;

const Glyph* GetGlyph(char ch) {
    static constexpr Glyph kBlank = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr Glyph kColon = {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00};
    static constexpr Glyph kDot = {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06};
    static constexpr Glyph kDash = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    static constexpr Glyph kPlus = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
    static constexpr Glyph kSlash = {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00};
    static constexpr Glyph kLeftParen = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
    static constexpr Glyph kRightParen = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};

    static constexpr Glyph k0 = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    static constexpr Glyph k1 = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static constexpr Glyph k2 = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    static constexpr Glyph k3 = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    static constexpr Glyph k4 = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    static constexpr Glyph k5 = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    static constexpr Glyph k6 = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    static constexpr Glyph k7 = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static constexpr Glyph k8 = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    static constexpr Glyph k9 = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};

    static constexpr Glyph kA = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static constexpr Glyph kB = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    static constexpr Glyph kC = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    static constexpr Glyph kD = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static constexpr Glyph kE = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static constexpr Glyph kF = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static constexpr Glyph kG = {0x0E, 0x11, 0x10, 0x10, 0x13, 0x11, 0x0F};
    static constexpr Glyph kH = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static constexpr Glyph kI = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    static constexpr Glyph kJ = {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
    static constexpr Glyph kK = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static constexpr Glyph kL = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    static constexpr Glyph kM = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static constexpr Glyph kN = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static constexpr Glyph kO = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static constexpr Glyph kP = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static constexpr Glyph kQ = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    static constexpr Glyph kR = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    static constexpr Glyph kS = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static constexpr Glyph kT = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static constexpr Glyph kU = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static constexpr Glyph kV = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    static constexpr Glyph kW = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    static constexpr Glyph kX = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    static constexpr Glyph kY = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    static constexpr Glyph kZ = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};

    switch (ch) {
        case ' ':
            return &kBlank;
        case ':':
            return &kColon;
        case '.':
            return &kDot;
        case '-':
            return &kDash;
        case '+':
            return &kPlus;
        case '/':
            return &kSlash;
        case '(':
            return &kLeftParen;
        case ')':
            return &kRightParen;
        case '0':
            return &k0;
        case '1':
            return &k1;
        case '2':
            return &k2;
        case '3':
            return &k3;
        case '4':
            return &k4;
        case '5':
            return &k5;
        case '6':
            return &k6;
        case '7':
            return &k7;
        case '8':
            return &k8;
        case '9':
            return &k9;
        case 'A':
            return &kA;
        case 'B':
            return &kB;
        case 'C':
            return &kC;
        case 'D':
            return &kD;
        case 'E':
            return &kE;
        case 'F':
            return &kF;
        case 'G':
            return &kG;
        case 'H':
            return &kH;
        case 'I':
            return &kI;
        case 'J':
            return &kJ;
        case 'K':
            return &kK;
        case 'L':
            return &kL;
        case 'M':
            return &kM;
        case 'N':
            return &kN;
        case 'O':
            return &kO;
        case 'P':
            return &kP;
        case 'Q':
            return &kQ;
        case 'R':
            return &kR;
        case 'S':
            return &kS;
        case 'T':
            return &kT;
        case 'U':
            return &kU;
        case 'V':
            return &kV;
        case 'W':
            return &kW;
        case 'X':
            return &kX;
        case 'Y':
            return &kY;
        case 'Z':
            return &kZ;
        default:
            return &kBlank;
    }
}

int TextWidthPixels(const std::string& text, const int scale) {
    if (text.empty()) {
        return 0;
    }
    const int charWidth = (kGlyphWidth * scale) + (kTextSpacing * scale);
    return static_cast<int>(text.size()) * charWidth - (kTextSpacing * scale);
}

std::string ToUpperAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string FitTextToWidth(std::string text, const int maxWidthPx, const int scale) {
    if (maxWidthPx <= 0) {
        return {};
    }
    text = ToUpperAscii(text);
    while (!text.empty() && TextWidthPixels(text, scale) > maxWidthPx) {
        text.pop_back();
    }
    return text;
}

std::string BasenameFromPath(const std::string& path) {
    if (path.empty()) {
        return "NONE";
    }
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos || slash + 1 >= path.size()) {
        return path;
    }
    return path.substr(slash + 1);
}

void FillRect(
    std::vector<uint32_t>& frame,
    const int frameWidth,
    const int frameHeight,
    int x,
    int y,
    int width,
    int height,
    const uint32_t color) {
    if (frameWidth <= 0 || frameHeight <= 0 || frame.empty() || width <= 0 || height <= 0) {
        return;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > frameWidth) {
        width = frameWidth - x;
    }
    if (y + height > frameHeight) {
        height = frameHeight - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int row = 0; row < height; ++row) {
        uint32_t* dst = frame.data() + static_cast<size_t>(y + row) * frameWidth + x;
        std::fill(dst, dst + width, color);
    }
}

void DrawText(
    std::vector<uint32_t>& frame,
    const int frameWidth,
    const int frameHeight,
    const std::string& text,
    int x,
    const int y,
    const int scale,
    const uint32_t color) {
    const int advance = (kGlyphWidth * scale) + (kTextSpacing * scale);
    const std::string upper = ToUpperAscii(text);
    for (const char ch : upper) {
        const Glyph* glyph = GetGlyph(ch);
        if (glyph == nullptr) {
            x += advance;
            continue;
        }

        for (int row = 0; row < kGlyphHeight; ++row) {
            const uint8_t bits = (*glyph)[row];
            for (int col = 0; col < kGlyphWidth; ++col) {
                if ((bits & (1u << (kGlyphWidth - 1 - col))) == 0) {
                    continue;
                }
                const int px = x + (col * scale);
                const int py = y + (row * scale);
                FillRect(frame, frameWidth, frameHeight, px, py, scale, scale, color);
            }
        }
        x += advance;
    }
}

void DrawInfoPanel(
    std::vector<uint32_t>& frame,
    const int frameWidth,
    const int frameHeight,
    const int eyeOffsetX,
    const int eyeWidth,
    const std::vector<std::string>& lines) {
    if (eyeWidth <= 0 || lines.empty()) {
        return;
    }

    const int lineHeight = (kGlyphHeight * kTextScale) + 1;
    const int padding = 6;
    const int panelWidth = std::min(eyeWidth - 12, 360);
    const int panelHeight = (padding * 2) + (lineHeight * static_cast<int>(lines.size()));
    if (panelWidth <= 0 || panelHeight <= 0) {
        return;
    }

    const int panelX = eyeOffsetX + ((eyeWidth - panelWidth) / 2);
    const int panelY = (frameHeight - panelHeight) / 2;

    FillRect(frame, frameWidth, frameHeight, panelX, panelY, panelWidth, panelHeight, 0xFF080808);
    FillRect(frame, frameWidth, frameHeight, panelX, panelY, panelWidth, 2, 0xFFFFFFFF);
    FillRect(
        frame, frameWidth, frameHeight, panelX, panelY + panelHeight - 2, panelWidth, 2, 0xFFFFFFFF);
    FillRect(frame, frameWidth, frameHeight, panelX, panelY, 2, panelHeight, 0xFFFFFFFF);
    FillRect(
        frame, frameWidth, frameHeight, panelX + panelWidth - 2, panelY, 2, panelHeight, 0xFFFFFFFF);

    const int textX = panelX + padding;
    const int maxTextWidth = panelWidth - (padding * 2);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string fitted = FitTextToWidth(lines[i], maxTextWidth, kTextScale);
        const int textY = panelY + padding + (static_cast<int>(i) * lineHeight);
        DrawText(frame, frameWidth, frameHeight, fitted, textX, textY, kTextScale, 0xFFFFFFFF);
    }
}

class App {
public:
    enum class ViewMode : int {
        Classic = 0,
        Anchored = 2,
    };

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
                if (!presentationLoaded_) {
                    loadPresentationSettings();
                    presentationLoaded_ = true;
                }
                if (!xrRenderer_.initialized()) {
                    const bool xrOk = xrRenderer_.initialize(app_->activity);
                    LOGI("OpenXR init: %d", xrOk ? 1 : 0);
                    if (!xrOk && !std::string(xrRenderer_.lastError()).empty()) {
                        LOGW("OpenXR fallback reason: %s", xrRenderer_.lastError());
                    }
                }
                if (xrRenderer_.initialized()) {
                    applyPresentationConfig();
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
        const int32_t eventType = AInputEvent_getType(event);
        if (eventType == AINPUT_EVENT_TYPE_KEY) {
            const int32_t action = AKeyEvent_getAction(event);
            if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) {
                return 0;
            }

            const int32_t keyCode = AKeyEvent_getKeyCode(event);
            const bool pressed = action == AKEY_EVENT_ACTION_DOWN;
            lastKeyCode_ = keyCode;
            if (pressed) {
                LOGI("key event: code=%d", keyCode);
            }

            switch (keyCode) {
                case AKEYCODE_DPAD_LEFT:
                    dpadLeft_ = pressed;
                    updateDirectionalState();
                    return 1;
                case AKEYCODE_DPAD_RIGHT:
                    dpadRight_ = pressed;
                    updateDirectionalState();
                    return 1;
                case AKEYCODE_DPAD_UP:
                    dpadUp_ = pressed;
                    updateDirectionalState();
                    return 1;
                case AKEYCODE_DPAD_DOWN:
                    dpadDown_ = pressed;
                    updateDirectionalState();
                    return 1;
                case AKEYCODE_BUTTON_A:
                case AKEYCODE_BUTTON_1:
                    input_.a = pressed;
                    return 1;
                case AKEYCODE_BUTTON_B:
                case AKEYCODE_BUTTON_2:
                    input_.b = pressed;
                    return 1;
                case AKEYCODE_BUTTON_C:
                    input_.select = pressed;
                    return 1;
                case AKEYCODE_BUTTON_L1:
                    buttonL_ = pressed;
                    updateShoulderState();
                    return 1;
                case AKEYCODE_BUTTON_R1:
                    buttonR_ = pressed;
                    updateShoulderState();
                    return 1;
                case AKEYCODE_BUTTON_L2:
                    triggerButtonL_ = pressed;
                    updateShoulderState();
                    return 1;
                case AKEYCODE_BUTTON_R2:
                    triggerButtonR_ = pressed;
                    updateShoulderState();
                    return 1;
                case AKEYCODE_BUTTON_START:
                case AKEYCODE_BUTTON_Y:
                case AKEYCODE_BUTTON_4:
                    // Quest controllers expose Y reliably; treat it as Start for title/menu flows.
                    input_.start = pressed;
                    return 1;
                case AKEYCODE_BUTTON_THUMBR:
                case AKEYCODE_F1:
                    handleInfoToggleInput(pressed);
                    return 1;
                case AKEYCODE_BUTTON_SELECT:
                case AKEYCODE_BUTTON_3:
                    input_.select = pressed;
                    return 1;
                case AKEYCODE_BUTTON_THUMBL:
                    if (pressed) {
                        requestRomPicker();
                    }
                    return 1;
                case AKEYCODE_BUTTON_X:
                    input_.select = pressed;
                    return 1;
                default:
                    return 0;
            }
        }

        if (eventType == AINPUT_EVENT_TYPE_MOTION) {
            constexpr float kStickDeadzone = 0.35f;
            constexpr float kHatThreshold = 0.5f;
            constexpr float kTriggerThreshold = 0.4f;

            const float stickX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
            const float stickY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
            const float hatX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
            const float hatY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
            const float lTrigger = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0);
            const float rTrigger = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);

            stickLeft_ = stickX < -kStickDeadzone || hatX < -kHatThreshold;
            stickRight_ = stickX > kStickDeadzone || hatX > kHatThreshold;
            stickUp_ = stickY > kStickDeadzone || hatY > kHatThreshold;
            stickDown_ = stickY < -kStickDeadzone || hatY < -kHatThreshold;
            triggerAxisL_ = lTrigger > kTriggerThreshold;
            triggerAxisR_ = rTrigger > kTriggerThreshold;

            updateDirectionalState();
            updateShoulderState();
            return 1;
        }

        return 0;
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

        XrStereoRenderer::ControllerState xrState{};
        if (xrRenderer_.initialized()) {
            xrRenderer_.getControllerState(xrState);
            xrRenderer_.setOverlayVisible(showInfoWindow_);
        }
        if (xrState.rightThumbClick && !prevXrRightThumbClick_) {
            toggleInfoWindow();
        }

        std::vector<uint8_t> pickedRom;
        std::string pickedName;
        if (TakePendingRom(pickedRom, pickedName)) {
            if (core_.loadRomFromBytes(pickedRom.data(), pickedRom.size(), pickedName)) {
                LOGI("ROM loaded from picker: %s", pickedName.c_str());
                autoPickerLaunchedForMissingRom_ = false;
            } else {
                LOGE("Picker ROM load failed: %s", core_.lastError().c_str());
            }
            pickerRequested_ = false;
            if (autoPickerRestoreInfoWindow_) {
                showInfoWindow_ = true;
                autoPickerRestoreInfoWindow_ = false;
            }
        } else if (TakePickerDismissedSignal()) {
            pickerRequested_ = false;
            LOGI("ROM picker dismissed");
            if (autoPickerRestoreInfoWindow_) {
                showInfoWindow_ = true;
                autoPickerRestoreInfoWindow_ = false;
            }
        }

        if (xrState.leftThumbClick && !prevXrLeftThumbClick_) {
            requestRomPicker();
        }

        if (!core_.isRomLoaded()) {

            if (reloadCounter_ <= 0) {
                tryLoadDefaultRom();
                reloadCounter_ = kRomReloadFrames;
            } else {
                reloadCounter_--;
            }

            int standbyWidth = 0;
            int standbyHeight = 0;
            const uint32_t* standbyPixels = composeStandbyFrame(standbyWidth, standbyHeight);
            if (xrRenderer_.initialized()) {
                xrRenderer_.updateFrame(standbyPixels, standbyWidth, standbyHeight);
                const bool xrRendered = xrRenderer_.renderFrame();
                if (!xrRendered && renderer_.initialized()) {
                    renderer_.updateFrame(standbyPixels, standbyWidth, standbyHeight);
                    renderer_.render();
                }
            } else if (renderer_.initialized()) {
                renderer_.updateFrame(standbyPixels, standbyWidth, standbyHeight);
                renderer_.render();
            }
        } else {
            VbInputState mergedInput = input_;
            mergedInput.left = mergedInput.left || xrState.left;
            mergedInput.right = mergedInput.right || xrState.right;
            mergedInput.up = mergedInput.up || xrState.up;
            mergedInput.down = mergedInput.down || xrState.down;
            mergedInput.a = mergedInput.a || xrState.a;
            mergedInput.b = mergedInput.b || xrState.b;
            mergedInput.l = mergedInput.l || xrState.l;
            mergedInput.r = mergedInput.r || xrState.r;
            mergedInput.start = mergedInput.start || xrState.start;
            mergedInput.select = mergedInput.select || xrState.select;

            applyCalibrationInput(mergedInput);
            applyDepthWalkthroughControls(xrState, mergedInput);
            core_.setInputState(mergedInput);
            core_.runFrame();
            pumpAudio();
            if (core_.hasFrame()) {
                const auto& sourceFrame = core_.framePixels();
                const int width = core_.frameWidth();
                const int height = core_.frameHeight();
                const uint32_t* renderPixels = composeRenderFrame(sourceFrame, width, height);

                if (xrRenderer_.initialized()) {
                    xrRenderer_.updateFrame(renderPixels, width, height);
                    const bool xrRendered = xrRenderer_.renderFrame();
                    if (!xrRendered && renderer_.initialized()) {
                        renderer_.updateFrame(renderPixels, width, height);
                        renderer_.render();
                    }
                } else if (renderer_.initialized()) {
                    renderer_.updateFrame(renderPixels, width, height);
                    renderer_.render();
                }
            }
        }

        prevXrLeftThumbClick_ = xrState.leftThumbClick;
        prevXrRightThumbClick_ = xrState.rightThumbClick;
        updateFps(std::chrono::steady_clock::now());

        const auto frameElapsed = std::chrono::steady_clock::now() - frameStart;
        if (frameElapsed < kFrameTarget) {
            std::this_thread::sleep_for(kFrameTarget - frameElapsed);
        }
    }

    void shutdown() {
        audioPlayer_.shutdown();
        xrRenderer_.shutdown();
        renderer_.shutdown();
        core_.shutdown();
    }

private:
    void pumpAudio() {
        if (!core_.isRomLoaded()) {
            return;
        }

        if (!audioPlayer_.ensureStarted(core_.audioSampleRate(), 2)) {
            return;
        }

        std::array<int16_t, 2048 * 2> pcmChunk{};
        while (true) {
            const size_t frames = core_.drainAudioFrames(pcmChunk.data(), 2048);
            if (frames == 0) {
                break;
            }
            audioPlayer_.writeFrames(pcmChunk.data(), static_cast<int32_t>(frames));
            if (frames < 2048) {
                break;
            }
        }
    }

    void handleInfoToggleInput(const bool pressed) {
        if (pressed && !infoToggleHeld_) {
            toggleInfoWindow();
        }
        infoToggleHeld_ = pressed;
    }

    void toggleInfoWindow() {
        showInfoWindow_ = !showInfoWindow_;
        LOGI("Info window %s", showInfoWindow_ ? "enabled" : "disabled");
    }

    const char* viewModeName() const {
        switch (viewMode_) {
            case ViewMode::Classic:
                return "CLASSIC";
            case ViewMode::Anchored:
                return "ANCHORED";
            default:
                return "CLASSIC";
        }
    }

    bool isDepthModeEnabled() const { return false; }
    bool isWorldAnchoredMode() const { return viewMode_ == ViewMode::Anchored; }

    void toggleDepthViewMode() {
        viewMode_ = (viewMode_ == ViewMode::Classic) ? ViewMode::Anchored : ViewMode::Classic;
        applyPresentationConfig();
        savePresentationSettings();
        LOGI("View mode: %s", viewModeName());
    }

    void applyDepthWalkthroughControls(
        const XrStereoRenderer::ControllerState& xrState, VbInputState& inputState) {
        if (!xrRenderer_.initialized()) {
            return;
        }

        const bool gripHeld = xrState.leftGrip || xrState.rightGrip;
        if (!isWorldAnchoredMode() || !gripHeld) {
            walkResetHeld_ = false;
            xrRenderer_.setWalkthroughOffset(walkOffsetX_, walkOffsetY_, walkOffsetZ_);
            xrRenderer_.setWalkthroughRotation(walkYaw_, walkPitch_);
            return;
        }

        auto applyAxis = [](const float value) {
            return (value > kWalkStickDeadzone || value < -kWalkStickDeadzone) ? value : 0.0f;
        };

        const float strafe = applyAxis(xrState.leftStickX);
        const float forward = applyAxis(xrState.leftStickY);
        const float turnYaw = applyAxis(xrState.rightStickX);
        const float turnPitch = applyAxis(xrState.rightStickY);
        const float rise = (xrState.r ? 1.0f : 0.0f) - (xrState.l ? 1.0f : 0.0f);

        walkYaw_ += turnYaw * kWalkYawStep;
        walkPitch_ = std::clamp(
            walkPitch_ + (turnPitch * kWalkPitchStep), -kWalkPitchLimit, kWalkPitchLimit);

        const float sinYaw = std::sin(walkYaw_);
        const float cosYaw = std::cos(walkYaw_);
        const float deltaX = (cosYaw * strafe) + (sinYaw * forward);
        const float deltaZ = (sinYaw * strafe) - (cosYaw * forward);

        walkOffsetX_ = std::clamp(
            walkOffsetX_ + (deltaX * kWalkOffsetStep), -kWalkOffsetLimit, kWalkOffsetLimit);
        walkOffsetY_ = std::clamp(
            walkOffsetY_ + (rise * kWalkOffsetStep), -kWalkOffsetLimit, kWalkOffsetLimit);
        walkOffsetZ_ = std::clamp(
            walkOffsetZ_ + (deltaZ * kWalkOffsetStep), -kWalkOffsetLimit, kWalkOffsetLimit);

        if (xrState.a && !walkResetHeld_) {
            resetWalkthroughHome();
        }
        walkResetHeld_ = xrState.a;

        xrRenderer_.setWalkthroughOffset(walkOffsetX_, walkOffsetY_, walkOffsetZ_);
        xrRenderer_.setWalkthroughRotation(walkYaw_, walkPitch_);

        // While grip is held in anchored mode, controls drive walkthrough navigation.
        inputState.left = false;
        inputState.right = false;
        inputState.up = false;
        inputState.down = false;
        inputState.a = false;
        inputState.l = false;
        inputState.r = false;
    }

    void applyPresentationConfig() {
        if (!xrRenderer_.initialized()) {
            return;
        }
        constexpr bool depthEnabled = false;
        const bool worldAnchoredEnabled = isWorldAnchoredMode();
        const float effectiveConvergence = worldAnchoredEnabled ? 0.0f : stereoConvergence_;
        xrRenderer_.setPresentationConfig(screenScale_, effectiveConvergence);
        xrRenderer_.setDepthMetadataEnabled(depthEnabled);
        xrRenderer_.setWorldAnchoredEnabled(worldAnchoredEnabled);
        xrRenderer_.setOverlayVisible(showInfoWindow_);
        xrRenderer_.setWalkthroughOffset(walkOffsetX_, walkOffsetY_, walkOffsetZ_);
        xrRenderer_.setWalkthroughRotation(walkYaw_, walkPitch_);
    }

    void resetWalkthroughHome() {
        walkOffsetX_ = 0.0f;
        walkOffsetY_ = 0.0f;
        walkOffsetZ_ = 0.0f;
        walkYaw_ = 0.0f;
        walkPitch_ = 0.0f;
        walkResetHeld_ = false;

        if (xrRenderer_.initialized()) {
            xrRenderer_.setWalkthroughOffset(walkOffsetX_, walkOffsetY_, walkOffsetZ_);
            xrRenderer_.setWalkthroughRotation(walkYaw_, walkPitch_);
        }
        LOGI("Walkthrough home reset");
    }

    std::string presentationSettingsPath() const {
        if (app_ == nullptr || app_->activity == nullptr) {
            return {};
        }

        const char* internalPath = app_->activity->internalDataPath;
        if (internalPath != nullptr && internalPath[0] != '\0') {
            return std::string(internalPath) + "/" + kPresentationSettingsFile;
        }
        const char* externalPath = app_->activity->externalDataPath;
        if (externalPath != nullptr && externalPath[0] != '\0') {
            return std::string(externalPath) + "/" + kPresentationSettingsFile;
        }
        return {};
    }

    void loadPresentationSettings() {
        screenScale_ = kDefaultScreenScale;
        stereoConvergence_ = kDefaultStereoConvergence;
        viewMode_ = ViewMode::Anchored;

        const std::string path = presentationSettingsPath();
        if (path.empty()) {
            return;
        }

        std::ifstream in(path);
        if (!in.good()) {
            return;
        }

        float loadedScale = screenScale_;
        float loadedConvergence = stereoConvergence_;
        int loadedViewMode = static_cast<int>(viewMode_);
        in >> loadedScale >> loadedConvergence;
        if (!in.fail()) {
            in >> loadedViewMode;
            if (in.fail()) {
                in.clear();
            }
            screenScale_ = std::clamp(loadedScale, kMinScreenScale, kMaxScreenScale);
            stereoConvergence_ =
                std::clamp(loadedConvergence, kMinStereoConvergence, kMaxStereoConvergence);
            viewMode_ = (loadedViewMode <= 0) ? ViewMode::Classic : ViewMode::Anchored;
            LOGI(
                "Loaded presentation settings: scale=%.3f convergence=%.3f viewMode=%d",
                screenScale_,
                stereoConvergence_,
                static_cast<int>(viewMode_));
        }
    }

    void savePresentationSettings() {
        const std::string path = presentationSettingsPath();
        if (path.empty()) {
            return;
        }

        std::ofstream out(path, std::ios::trunc);
        if (!out.good()) {
            LOGW("Failed to save presentation settings: %s", path.c_str());
            return;
        }

        out << std::fixed << std::setprecision(4) << screenScale_ << " " << stereoConvergence_
            << " " << static_cast<int>(viewMode_) << "\n";
    }

    void resetCalibrationEdgeState() {
        adjustUpHeld_ = false;
        adjustDownHeld_ = false;
        adjustLeftHeld_ = false;
        adjustRightHeld_ = false;
        adjustResetHeld_ = false;
    }

    void applyCalibrationInput(VbInputState& inputState) {
        if (showInfoWindow_) {
            if (inputState.b && !depthToggleHeld_) {
                toggleDepthViewMode();
            }
            depthToggleHeld_ = inputState.b;
            inputState.b = false;
        } else {
            depthToggleHeld_ = false;
        }

        if (!showInfoWindow_) {
            resetCalibrationEdgeState();
            return;
        }

        const bool modifierHeld = inputState.l && inputState.r;
        if (!modifierHeld) {
            resetCalibrationEdgeState();
            return;
        }

        bool changed = false;
        if (inputState.up && !adjustUpHeld_) {
            screenScale_ = std::clamp(screenScale_ + kScreenScaleStep, kMinScreenScale, kMaxScreenScale);
            changed = true;
        }
        if (inputState.down && !adjustDownHeld_) {
            screenScale_ = std::clamp(screenScale_ - kScreenScaleStep, kMinScreenScale, kMaxScreenScale);
            changed = true;
        }
        if (inputState.right && !adjustRightHeld_) {
            stereoConvergence_ = std::clamp(
                stereoConvergence_ + kStereoConvergenceStep,
                kMinStereoConvergence,
                kMaxStereoConvergence);
            changed = true;
        }
        if (inputState.left && !adjustLeftHeld_) {
            stereoConvergence_ = std::clamp(
                stereoConvergence_ - kStereoConvergenceStep,
                kMinStereoConvergence,
                kMaxStereoConvergence);
            changed = true;
        }
        if (inputState.a && !adjustResetHeld_) {
            screenScale_ = kDefaultScreenScale;
            stereoConvergence_ = kDefaultStereoConvergence;
            changed = true;
        }
        adjustUpHeld_ = inputState.up;
        adjustDownHeld_ = inputState.down;
        adjustLeftHeld_ = inputState.left;
        adjustRightHeld_ = inputState.right;
        adjustResetHeld_ = inputState.a;

        if (changed) {
            applyPresentationConfig();
            savePresentationSettings();
            LOGI(
                "Updated presentation settings: scale=%.3f convergence=%.3f viewMode=%d",
                screenScale_,
                stereoConvergence_,
                static_cast<int>(viewMode_));
        }

        // Consume calibration controls while both shoulders are held.
        inputState.left = false;
        inputState.right = false;
        inputState.up = false;
        inputState.down = false;
        inputState.a = false;
        inputState.l = false;
        inputState.r = false;
        inputState.b = false;
    }

    void updateFps(const std::chrono::steady_clock::time_point now) {
        fpsFrameCount_++;
        const auto elapsed = now - fpsWindowStart_;
        if (elapsed >= std::chrono::seconds(1)) {
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            if (elapsedMs > 0) {
                fps_ = static_cast<double>(fpsFrameCount_) * 1000.0 / static_cast<double>(elapsedMs);
            }
            fpsFrameCount_ = 0;
            fpsWindowStart_ = now;
        }
    }

    std::vector<std::string> buildInfoLines() const {
        std::vector<std::string> lines;
        lines.reserve(12);
        const auto nowTicks = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        const bool blinkOn =
            ((nowTicks / kInfoHintBlinkPeriod.count()) % 2) == 0;
        lines.emplace_back(blinkOn ? "PUSH RIGHT STICK TO CLOSE" : " ");

        std::ostringstream fpsText;
        fpsText << std::fixed << std::setprecision(1) << fps_;
        lines.emplace_back("FPS: " + fpsText.str());

        if (core_.isRomLoaded()) {
            lines.emplace_back("ROM: " + BasenameFromPath(core_.romLabel()));
        } else {
            lines.emplace_back("ROM: NONE");
        }

        lines.emplace_back("ROM PICKER: HIDE INFO + L3");
        lines.emplace_back(std::string("VIEW: ") + viewModeName() + " (TOGGLE \"B\")");

        if (isWorldAnchoredMode()) {
            lines.emplace_back("NAV (HOLD ANY GRIP)");
            lines.emplace_back("  L-STICK: MOVE");
            lines.emplace_back("  R-STICK: LOOK");
            lines.emplace_back("  L/R TRIGGER: UP/DOWN");
            lines.emplace_back("  A: RESET VIEW");
        }

        {
            std::ostringstream scaleText;
            scaleText << std::fixed << std::setprecision(2) << screenScale_;
            lines.emplace_back("SCREEN SIZE: " + scaleText.str());
        }

        if (!isWorldAnchoredMode()) {
            std::ostringstream convergenceText;
            convergenceText << std::fixed << std::setprecision(3)
                            << stereoConvergence_;
            lines.emplace_back("STEREO CONV: " + convergenceText.str());
            lines.emplace_back("CALIB: HOLD L+R");
            lines.emplace_back("U/D SIZE, L/R CONV, A RESET");
        } else {
            lines.emplace_back("CALIB: HOLD L+R");
            lines.emplace_back("U/D SIZE, A RESET");
        }

        return lines;
    }

    const uint32_t* composeStandbyFrame(int& outWidth, int& outHeight) {
        outWidth = kStandbyFrameWidth;
        outHeight = kStandbyFrameHeight;
        standbyFrame_.assign(
            static_cast<size_t>(kStandbyFrameWidth) * static_cast<size_t>(kStandbyFrameHeight),
            0xFF000000);

        const bool canDrawMonoText = kStandbyFrameWidth > 40 && kStandbyFrameHeight > 40;
        const bool sideBySideStandby = kStandbyFrameWidth >= (kStandbyFrameHeight * 2);
        const int eyeWidth = sideBySideStandby ? (kStandbyFrameWidth / 2) : kStandbyFrameWidth;
        auto drawStandbyText = [&](const char* text, const int x, const int y) {
            if (sideBySideStandby) {
                DrawText(
                    standbyFrame_, kStandbyFrameWidth, kStandbyFrameHeight, text, x, y, 2, 0xFFFFFFFF);
                DrawText(
                    standbyFrame_,
                    kStandbyFrameWidth,
                    kStandbyFrameHeight,
                    text,
                    x + eyeWidth,
                    y,
                    2,
                    0xFFFFFFFF);
            } else {
                DrawText(
                    standbyFrame_, kStandbyFrameWidth, kStandbyFrameHeight, text, x, y, 2, 0xFFFFFFFF);
            }
        };

        if (canDrawMonoText) {
            drawStandbyText("NO ROM LOADED", 18, 18);

            if (showInfoWindow_) {
                drawStandbyText("R3: HIDE INFO", 18, 40);
            } else {
                drawStandbyText("L3: OPEN ROM PICKER", 18, 40);
                drawStandbyText("R3: SHOW INFO", 18, 62);
            }
        }

        if (showInfoWindow_) {
            const std::vector<std::string> lines = buildInfoLines();
            if (sideBySideStandby) {
                DrawInfoPanel(standbyFrame_, kStandbyFrameWidth, kStandbyFrameHeight, 0, eyeWidth, lines);
                DrawInfoPanel(
                    standbyFrame_,
                    kStandbyFrameWidth,
                    kStandbyFrameHeight,
                    eyeWidth,
                    eyeWidth,
                    lines);
            } else {
                DrawInfoPanel(standbyFrame_, kStandbyFrameWidth, kStandbyFrameHeight, 0, eyeWidth, lines);
            }
        }

        return standbyFrame_.data();
    }

    const uint32_t* composeRenderFrame(
        const std::vector<uint32_t>& sourceFrame, const int width, const int height) {
        if (!showInfoWindow_) {
            return sourceFrame.data();
        }

        overlayFrame_ = sourceFrame;
        const std::vector<std::string> lines = buildInfoLines();

        if (width >= (height * 2)) {
            const int eyeWidth = width / 2;
            DrawInfoPanel(overlayFrame_, width, height, 0, eyeWidth, lines);
            DrawInfoPanel(overlayFrame_, width, height, eyeWidth, eyeWidth, lines);
        } else {
            DrawInfoPanel(overlayFrame_, width, height, 0, width, lines);
        }

        return overlayFrame_.data();
    }

    void updateDirectionalState() {
        input_.left = dpadLeft_ || stickLeft_;
        input_.right = dpadRight_ || stickRight_;
        input_.up = dpadUp_ || stickUp_;
        input_.down = dpadDown_ || stickDown_;
    }

    void updateShoulderState() {
        input_.l = buttonL_ || triggerButtonL_ || triggerAxisL_;
        input_.r = buttonR_ || triggerButtonR_ || triggerAxisR_;
    }

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

        if (!pickerRequested_ && !autoPickerLaunchedForMissingRom_) {
            requestRomPicker(true);
        }
    }

    void requestRomPicker(const bool autoLaunchIfInfoShown = false) {
        if (showInfoWindow_ && !autoLaunchIfInfoShown) {
            return;
        }
        if (pickerRequested_ || app_ == nullptr || app_->activity == nullptr ||
            app_->activity->vm == nullptr || app_->activity->clazz == nullptr) {
            return;
        }

        const bool restoreInfoAfterPicker = autoLaunchIfInfoShown && showInfoWindow_;
        if (restoreInfoAfterPicker) {
            showInfoWindow_ = false;
            autoPickerRestoreInfoWindow_ = true;
        }

        JNIEnv* env = nullptr;
        bool attached = false;
        if (app_->activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (app_->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                LOGE("Failed to attach JNI thread for ROM picker");
                if (restoreInfoAfterPicker) {
                    showInfoWindow_ = true;
                    autoPickerRestoreInfoWindow_ = false;
                }
                return;
            }
            attached = true;
        }

        jclass activityClass = env->GetObjectClass(app_->activity->clazz);
        if (activityClass == nullptr) {
            if (restoreInfoAfterPicker) {
                showInfoWindow_ = true;
                autoPickerRestoreInfoWindow_ = false;
            }
            if (attached) {
                app_->activity->vm->DetachCurrentThread();
            }
            return;
        }

        jmethodID openPickerMethod = env->GetMethodID(activityClass, "openRomPicker", "()V");
        bool pickerLaunched = false;
        if (openPickerMethod != nullptr) {
            env->CallVoidMethod(app_->activity->clazz, openPickerMethod);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            } else {
                pickerRequested_ = true;
                pickerLaunched = true;
                if (autoLaunchIfInfoShown) {
                    autoPickerLaunchedForMissingRom_ = true;
                }
                LOGI("Requested ROM picker");
            }
        }
        if (!pickerLaunched && restoreInfoAfterPicker) {
            showInfoWindow_ = true;
            autoPickerRestoreInfoWindow_ = false;
        }
        env->DeleteLocalRef(activityClass);

        if (attached) {
            app_->activity->vm->DetachCurrentThread();
        }
    }

    android_app* app_ = nullptr;
    LibretroVbCore core_;
    AudioPlayer audioPlayer_;
    GlRenderer renderer_;
    XrStereoRenderer xrRenderer_;
    VbInputState input_;

    bool running_ = false;
    bool resumed_ = false;
    int reloadCounter_ = 0;
    bool pickerRequested_ = false;
    bool autoPickerLaunchedForMissingRom_ = false;
    bool autoPickerRestoreInfoWindow_ = false;
    int lastKeyCode_ = -1;
    bool prevXrLeftThumbClick_ = false;
    bool prevXrRightThumbClick_ = false;
    bool showInfoWindow_ = true;
    bool infoToggleHeld_ = false;
    std::vector<uint32_t> overlayFrame_;
    std::vector<uint32_t> standbyFrame_;
    int fpsFrameCount_ = 0;
    double fps_ = 0.0;
    std::chrono::steady_clock::time_point fpsWindowStart_ = std::chrono::steady_clock::now();
    bool presentationLoaded_ = false;
    float screenScale_ = kDefaultScreenScale;
    float stereoConvergence_ = kDefaultStereoConvergence;
    bool adjustUpHeld_ = false;
    bool adjustDownHeld_ = false;
    bool adjustLeftHeld_ = false;
    bool adjustRightHeld_ = false;
    bool adjustResetHeld_ = false;
    bool depthToggleHeld_ = false;
    ViewMode viewMode_ = ViewMode::Anchored;
    bool walkResetHeld_ = false;
    float walkOffsetX_ = 0.0f;
    float walkOffsetY_ = 0.0f;
    float walkOffsetZ_ = 0.0f;
    float walkYaw_ = 0.0f;
    float walkPitch_ = 0.0f;
    bool dpadLeft_ = false;
    bool dpadRight_ = false;
    bool dpadUp_ = false;
    bool dpadDown_ = false;
    bool stickLeft_ = false;
    bool stickRight_ = false;
    bool stickUp_ = false;
    bool stickDown_ = false;
    bool buttonL_ = false;
    bool buttonR_ = false;
    bool triggerButtonL_ = false;
    bool triggerButtonR_ = false;
    bool triggerAxisL_ = false;
    bool triggerAxisR_ = false;
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
Java_com_keitark_vrboy_MainActivity_nativeOnRomSelected(
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

extern "C" JNIEXPORT void JNICALL
Java_com_keitark_vrboy_MainActivity_nativeOnRomPickerDismissed(
    JNIEnv* /*env*/, jobject /*thiz*/) {
    std::scoped_lock lock(gPickerSignal.mutex);
    gPickerSignal.dismissed = true;
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
