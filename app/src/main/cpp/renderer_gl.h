#pragma once

#include <android/native_window.h>
#include <cstdint>

class GlRenderer {
public:
    bool initialize(ANativeWindow* window);
    void shutdown();

    void updateFrame(const uint32_t* pixels, int width, int height);
    void render();

    [[nodiscard]] bool initialized() const { return initialized_; }

private:
    bool createContext();
    bool createProgram();
    bool ensureTexture();

    ANativeWindow* window_ = nullptr;
    void* display_ = nullptr;
    void* surface_ = nullptr;
    void* context_ = nullptr;

    unsigned int program_ = 0;
    unsigned int texture_ = 0;
    int textureWidth_ = 0;
    int textureHeight_ = 0;

    bool initialized_ = false;
};
