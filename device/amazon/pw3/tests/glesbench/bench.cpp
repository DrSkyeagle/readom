// SPDX-License-Identifier: Apache-2.0
// SwiftShader's GLES on this CPU, step by step, for what SurfaceFlinger asks of
// it when the composer falls back: textured full-screen quads into a
// gralloc-backed target, from gralloc-backed textures (EGLImage, external
// texture), with and without blending, and a read-back. Prints milliseconds
// (best of three). Touches no display hardware.
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <ui/GraphicBuffer.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

using android::GraphicBuffer;
using android::sp;

namespace {
const int W = 1072, H = 1448;

double best(const std::function<void()>& step) {
    double result = 1e9;
    for (int i = 0; i < 3; ++i) {
        const auto start = std::chrono::steady_clock::now();
        step();
        glFinish();
        const auto end = std::chrono::steady_clock::now();
        result = std::min(result, std::chrono::duration<double, std::milli>(end - start).count());
    }
    return result;
}

GLuint program(const char* fragment, bool external) {
    const char* vertex =
        "attribute vec4 position; attribute vec2 texCoords; varying vec2 outTexCoords;\n"
        "void main() { outTexCoords = texCoords; gl_Position = position; }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vertex, nullptr);
    glCompileShader(v);
    std::string source = external ? "#extension GL_OES_EGL_image_external : require\n" : "";
    source += fragment;
    const char* s = source.c_str();
    glShaderSource(f, 1, &s, nullptr);
    glCompileShader(f);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "position");
    glBindAttribLocation(p, 1, "texCoords");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "link failed\n"); exit(1); }
    return p;
}

sp<GraphicBuffer> buffer(uint32_t usage, uint8_t fill) {
    sp<GraphicBuffer> b = new GraphicBuffer(W, H, android::PIXEL_FORMAT_RGBA_8888, 1, usage, "pw3-gles-bench");
    if (b->initCheck() != android::NO_ERROR) { fprintf(stderr, "GraphicBuffer failed\n"); exit(1); }
    void* p = nullptr;
    b->lock(GraphicBuffer::USAGE_SW_WRITE_OFTEN, &p);
    for (int y = 0; y < H; ++y) memset(static_cast<uint8_t*>(p) + size_t(y) * b->getStride() * 4, fill, size_t(W) * 4);
    b->unlock();
    return b;
}

EGLImageKHR image(EGLDisplay display, const sp<GraphicBuffer>& b) {
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR i = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                      static_cast<EGLClientBuffer>(b->getNativeBuffer()), attributes);
    if (i == EGL_NO_IMAGE_KHR) { fprintf(stderr, "eglCreateImageKHR failed\n"); exit(1); }
    return i;
}
}  // namespace

int main() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, nullptr, nullptr);
    const EGLint configAttributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    eglChooseConfig(display, configAttributes, &config, 1, &count);
    const EGLint pbufferAttributes[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbufferAttributes);
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    eglMakeCurrent(display, surface, surface, context);
    printf("renderer: %s\n", glGetString(GL_RENDERER));

    const GLfloat quad[] = {-1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glViewport(0, 0, W, H);

    const char* sample2d = "precision mediump float; varying vec2 outTexCoords; uniform sampler2D sampler;\n"
                           "void main() { gl_FragColor = texture2D(sampler, outTexCoords); }\n";
    const char* sampleExternal = "precision mediump float; varying vec2 outTexCoords; uniform samplerExternalOES sampler;\n"
                                 "void main() { gl_FragColor = texture2D(sampler, outTexCoords); }\n";
    GLuint p2d = program(sample2d, false), pExternal = program(sampleExternal, true);

    std::vector<uint8_t> pixels(size_t(W) * H * 4, 200);
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    const uint32_t textureUsage = GraphicBuffer::USAGE_HW_TEXTURE | GraphicBuffer::USAGE_SW_WRITE_OFTEN;
    sp<GraphicBuffer> source = buffer(textureUsage, 120);
    GLuint externalTexture;
    glGenTextures(1, &externalTexture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image(display, source));
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    printf("pbuffer target:\n");
    printf("  clear                          %8.1f ms\n", best([] { glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT); }));
    glUseProgram(p2d);
    glBindTexture(GL_TEXTURE_2D, texture);
    printf("  2D texture, nearest            %8.1f ms\n", best([] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    printf("  2D texture, linear             %8.1f ms\n", best([] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    printf("  2D texture, linear, blended    %8.1f ms\n", best([] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    glDisable(GL_BLEND);
    glUseProgram(pExternal);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
    printf("  external (gralloc) texture     %8.1f ms\n", best([] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    glEnable(GL_BLEND);
    printf("  external texture, blended      %8.1f ms\n", best([] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    glDisable(GL_BLEND);
    std::vector<uint8_t> readBack(size_t(W) * H * 4);
    printf("  glReadPixels                   %8.1f ms\n", best([&] { glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, readBack.data()); }));

    // SurfaceFlinger's screenshot: render into a gralloc buffer through an
    // EGLImage bound to a framebuffer object.
    const uint32_t targetUsage = GraphicBuffer::USAGE_HW_RENDER | GraphicBuffer::USAGE_HW_TEXTURE | GraphicBuffer::USAGE_SW_READ_OFTEN;
    sp<GraphicBuffer> target = buffer(targetUsage, 0);
    GLuint targetTexture, framebuffer;
    glGenTextures(1, &targetTexture);
    glBindTexture(GL_TEXTURE_2D, targetTexture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image(display, target));
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targetTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete\n"); return 1; }
    printf("gralloc target (EGLImage FBO):\n");
    printf("  clear                          %8.1f ms\n", best([] { glClear(GL_COLOR_BUFFER_BIT); }));
    glUseProgram(p2d);
    glBindTexture(GL_TEXTURE_2D, texture);
    printf("  2D texture, linear             %8.1f ms\n", best([] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    glUseProgram(pExternal);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
    printf("  external (gralloc) texture     %8.1f ms\n", best([] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    glEnable(GL_BLEND);
    printf("  external texture, blended      %8.1f ms\n", best([] { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    glDisable(GL_BLEND);
    printf("  three layers + finish          %8.1f ms\n", best([] { for (int i = 0; i < 3; ++i) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }));
    return 0;
}
