#include "ShaderProgram.h"
#include <fstream>
#include <sstream>
#include <cstdio>

ShaderProgram::~ShaderProgram() {
    if (m_program) { GL_CHECK(glDeleteProgram(m_program)); }
}

void ShaderProgram::bind()   const { GL_CHECK(glUseProgram(m_program)); }
void ShaderProgram::unbind() const { GL_CHECK(glUseProgram(0)); }

std::string ShaderProgram::readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "ShaderProgram: cannot open '%s'\n", path.c_str()); return {}; }
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

GLuint ShaderProgram::compile(GLenum type, const std::string& src, const std::string& tag) {
    GLuint s = glCreateShader(type);
    const char* cstr = src.c_str();
    GL_CHECK(glShaderSource(s, 1, &cstr, nullptr));
    GL_CHECK(glCompileShader(s));

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len), '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        fprintf(stderr, "── Shader compile error [%s] ──\n%s\n", tag.c_str(), log.c_str());
        glDeleteShader(s); return 0;
    }
    return s;
}

bool ShaderProgram::link(GLuint vert, GLuint frag) {
    GLuint prog = glCreateProgram();
    GL_CHECK(glAttachShader(prog, vert));
    GL_CHECK(glAttachShader(prog, frag));
    GL_CHECK(glLinkProgram(prog));

    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        fprintf(stderr, "── Shader link error ──\n%s\n", log.c_str());
        glDeleteProgram(prog); glDeleteShader(vert); glDeleteShader(frag);
        return false;
    }

    glDetachShader(prog, vert); glDeleteShader(vert);
    glDetachShader(prog, frag); glDeleteShader(frag);

    if (m_program) glDeleteProgram(m_program);
    m_program = prog;
    m_locCache.clear();
    return true;
}

bool ShaderProgram::load(const std::string& vertPath, const std::string& fragPath) {
    m_vertPath = vertPath; m_fragPath = fragPath;

    std::string vs = readFile(vertPath);
    std::string fs = readFile(fragPath);
    if (vs.empty() || fs.empty()) return false;

    GLuint vert = compile(GL_VERTEX_SHADER,   vs, vertPath);
    GLuint frag = compile(GL_FRAGMENT_SHADER, fs, fragPath);
    if (!vert || !frag) { if(vert) glDeleteShader(vert); if(frag) glDeleteShader(frag); return false; }

    if (!link(vert, frag)) return false;

    m_vertMtime = std::filesystem::last_write_time(vertPath);
    m_fragMtime = std::filesystem::last_write_time(fragPath);
    fprintf(stdout, "Shader loaded: %s\n", fragPath.c_str());
    return true;
}

bool ShaderProgram::checkReload() {
    if (m_vertPath.empty()) return false;
    try {
        auto vt = std::filesystem::last_write_time(m_vertPath);
        auto ft = std::filesystem::last_write_time(m_fragPath);
        if (vt != m_vertMtime || ft != m_fragMtime) {
            fprintf(stdout, "Hot-reload: %s\n", m_fragPath.c_str());
            return load(m_vertPath, m_fragPath);
        }
    } catch (...) {}
    return false;
}

GLint ShaderProgram::loc(const char* name) const {
    auto it = m_locCache.find(name);
    if (it != m_locCache.end()) return it->second;
    GLint l = glGetUniformLocation(m_program, name);
    m_locCache[name] = l;
    return l;
}

void ShaderProgram::set(const char* n, float v)             const { if(loc(n)>=0) GL_CHECK(glUniform1f(loc(n),v)); }
void ShaderProgram::set(const char* n, int v)               const { if(loc(n)>=0) GL_CHECK(glUniform1i(loc(n),v)); }
void ShaderProgram::set(const char* n, bool v)              const { if(loc(n)>=0) GL_CHECK(glUniform1i(loc(n),(int)v)); }
void ShaderProgram::set(const char* n, const glm::vec2& v)  const { if(loc(n)>=0) GL_CHECK(glUniform2fv(loc(n),1,&v[0])); }
void ShaderProgram::set(const char* n, const glm::vec3& v)  const { if(loc(n)>=0) GL_CHECK(glUniform3fv(loc(n),1,&v[0])); }
void ShaderProgram::set(const char* n, const glm::vec4& v)  const { if(loc(n)>=0) GL_CHECK(glUniform4fv(loc(n),1,&v[0])); }
void ShaderProgram::set(const char* n, const glm::mat4& v)  const { if(loc(n)>=0) GL_CHECK(glUniformMatrix4fv(loc(n),1,GL_FALSE,&v[0][0])); }
