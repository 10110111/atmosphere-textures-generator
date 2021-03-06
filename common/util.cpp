#include "util.hpp"
#include <qopengl.h>

std::string openglErrorString(const GLenum error)
{
    switch(error)
    {
    case GL_NO_ERROR: return "No error";
    case GL_INVALID_ENUM: return "Invalid enumerator";
    case GL_INVALID_VALUE: return "Invalid value";
    case GL_INVALID_OPERATION: return "Invalid operation";
    case GL_STACK_OVERFLOW: return "Stack overflow";
    case GL_STACK_UNDERFLOW: return "Stack underflow";
    case GL_OUT_OF_MEMORY: return "Out of memory";
    case GL_TABLE_TOO_LARGE: return "Table too large";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "Invalid framebuffer operation";
    }
    return "Error code " + std::to_string(error);
}

void checkFramebufferStatus(QOpenGLFunctions_3_3_Core& gl, const char*const fboDescription)
{
    const auto status=gl.glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status==GL_FRAMEBUFFER_COMPLETE) return;

    QString errorDescription;
    switch(status)
    {
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
        errorDescription=QObject::tr("incomplete attachment");
        break;
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
        errorDescription=QObject::tr("missing attachment");
        break;
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        errorDescription=QObject::tr("invalid framebuffer operation");
        break;
    case GL_FRAMEBUFFER_UNSUPPORTED:
        errorDescription=QObject::tr("framebuffer unsupported");
        break;
    default:
        errorDescription=QObject::tr("unknown error 0x%1").arg(status, 0, 16);
        break;
    }
    throw OpenGLError{QObject::tr("%1 is incomplete: %2").arg(fboDescription).arg(errorDescription)};
}

void dumpActiveUniforms(QOpenGLFunctions_3_3_Core& gl, const GLuint program)
{
    int uniformCount=0, maxLen=0;
    gl.glGetProgramiv(program,GL_ACTIVE_UNIFORMS,&uniformCount);
    gl.glGetProgramiv(program,GL_ACTIVE_UNIFORM_MAX_LENGTH,&maxLen);
    std::cerr << "Active uniforms:\n";
    for(int uniformIndex=0;uniformIndex<uniformCount;++uniformIndex)
    {
        std::vector<char> name(maxLen);
        GLsizei size;
        GLenum type;
        gl.glGetActiveUniform(program,uniformIndex,maxLen,nullptr,&size,&type,name.data());
        std::cerr << ' ' << name.data() << "\n";
    }
}
