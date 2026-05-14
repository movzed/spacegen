#pragma once
#include "gl.h"
#include <glm/glm.hpp>
#include <filesystem>
#include <string>
#include <unordered_map>

class ShaderProgram {
public:
    ShaderProgram()  = default;
    ~ShaderProgram();

    // Load vertex + fragment from file paths. Returns true on success.
    bool load(const std::string& vertPath, const std::string& fragPath);

    // Call once per frame — recompiles if either source file changed on disk.
    bool checkReload();

    void bind()   const;
    void unbind() const;

    bool   valid()  const { return m_program != 0; }
    GLuint handle() const { return m_program; }

    // Uniform setters — cached location lookup, no-op if name not found.
    void set(const char* name, float v)              const;
    void set(const char* name, int v)                const;
    void set(const char* name, bool v)               const;
    void set(const char* name, const glm::vec2& v)   const;
    void set(const char* name, const glm::vec3& v)   const;
    void set(const char* name, const glm::vec4& v)   const;
    void set(const char* name, const glm::mat4& v)   const;

private:
    GLuint      compile(GLenum type, const std::string& src, const std::string& tag);
    bool        link(GLuint vert, GLuint frag);
    std::string readFile(const std::string& path);
    GLint       loc(const char* name) const;

    GLuint      m_program  = 0;
    std::string m_vertPath, m_fragPath;
    std::filesystem::file_time_type m_vertMtime{}, m_fragMtime{};
    mutable std::unordered_map<std::string, GLint> m_locCache;
};
