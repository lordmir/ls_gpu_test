#include "GLLoader.h"

PFNGLCREATESHADERPROC glCreateShader_ptr;
PFNGLSHADERSOURCEPROC glShaderSource_ptr;
PFNGLCOMPILESHADERPROC glCompileShader_ptr;
PFNGLCREATEPROGRAMPROC glCreateProgram_ptr;
PFNGLATTACHSHADERPROC glAttachShader_ptr;
PFNGLLINKPROGRAMPROC glLinkProgram_ptr;
PFNGLUSEPROGRAMPROC glUseProgram_ptr;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_ptr;
PFNGLUNIFORM1FPROC glUniform1f_ptr;
PFNGLUNIFORM2FPROC glUniform2f_ptr;
PFNGLUNIFORM1IPROC glUniform1i_ptr;
PFNGLUNIFORMMATRIX2FVPROC glUniformMatrix2fv_ptr;
PFNGLUNIFORMMATRIX3FVPROC glUniformMatrix3fv_ptr;
PFNGLGETSHADERIVPROC glGetShaderiv_ptr;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog_ptr;
PFNGLGETPROGRAMIVPROC glGetProgramiv_ptr;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_ptr;
PFNGLACTIVETEXTUREPROC glActiveTexture_ptr;

void InitGLLoader() {
#ifdef __WXMSW__
    auto get_proc = [](const char* name) { return (void*)wglGetProcAddress(name); };
#else
    auto get_proc = [](const char* name) { 
        void* p = (void*)glXGetProcAddress((const GLubyte*)name);
        if (!p) p = (void*)glXGetProcAddressARB((const GLubyte*)name);
        return p;
    };
#endif

    glCreateShader_ptr = (PFNGLCREATESHADERPROC)get_proc("glCreateShader");
    glShaderSource_ptr = (PFNGLSHADERSOURCEPROC)get_proc("glShaderSource");
    glCompileShader_ptr = (PFNGLCOMPILESHADERPROC)get_proc("glCompileShader");
    glCreateProgram_ptr = (PFNGLCREATEPROGRAMPROC)get_proc("glCreateProgram");
    glAttachShader_ptr = (PFNGLATTACHSHADERPROC)get_proc("glAttachShader");
    glLinkProgram_ptr = (PFNGLLINKPROGRAMPROC)get_proc("glLinkProgram");
    glUseProgram_ptr = (PFNGLUSEPROGRAMPROC)get_proc("glUseProgram");
    glGetUniformLocation_ptr = (PFNGLGETUNIFORMLOCATIONPROC)get_proc("glGetUniformLocation");
    glUniform1f_ptr = (PFNGLUNIFORM1FPROC)get_proc("glUniform1f");
    glUniform2f_ptr = (PFNGLUNIFORM2FPROC)get_proc("glUniform2f");
    glUniform1i_ptr = (PFNGLUNIFORM1IPROC)get_proc("glUniform1i");
    glUniformMatrix2fv_ptr = (PFNGLUNIFORMMATRIX2FVPROC)get_proc("glUniformMatrix2fv");
    glUniformMatrix3fv_ptr = (PFNGLUNIFORMMATRIX3FVPROC)get_proc("glUniformMatrix3fv");
    glGetShaderiv_ptr = (PFNGLGETSHADERIVPROC)get_proc("glGetShaderiv");
    glGetShaderInfoLog_ptr = (PFNGLGETSHADERINFOLOGPROC)get_proc("glGetShaderInfoLog");
    glGetProgramiv_ptr = (PFNGLGETPROGRAMIVPROC)get_proc("glGetProgramiv");
    glGetProgramInfoLog_ptr = (PFNGLGETPROGRAMINFOLOGPROC)get_proc("glGetProgramInfoLog");
    glActiveTexture_ptr = (PFNGLACTIVETEXTUREPROC)get_proc("glActiveTexture");
}
