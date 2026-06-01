#ifndef GL_LOADER_H
#define GL_LOADER_H

#ifdef __WXMSW__
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/glx.h>
#include <GL/gl.h>
#endif
#include <GL/glext.h>

#include <wx/glcanvas.h>

// --- Minimal GL Loader for Shaders ---
typedef GLuint (APIENTRYP PFNGLCREATESHADERPROC) (GLenum type);
typedef void (APIENTRYP PFNGLSHADERSOURCEPROC) (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
typedef void (APIENTRYP PFNGLCOMPILESHADERPROC) (GLuint shader);
typedef GLuint (APIENTRYP PFNGLCREATEPROGRAMPROC) (void);
typedef void (APIENTRYP PFNGLATTACHSHADERPROC) (GLuint program, GLuint shader);
typedef void (APIENTRYP PFNGLLINKPROGRAMPROC) (GLuint program);
typedef void (APIENTRYP PFNGLUSEPROGRAMPROC) (GLuint program);
typedef GLint (APIENTRYP PFNGLGETUNIFORMLOCATIONPROC) (GLuint program, const GLchar* name);
typedef void (APIENTRYP PFNGLUNIFORM1FPROC) (GLint location, GLfloat v0);
typedef void (APIENTRYP PFNGLUNIFORM2FPROC) (GLint location, GLfloat v0, GLfloat v1);
typedef void (APIENTRYP PFNGLUNIFORM1IPROC) (GLint location, GLint v0);
typedef void (APIENTRYP PFNGLUNIFORMMATRIX2FVPROC) (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
typedef void (APIENTRYP PFNGLUNIFORMMATRIX3FVPROC) (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
typedef void (APIENTRYP PFNGLGETSHADERIVPROC) (GLuint shader, GLenum pname, GLint* params);
typedef void (APIENTRYP PFNGLGETSHADERINFOLOGPROC) (GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void (APIENTRYP PFNGLGETPROGRAMIVPROC) (GLuint program, GLenum pname, GLint* params);
typedef void (APIENTRYP PFNGLGETPROGRAMINFOLOGPROC) (GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void (APIENTRYP PFNGLACTIVETEXTUREPROC) (GLenum texture);

extern PFNGLCREATESHADERPROC glCreateShader_ptr;
extern PFNGLSHADERSOURCEPROC glShaderSource_ptr;
extern PFNGLCOMPILESHADERPROC glCompileShader_ptr;
extern PFNGLCREATEPROGRAMPROC glCreateProgram_ptr;
extern PFNGLATTACHSHADERPROC glAttachShader_ptr;
extern PFNGLLINKPROGRAMPROC glLinkProgram_ptr;
extern PFNGLUSEPROGRAMPROC glUseProgram_ptr;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_ptr;
extern PFNGLUNIFORM1FPROC glUniform1f_ptr;
extern PFNGLUNIFORM2FPROC glUniform2f_ptr;
extern PFNGLUNIFORM1IPROC glUniform1i_ptr;
extern PFNGLUNIFORMMATRIX2FVPROC glUniformMatrix2fv_ptr;
extern PFNGLUNIFORMMATRIX3FVPROC glUniformMatrix3fv_ptr;
extern PFNGLGETSHADERIVPROC glGetShaderiv_ptr;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog_ptr;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv_ptr;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_ptr;
extern PFNGLACTIVETEXTUREPROC glActiveTexture_ptr;

#define glCreateShader glCreateShader_ptr
#define glShaderSource glShaderSource_ptr
#define glCompileShader glCompileShader_ptr
#define glCreateProgram glCreateProgram_ptr
#define glAttachShader glAttachShader_ptr
#define glLinkProgram glLinkProgram_ptr
#define glUseProgram glUseProgram_ptr
#define glGetUniformLocation glGetUniformLocation_ptr
#define glUniform1f glUniform1f_ptr
#define glUniform2f glUniform2f_ptr
#define glUniform1i glUniform1i_ptr
#define glUniformMatrix2fv glUniformMatrix2fv_ptr
#define glUniformMatrix3fv glUniformMatrix3fv_ptr
#define glGetShaderiv glGetShaderiv_ptr
#define glGetShaderInfoLog glGetShaderInfoLog_ptr
#define glGetProgramiv glGetProgramiv_ptr
#define glGetProgramInfoLog glGetProgramInfoLog_ptr
#define glActiveTexture glActiveTexture_ptr

void InitGLLoader();

#endif // GL_LOADER_H
