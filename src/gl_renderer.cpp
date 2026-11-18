#include "horizon/gl_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace horizon {
namespace {
constexpr const char* kVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUv;
uniform mat4 uMvp;
out vec4 vColor;
out vec2 vUv;
void main() {
    gl_Position = uMvp * vec4(aPosition, 1.0);
    vColor = aColor;
    vUv = aUv;
}
)GLSL";

constexpr const char* kFragmentShader = R"GLSL(#version 330 core
in vec4 vColor;
in vec2 vUv;
uniform sampler2D uTexture;
uniform int uTextureEnabled;
out vec4 fragmentColor;
void main() {
    vec4 sampled = (uTextureEnabled != 0) ? texture(uTexture, vUv) : vec4(1.0);
    fragmentColor = vColor * sampled;
}
)GLSL";
}

OpenGLRenderer::OpenGLRenderer(SDL_Window* window, int width, int height)
    : window_(window), width_(width), height_(height) {
    if (window_ == nullptr || width_ <= 0 || height_ <= 0)
        throw std::invalid_argument("OpenGL renderer requires a valid SDL window and dimensions");
    if (SDL_GL_GetCurrentContext() == nullptr)
        throw std::runtime_error("OpenGL renderer requires a current SDL_GLContext");
    load_functions();
    create_program();
    create_framebuffer();

    GenVertexArrays_(1, &vao_);
    GenBuffers_(1, &vertex_buffer_);
    GenBuffers_(1, &index_buffer_);
    require(vao_ != 0U && vertex_buffer_ != 0U && index_buffer_ != 0U,
            "OpenGL object allocation failed");
    BindVertexArray_(vao_);
    BindBuffer_(GL_ARRAY_BUFFER, vertex_buffer_);
    BindBuffer_(GL_ELEMENT_ARRAY_BUFFER, index_buffer_);
    configure_vertex_layout();
    BindVertexArray_(0U);
    BindFramebuffer_(GL_FRAMEBUFFER, framebuffer_);
    Viewport_(0, 0, width_, height_);
    Disable_(GL_BLEND);
    Enable_(GL_DEPTH_TEST);
    DepthFunc_(GL_LESS);
    DepthMask_(GL_TRUE);
}

OpenGLRenderer::~OpenGLRenderer() {
    if (SDL_GL_GetCurrentContext() == nullptr) return;
    for (const auto& item : texture_cache_) {
        const GLuint texture = item.second;
        if (texture != 0U && DeleteTextures_ != nullptr)
            DeleteTextures_(1, &texture);
    }
    if (depth_buffer_ != 0U && DeleteRenderbuffers_ != nullptr)
        DeleteRenderbuffers_(1, &depth_buffer_);
    if (framebuffer_ != 0U && DeleteFramebuffers_ != nullptr)
        DeleteFramebuffers_(1, &framebuffer_);
    if (color_texture_ != 0U && DeleteTextures_ != nullptr)
        DeleteTextures_(1, &color_texture_);
    if (index_buffer_ != 0U && DeleteBuffers_ != nullptr)
        DeleteBuffers_(1, &index_buffer_);
    if (vertex_buffer_ != 0U && DeleteBuffers_ != nullptr)
        DeleteBuffers_(1, &vertex_buffer_);
    if (vao_ != 0U && DeleteVertexArrays_ != nullptr)
        DeleteVertexArrays_(1, &vao_);
    if (program_ != 0U && DeleteProgram_ != nullptr)
        DeleteProgram_(program_);
}

bool OpenGLRenderer::available() const noexcept {
    return program_ != 0U && framebuffer_ != 0U && vao_ != 0U;
}

bool OpenGLRenderer::has_pending_gpu_frame() const noexcept {
    return pending_gpu_frame_;
}

void OpenGLRenderer::require(bool condition, const char* message) const {
    if (!condition) throw std::runtime_error(message);
}

void OpenGLRenderer::load_functions() {
#define LOAD_GL(name) name##_ = reinterpret_cast<name##Proc>(SDL_GL_GetProcAddress("gl" #name)); require(name##_ != nullptr, "Missing OpenGL entry point: gl" #name)
    LOAD_GL(GenVertexArrays);
    LOAD_GL(DeleteVertexArrays);
    LOAD_GL(BindVertexArray);
    LOAD_GL(GenBuffers);
    LOAD_GL(DeleteBuffers);
    LOAD_GL(BindBuffer);
    LOAD_GL(BufferData);
    LOAD_GL(EnableVertexAttribArray);
    LOAD_GL(VertexAttribPointer);
    LOAD_GL(CreateShader);
    LOAD_GL(ShaderSource);
    LOAD_GL(CompileShader);
    LOAD_GL(GetShaderiv);
    LOAD_GL(GetShaderInfoLog);
    LOAD_GL(DeleteShader);
    LOAD_GL(CreateProgram);
    LOAD_GL(AttachShader);
    LOAD_GL(LinkProgram);
    LOAD_GL(GetProgramiv);
    LOAD_GL(GetProgramInfoLog);
    LOAD_GL(DeleteProgram);
    LOAD_GL(UseProgram);
    LOAD_GL(GetUniformLocation);
    LOAD_GL(Uniform1i);
    LOAD_GL(Uniform1f);
    LOAD_GL(UniformMatrix4fv);
    LOAD_GL(ActiveTexture);
    LOAD_GL(GenTextures);
    LOAD_GL(DeleteTextures);
    LOAD_GL(BindTexture);
    LOAD_GL(TexImage2D);
    LOAD_GL(TexSubImage2D);
    LOAD_GL(TexParameteri);
    LOAD_GL(GenFramebuffers);
    LOAD_GL(DeleteFramebuffers);
    LOAD_GL(BindFramebuffer);
    LOAD_GL(FramebufferTexture2D);
    LOAD_GL(CheckFramebufferStatus);
    LOAD_GL(BlitFramebuffer);
    LOAD_GL(GenRenderbuffers);
    LOAD_GL(DeleteRenderbuffers);
    LOAD_GL(BindRenderbuffer);
    LOAD_GL(RenderbufferStorage);
    LOAD_GL(FramebufferRenderbuffer);
    LOAD_GL(DrawArrays);
    LOAD_GL(DrawElements);
    LOAD_GL(Viewport);
    LOAD_GL(Scissor);
    LOAD_GL(Enable);
    LOAD_GL(Disable);
    LOAD_GL(DepthFunc);
    LOAD_GL(DepthMask);
    LOAD_GL(FrontFace);
    LOAD_GL(CullFace);
    LOAD_GL(BlendFunc);
    LOAD_GL(ClearColor);
    LOAD_GL(ClearDepth);
    LOAD_GL(Clear);
#undef LOAD_GL
}

GLuint OpenGLRenderer::compile_shader(GLenum type, const char* source) {
    const GLuint shader = CreateShader_(type);
    require(shader != 0U, "glCreateShader failed");
    const GLchar* source_pointer = source;
    ShaderSource_(shader, 1, &source_pointer, nullptr);
    CompileShader_(shader);
    GLint compiled = GL_FALSE;
    GetShaderiv_(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        GLint length = 0;
        GetShaderiv_(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
        GLsizei written = 0;
        GetShaderInfoLog_(shader, static_cast<GLsizei>(log.size()), &written, log.data());
        DeleteShader_(shader);
        throw std::runtime_error("GLSL compilation failed: " + log);
    }
    return shader;
}

void OpenGLRenderer::create_program() {
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = 0U;
    try {
        fragment = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
        program_ = CreateProgram_();
        require(program_ != 0U, "glCreateProgram failed");
        AttachShader_(program_, vertex);
        AttachShader_(program_, fragment);
        LinkProgram_(program_);
        GLint linked = GL_FALSE;
        GetProgramiv_(program_, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint length = 0;
            GetProgramiv_(program_, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            GLsizei written = 0;
            GetProgramInfoLog_(program_, static_cast<GLsizei>(log.size()), &written, log.data());
            throw std::runtime_error("GLSL program link failed: " + log);
        }
        sampler_uniform_ = GetUniformLocation_(program_, "uTexture");
        texture_enabled_uniform_ = GetUniformLocation_(program_, "uTextureEnabled");
        mvp_uniform_ = GetUniformLocation_(program_, "uMvp");
        UseProgram_(program_);
        if (sampler_uniform_ >= 0) Uniform1i_(sampler_uniform_, 0);
        DeleteShader_(vertex);
        DeleteShader_(fragment);
    } catch (...) {
        DeleteShader_(vertex);
        if (fragment != 0U) DeleteShader_(fragment);
        if (program_ != 0U) {
            DeleteProgram_(program_);
            program_ = 0U;
        }
        throw;
    }
}

void OpenGLRenderer::create_framebuffer() {
    GenFramebuffers_(1, &framebuffer_);
    GenTextures_(1, &color_texture_);
    GenRenderbuffers_(1, &depth_buffer_);
    require(framebuffer_ != 0U && color_texture_ != 0U && depth_buffer_ != 0U,
            "OpenGL framebuffer resource allocation failed");

    BindTexture_(GL_TEXTURE_2D, color_texture_);
    TexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    TexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    TexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    TexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    TexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    BindFramebuffer_(GL_FRAMEBUFFER, framebuffer_);
    FramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, color_texture_, 0);
    BindRenderbuffer_(GL_RENDERBUFFER, depth_buffer_);
    RenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width_, height_);
    FramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             GL_RENDERBUFFER, depth_buffer_);
    require(CheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
            "OpenGL off-screen framebuffer is incomplete");
    BindFramebuffer_(GL_FRAMEBUFFER, 0U);
}

void OpenGLRenderer::configure_vertex_layout() {
    const GLsizei stride = static_cast<GLsizei>(sizeof(OpenGLVertex));
    EnableVertexAttribArray_(0U);
    VertexAttribPointer_(0U, 3, GL_FLOAT, GL_FALSE, stride,
                         reinterpret_cast<const void*>(offsetof(OpenGLVertex, x)));
    EnableVertexAttribArray_(1U);
    VertexAttribPointer_(1U, 4, GL_FLOAT, GL_FALSE, stride,
                         reinterpret_cast<const void*>(offsetof(OpenGLVertex, r)));
    EnableVertexAttribArray_(2U);
    VertexAttribPointer_(2U, 2, GL_FLOAT, GL_FALSE, stride,
                         reinterpret_cast<const void*>(offsetof(OpenGLVertex, u)));
}

void OpenGLRenderer::begin_frame() {
    BindFramebuffer_(GL_FRAMEBUFFER, framebuffer_);
}

void OpenGLRenderer::set_viewport(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return;
    Viewport_(x, y, width, height);
}

void OpenGLRenderer::set_scissor(int x, int y, int width, int height, bool enabled) {
    if (enabled) {
        Enable_(GL_SCISSOR_TEST);
        Scissor_(x, y, width, height);
    } else {
        Disable_(GL_SCISSOR_TEST);
    }
}

void OpenGLRenderer::set_depth_state(bool enabled, bool write_enabled, GLenum function) {
    if (enabled) Enable_(GL_DEPTH_TEST);
    else Disable_(GL_DEPTH_TEST);
    DepthMask_(write_enabled ? GL_TRUE : GL_FALSE);
    DepthFunc_(function);
}

void OpenGLRenderer::set_raster_state(bool cull_enabled, GLenum front_face,
                                      GLenum cull_face, bool blend_enabled) {
    if (cull_enabled) {
        Enable_(GL_CULL_FACE);
        FrontFace_(front_face);
        CullFace_(cull_face);
    } else {
        Disable_(GL_CULL_FACE);
    }
    if (blend_enabled) {
        Enable_(GL_BLEND);
        BlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        Disable_(GL_BLEND);
    }
}

void OpenGLRenderer::clear(float r, float g, float b, float a, double depth) {
    begin_frame();
    ClearColor_(r, g, b, a);
    ClearDepth_(depth);
    Clear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    pending_gpu_frame_ = true;
}

void OpenGLRenderer::upload_texture(std::uint64_t key, std::uint32_t width,
                                    std::uint32_t height,
                                    std::span<const std::uint8_t> rgba,
                                    bool linear_filter) {
    if (key == 0U || width == 0U || height == 0U)
        throw std::invalid_argument("invalid texture cache key or dimensions");
    const std::uint64_t required = static_cast<std::uint64_t>(width) * height * 4U;
    if (required > rgba.size())
        throw std::invalid_argument("texture upload buffer is truncated");
    GLuint texture = 0U;
    const auto found = texture_cache_.find(key);
    if (found == texture_cache_.end()) {
        GenTextures_(1, &texture);
        require(texture != 0U, "glGenTextures failed for guest texture");
        texture_cache_.emplace(key, texture);
        BindTexture_(GL_TEXTURE_2D, texture);
        TexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        TexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        TexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                       linear_filter ? GL_LINEAR : GL_NEAREST);
        TexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                       linear_filter ? GL_LINEAR : GL_NEAREST);
        TexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8,
                    static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else {
        texture = found->second;
    }
}

void OpenGLRenderer::upload_vertices(std::span<const OpenGLVertex> vertices) {
    require(!vertices.empty(), "draw call has no vertices");
    BindVertexArray_(vao_);
    BindBuffer_(GL_ARRAY_BUFFER, vertex_buffer_);
    const std::size_t bytes = vertices.size_bytes();
    BufferData_(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes),
                vertices.data(), GL_STREAM_DRAW);
    configure_vertex_layout();
}

void OpenGLRenderer::draw_arrays(std::span<const OpenGLVertex> vertices,
                                 GLenum primitive, std::uint64_t texture_key) {
    if (vertices.empty()) return;
    begin_frame();
    UseProgram_(program_);
    const std::array<GLfloat, 16> identity{
        1.0F,0.0F,0.0F,0.0F, 0.0F,1.0F,0.0F,0.0F,
        0.0F,0.0F,1.0F,0.0F, 0.0F,0.0F,0.0F,1.0F
    };
    if (mvp_uniform_ >= 0) UniformMatrix4fv_(mvp_uniform_, 1, GL_FALSE, identity.data());
    const bool textured = texture_key != 0U && texture_cache_.contains(texture_key);
    if (texture_enabled_uniform_ >= 0)
        Uniform1i_(texture_enabled_uniform_, textured ? 1 : 0);
    if (textured) {
        ActiveTexture_(GL_TEXTURE0);
        BindTexture_(GL_TEXTURE_2D, texture_cache_.at(texture_key));
    }
    if (sampler_uniform_ >= 0) Uniform1i_(sampler_uniform_, 0);
    upload_vertices(vertices);
    DrawArrays_(primitive, 0, static_cast<GLsizei>(vertices.size()));
    BindVertexArray_(0U);
    pending_gpu_frame_ = true;
}

void OpenGLRenderer::draw_elements(std::span<const OpenGLVertex> vertices,
                                   std::span<const std::uint32_t> indices,
                                   GLenum primitive, std::uint64_t texture_key) {
    if (vertices.empty() || indices.empty()) return;
    begin_frame();
    UseProgram_(program_);
    const std::array<GLfloat, 16> identity{
        1.0F,0.0F,0.0F,0.0F, 0.0F,1.0F,0.0F,0.0F,
        0.0F,0.0F,1.0F,0.0F, 0.0F,0.0F,0.0F,1.0F
    };
    if (mvp_uniform_ >= 0) UniformMatrix4fv_(mvp_uniform_, 1, GL_FALSE, identity.data());
    const bool textured = texture_key != 0U && texture_cache_.contains(texture_key);
    if (texture_enabled_uniform_ >= 0)
        Uniform1i_(texture_enabled_uniform_, textured ? 1 : 0);
    if (textured) {
        ActiveTexture_(GL_TEXTURE0);
        BindTexture_(GL_TEXTURE_2D, texture_cache_.at(texture_key));
    }
    if (sampler_uniform_ >= 0) Uniform1i_(sampler_uniform_, 0);
    upload_vertices(vertices);
    BindBuffer_(GL_ELEMENT_ARRAY_BUFFER, index_buffer_);
    BufferData_(GL_ELEMENT_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(indices.size_bytes()),
                indices.data(), GL_STREAM_DRAW);
    DrawElements_(primitive, static_cast<GLsizei>(indices.size()),
                  GL_UNSIGNED_INT, nullptr);
    BindVertexArray_(0U);
    pending_gpu_frame_ = true;
}

void OpenGLRenderer::upload_rgba(std::span<const std::uint8_t> rgba,
                                 int width, int height) {
    if (width != width_ || height != height_ ||
        rgba.size() < static_cast<std::size_t>(width_) *
                      static_cast<std::size_t>(height_) * 4U)
        throw std::invalid_argument("CPU framebuffer dimensions do not match OpenGL surface");
    BindTexture_(GL_TEXTURE_2D, color_texture_);
    TexSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, width_, height_,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    pending_gpu_frame_ = false;
}

void OpenGLRenderer::present() {
    BindFramebuffer_(GL_READ_FRAMEBUFFER, framebuffer_);
    BindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0U);
    BlitFramebuffer_(0, 0, width_, height_, 0, 0, width_, height_,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    SDL_GL_SwapWindow(window_);
    BindFramebuffer_(GL_FRAMEBUFFER, framebuffer_);
    pending_gpu_frame_ = false;
}

} // namespace horizon
