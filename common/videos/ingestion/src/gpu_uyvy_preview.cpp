#include "gpu_uyvy_preview.hpp"

#include <cuda_runtime.h>

#include <array>
#include <iostream>

namespace {
constexpr char kVertexShader[] = R"(#version 330 core
out vec2 uv;
const vec2 positions[3] = vec2[](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
void main() {
    gl_Position = vec4(positions[gl_VertexID], 0, 1);
    uv = (positions[gl_VertexID] + 1.0) * 0.5;
}
)";

constexpr char kFragmentShader[] = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D uyvy;
uniform vec2 resolution;
void main() {
    float x = uv.x * resolution.x;
    float packedX = (floor(x / 2.0) + 0.5) / (resolution.x / 2.0);
    vec4 p = texture(uyvy, vec2(packedX, uv.y));
    float y = (mod(floor(x), 2.0) < 1.0) ? p.g : p.a;
    float cb = p.r - 0.5;
    float cr = p.b - 0.5;
    float yy = max(0.0, (y - 16.0 / 255.0) * (255.0 / 219.0));
    color = vec4(clamp(vec3(yy + 1.5748 * cr,
                            yy - 0.1873 * cb - 0.4681 * cr,
                            yy + 1.8556 * cb), 0.0, 1.0), 1.0);
}
)";

GLuint compile(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE)
        return shader;
    glDeleteShader(shader);
    return 0;
}
} // namespace

GpuUyvyPreview::~GpuUyvyPreview()
{
    destroyResources_();
}

bool GpuUyvyPreview::createProgram_()
{
    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment)
        return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (ok == GL_TRUE)
        return true;
    glDeleteProgram(program_);
    program_ = 0;
    return false;
}

bool GpuUyvyPreview::ensureResources_(int width, int height)
{
    if (width == width_ && height == height_ && pbo_ != 0)
        return true;
    destroyResources_();
    width_ = width;
    height_ = height;
    if (!createProgram_())
        return false;

    glGenTextures(1, &uyvy_texture_);
    glBindTexture(GL_TEXTURE_2D, uyvy_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width / 2, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenTextures(1, &rgb_texture_);
    glBindTexture(GL_TEXTURE_2D, rgb_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rgb_texture_, 0);
    const bool fbo_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // The core profile rejects glDrawArrays unless a vertex array object is
    // bound, even when the vertex shader sources positions from gl_VertexID.
    glGenVertexArrays(1, &vao_);

    glGenBuffers(1, &pbo_);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<size_t>(width) * height * 2, nullptr, GL_STREAM_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (!fbo_ok || cudaGraphicsGLRegisterBuffer(&cuda_pbo_, pbo_, cudaGraphicsMapFlagsWriteDiscard) != cudaSuccess)
    {
        destroyResources_();
        return false;
    }
    return true;
}

bool GpuUyvyPreview::update(const UYVYFrame& frame)
{
    if (!frame.d_data || frame.width <= 0 || frame.height <= 0 || !ensureResources_(frame.width, frame.height))
        return false;
    if (cudaGraphicsMapResources(1, &cuda_pbo_, 0) != cudaSuccess)
        return false;
    void* destination = nullptr;
    size_t bytes = 0;
    const cudaError_t pointer_result = cudaGraphicsResourceGetMappedPointer(&destination, &bytes, cuda_pbo_);
    const size_t row_bytes = static_cast<size_t>(frame.width) * 2;
    const cudaError_t copy_result = pointer_result == cudaSuccess
        ? cudaMemcpy2D(destination, row_bytes, frame.d_data, frame.pitch, row_bytes, frame.height, cudaMemcpyDeviceToDevice)
        : pointer_result;
    cudaGraphicsUnmapResources(1, &cuda_pbo_, 0);
    if (copy_result != cudaSuccess)
        return false;

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_);
    glBindTexture(GL_TEXTURE_2D, uyvy_texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_ / 2, height_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, uyvy_texture_);
    glUniform1i(glGetUniformLocation(program_, "uyvy"), 0);
    glUniform2f(glGetUniformLocation(program_, "resolution"), static_cast<float>(width_), static_cast<float>(height_));
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void GpuUyvyPreview::destroyResources_()
{
    if (cuda_pbo_)
        cudaGraphicsUnregisterResource(cuda_pbo_);
    cuda_pbo_ = nullptr;
    if (pbo_) glDeleteBuffers(1, &pbo_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (uyvy_texture_) glDeleteTextures(1, &uyvy_texture_);
    if (rgb_texture_) glDeleteTextures(1, &rgb_texture_);
    if (program_) glDeleteProgram(program_);
    pbo_ = fbo_ = vao_ = uyvy_texture_ = rgb_texture_ = program_ = 0;
    width_ = height_ = 0;
}
