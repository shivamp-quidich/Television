#pragma once

#include <GL/glew.h>
#include <cuda_gl_interop.h>

#include "shared_data.h"

class GpuUyvyPreview {
public:
    ~GpuUyvyPreview();

    bool update(const UYVYFrame& frame);
    GLuint rgbTexture() const { return rgb_texture_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool ensureResources_(int width, int height);
    bool createProgram_();
    void destroyResources_();

    int width_ = 0;
    int height_ = 0;
    GLuint uyvy_texture_ = 0;
    GLuint rgb_texture_ = 0;
    GLuint pbo_ = 0;
    GLuint fbo_ = 0;
    GLuint vao_ = 0;
    GLuint program_ = 0;
    cudaGraphicsResource_t cuda_pbo_ = nullptr;
};
