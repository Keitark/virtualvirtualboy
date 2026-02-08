#include "renderer_gl.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "log.h"

namespace {

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
    "varying vec2 vUv;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  vec4 c = texture2D(uTex, vUv);\n"
    "  gl_FragColor = vec4(c.r, c.g, c.b, 1.0);\n"
    "}\n";

GLuint CompileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint CreateProgram(const char* vs, const char* fs) {
    const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vs);
    const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fs);
    if (vertex == 0 || fragment == 0) {
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "aPos");
    glBindAttribLocation(program, 1, "aUv");
    glLinkProgram(program);

    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("Program link error: %s", log);
        glDeleteProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return 0;
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return program;
}

}  // namespace

bool GlRenderer::createContext() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(static_cast<EGLDisplay>(display_), &major, &minor)) {
        LOGE("eglInitialize failed");
        return false;
    }

    constexpr EGLint kConfigAttrs[] = {
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };

    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(
            static_cast<EGLDisplay>(display_), kConfigAttrs, &config, 1, &count) ||
        count < 1) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    surface_ = eglCreateWindowSurface(
        static_cast<EGLDisplay>(display_), config, static_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        return false;
    }

    constexpr EGLint kContextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(
        static_cast<EGLDisplay>(display_), config, EGL_NO_CONTEXT, kContextAttrs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }

    if (!eglMakeCurrent(
            static_cast<EGLDisplay>(display_),
            static_cast<EGLSurface>(surface_),
            static_cast<EGLSurface>(surface_),
            static_cast<EGLContext>(context_))) {
        LOGE("eglMakeCurrent failed");
        return false;
    }

    return true;
}

bool GlRenderer::createProgram() {
    program_ = CreateProgram(kVertexShader, kFragmentShader);
    if (program_ == 0) {
        return false;
    }
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "uTex"), 0);
    return true;
}

bool GlRenderer::ensureTexture() {
    if (texture_ != 0) {
        return true;
    }

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

bool GlRenderer::initialize(ANativeWindow* window) {
    shutdown();

    if (window == nullptr) {
        LOGE("Renderer init failed: null window");
        return false;
    }
    window_ = window;

    if (!createContext() || !createProgram() || !ensureTexture()) {
        shutdown();
        return false;
    }

    initialized_ = true;
    LOGI("Renderer initialized");
    return true;
}

void GlRenderer::updateFrame(const uint32_t* pixels, int width, int height) {
    if (!initialized_ || pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);

    if (width != textureWidth_ || height != textureHeight_) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            width,
            height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            pixels);
        textureWidth_ = width;
        textureHeight_ = height;
    } else {
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            width,
            height,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            pixels);
    }
}

void GlRenderer::render() {
    if (!initialized_) {
        return;
    }

    const GLint width = ANativeWindow_getWidth(window_);
    const GLint height = ANativeWindow_getHeight(window_);
    glViewport(0, 0, width, height);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);

    constexpr GLfloat kVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,  // bottom-left
        1.0f,  -1.0f, 1.0f, 1.0f,  // bottom-right
        -1.0f, 1.0f,  0.0f, 0.0f,  // top-left
        1.0f,  1.0f,  1.0f, 0.0f,  // top-right
    };

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kVertices);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(GLfloat),
        reinterpret_cast<const void*>(kVertices + 2));
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_));
}

void GlRenderer::shutdown() {
    if (display_ != nullptr && display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(
            static_cast<EGLDisplay>(display_),
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            EGL_NO_CONTEXT);
    }

    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }

    if (display_ != nullptr && display_ != EGL_NO_DISPLAY) {
        if (context_ != nullptr && context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(
                static_cast<EGLDisplay>(display_), static_cast<EGLContext>(context_));
        }
        if (surface_ != nullptr && surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(
                static_cast<EGLDisplay>(display_), static_cast<EGLSurface>(surface_));
        }
        eglTerminate(static_cast<EGLDisplay>(display_));
    }

    context_ = nullptr;
    surface_ = nullptr;
    display_ = nullptr;
    window_ = nullptr;
    textureWidth_ = 0;
    textureHeight_ = 0;
    initialized_ = false;
}
