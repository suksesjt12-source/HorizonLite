#pragma once

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace horizon {

struct OpenGLVertex {
    float x{}, y{}, z{};
    float r{1.0F}, g{1.0F}, b{1.0F}, a{1.0F};
    float u{}, v{};
};

class OpenGLRenderer {
public:
    OpenGLRenderer(SDL_Window* window, int width, int height);
    ~OpenGLRenderer();

    OpenGLRenderer(const OpenGLRenderer&) = delete;
    OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool has_pending_gpu_frame() const noexcept;
    void begin_frame();
    void set_viewport(int x, int y, int width, int height);
    void set_scissor(int x, int y, int width, int height, bool enabled);
    void set_depth_state(bool enabled, bool write_enabled, GLenum function);
    void set_raster_state(bool cull_enabled, GLenum front_face,
                          GLenum cull_face, bool blend_enabled);
    void clear(float r, float g, float b, float a, double depth = 1.0);
    void upload_texture(std::uint64_t key, std::uint32_t width,
                        std::uint32_t height,
                        std::span<const std::uint8_t> rgba,
                        bool linear_filter);
    void draw_arrays(std::span<const OpenGLVertex> vertices, GLenum primitive,
                     std::uint64_t texture_key = 0U);
    void draw_elements(std::span<const OpenGLVertex> vertices,
                       std::span<const std::uint32_t> indices,
                       GLenum primitive, std::uint64_t texture_key = 0U);
    void upload_rgba(std::span<const std::uint8_t> rgba,
                     int width, int height);
    void present();

private:
#define HORIZON_GL_PROC(ret, name, args) using name##Proc = ret (APIENTRY*) args; name##Proc name##_{};
    HORIZON_GL_PROC(void, GenVertexArrays, (GLsizei, GLuint*))
    HORIZON_GL_PROC(void, DeleteVertexArrays, (GLsizei, const GLuint*))
    HORIZON_GL_PROC(void, BindVertexArray, (GLuint))
    HORIZON_GL_PROC(void, GenBuffers, (GLsizei, GLuint*))
    HORIZON_GL_PROC(void, DeleteBuffers, (GLsizei, const GLuint*))
    HORIZON_GL_PROC(void, BindBuffer, (GLenum, GLuint))
    HORIZON_GL_PROC(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))
    HORIZON_GL_PROC(void, EnableVertexAttribArray, (GLuint))
    HORIZON_GL_PROC(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
    HORIZON_GL_PROC(GLuint, CreateShader, (GLenum))
    HORIZON_GL_PROC(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
    HORIZON_GL_PROC(void, CompileShader, (GLuint))
    HORIZON_GL_PROC(void, GetShaderiv, (GLuint, GLenum, GLint*))
    HORIZON_GL_PROC(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
    HORIZON_GL_PROC(void, DeleteShader, (GLuint))
    HORIZON_GL_PROC(GLuint, CreateProgram, ())
    HORIZON_GL_PROC(void, AttachShader, (GLuint, GLuint))
    HORIZON_GL_PROC(void, LinkProgram, (GLuint))
    HORIZON_GL_PROC(void, GetProgramiv, (GLuint, GLenum, GLint*))
    HORIZON_GL_PROC(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
    HORIZON_GL_PROC(void, DeleteProgram, (GLuint))
    HORIZON_GL_PROC(void, UseProgram, (GLuint))
    HORIZON_GL_PROC(GLint, GetUniformLocation, (GLuint, const GLchar*))
    HORIZON_GL_PROC(void, Uniform1i, (GLint, GLint))
    HORIZON_GL_PROC(void, Uniform1f, (GLint, GLfloat))
    HORIZON_GL_PROC(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))
    HORIZON_GL_PROC(void, ActiveTexture, (GLenum))
    HORIZON_GL_PROC(void, GenTextures, (GLsizei, GLuint*))
    HORIZON_GL_PROC(void, DeleteTextures, (GLsizei, const GLuint*))
    HORIZON_GL_PROC(void, BindTexture, (GLenum, GLuint))
    HORIZON_GL_PROC(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))
    HORIZON_GL_PROC(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))
    HORIZON_GL_PROC(void, TexParameteri, (GLenum, GLenum, GLint))
    HORIZON_GL_PROC(void, GenFramebuffers, (GLsizei, GLuint*))
    HORIZON_GL_PROC(void, DeleteFramebuffers, (GLsizei, const GLuint*))
    HORIZON_GL_PROC(void, BindFramebuffer, (GLenum, GLuint))
    HORIZON_GL_PROC(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
    HORIZON_GL_PROC(GLenum, CheckFramebufferStatus, (GLenum))
    HORIZON_GL_PROC(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
    HORIZON_GL_PROC(void, GenRenderbuffers, (GLsizei, GLuint*))
    HORIZON_GL_PROC(void, DeleteRenderbuffers, (GLsizei, const GLuint*))
    HORIZON_GL_PROC(void, BindRenderbuffer, (GLenum, GLuint))
    HORIZON_GL_PROC(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
    HORIZON_GL_PROC(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
    HORIZON_GL_PROC(void, DrawArrays, (GLenum, GLint, GLsizei))
    HORIZON_GL_PROC(void, DrawElements, (GLenum, GLsizei, GLenum, const void*))
    HORIZON_GL_PROC(void, Viewport, (GLint, GLint, GLsizei, GLsizei))
    HORIZON_GL_PROC(void, Scissor, (GLint, GLint, GLsizei, GLsizei))
    HORIZON_GL_PROC(void, Enable, (GLenum))
    HORIZON_GL_PROC(void, Disable, (GLenum))
    HORIZON_GL_PROC(void, DepthFunc, (GLenum))
    HORIZON_GL_PROC(void, DepthMask, (GLboolean))
    HORIZON_GL_PROC(void, FrontFace, (GLenum))
    HORIZON_GL_PROC(void, CullFace, (GLenum))
    HORIZON_GL_PROC(void, BlendFunc, (GLenum, GLenum))
    HORIZON_GL_PROC(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
    HORIZON_GL_PROC(void, ClearDepth, (GLdouble))
    HORIZON_GL_PROC(void, ClearDepthf, (GLfloat))
    HORIZON_GL_PROC(void, Clear, (GLbitfield))
#undef HORIZON_GL_PROC

    void load_functions();
    void create_program();
    void create_framebuffer();
    void configure_vertex_layout();
    void upload_vertices(std::span<const OpenGLVertex> vertices);
    [[nodiscard]] GLuint compile_shader(GLenum type, const char* source);
    void require(bool condition, const char* message) const;

    SDL_Window* window_{};
    GLuint vao_{}, vertex_buffer_{}, index_buffer_{};
    GLuint program_{}, framebuffer_{}, color_texture_{}, depth_buffer_{};
    GLint sampler_uniform_{-1}, texture_enabled_uniform_{-1}, mvp_uniform_{-1};
    int width_{}, height_{};
    bool pending_gpu_frame_{};
    bool is_gles_{};
    std::unordered_map<std::uint64_t, GLuint> texture_cache_;
};

} // namespace horizon
