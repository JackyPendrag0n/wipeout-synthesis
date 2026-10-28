// Minimal OpenGL 3.3 core function table, loaded at runtime through a get-proc-address callback
// (the PS2Recomp runner gets one from raylib's bundled GLFW).
#pragma once

// On Windows glcorearb.h pulls in <windows.h> for APIENTRY, which clashes with raylib.h
// (Rectangle, CloseWindow, ShowCursor). The calling convention is all it needs.
#if defined(_WIN32) && !defined(APIENTRY)
#define APIENTRY __stdcall
#endif
#include <GL/glcorearb.h>

#define RT_GL_FUNCS(X) \
    X(PFNGLGETERRORPROC, GetError) \
    X(PFNGLGETINTEGERVPROC, GetIntegerv) \
    X(PFNGLGETSTRINGPROC, GetString) \
    X(PFNGLENABLEPROC, Enable) \
    X(PFNGLDISABLEPROC, Disable) \
    X(PFNGLVIEWPORTPROC, Viewport) \
    X(PFNGLSCISSORPROC, Scissor) \
    X(PFNGLCLEARCOLORPROC, ClearColor) \
    X(PFNGLCLEARDEPTHPROC, ClearDepth) \
    X(PFNGLCLEARPROC, Clear) \
    X(PFNGLDEPTHFUNCPROC, DepthFunc) \
    X(PFNGLDEPTHMASKPROC, DepthMask) \
    X(PFNGLCOLORMASKPROC, ColorMask) \
    X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate) \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, BlendEquationSeparate) \
    X(PFNGLBLENDCOLORPROC, BlendColor) \
    X(PFNGLCULLFACEPROC, CullFace) \
    X(PFNGLPIXELSTOREIPROC, PixelStorei) \
    X(PFNGLREADPIXELSPROC, ReadPixels) \
    X(PFNGLGENTEXTURESPROC, GenTextures) \
    X(PFNGLDELETETEXTURESPROC, DeleteTextures) \
    X(PFNGLBINDTEXTUREPROC, BindTexture) \
    X(PFNGLACTIVETEXTUREPROC, ActiveTexture) \
    X(PFNGLTEXIMAGE2DPROC, TexImage2D) \
    X(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D) \
    X(PFNGLTEXPARAMETERIPROC, TexParameteri) \
    X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap) \
    X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer) \
    X(PFNGLDRAWBUFFERSPROC, DrawBuffers) \
    X(PFNGLGENBUFFERSPROC, GenBuffers) \
    X(PFNGLBINDBUFFERPROC, BindBuffer) \
    X(PFNGLBUFFERDATAPROC, BufferData) \
    X(PFNGLBUFFERSUBDATAPROC, BufferSubData) \
    X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray) \
    X(PFNGLDRAWARRAYSPROC, DrawArrays) \
    X(PFNGLCREATESHADERPROC, CreateShader) \
    X(PFNGLSHADERSOURCEPROC, ShaderSource) \
    X(PFNGLCOMPILESHADERPROC, CompileShader) \
    X(PFNGLGETSHADERIVPROC, GetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, CreateProgram) \
    X(PFNGLATTACHSHADERPROC, AttachShader) \
    X(PFNGLBINDATTRIBLOCATIONPROC, BindAttribLocation) \
    X(PFNGLBINDFRAGDATALOCATIONINDEXEDPROC, BindFragDataLocationIndexed) \
    X(PFNGLLINKPROGRAMPROC, LinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, GetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, UseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, Uniform1i) \
    X(PFNGLUNIFORM1FPROC, Uniform1f) \
    X(PFNGLUNIFORM2FPROC, Uniform2f) \
    X(PFNGLUNIFORM4FPROC, Uniform4f) \
    X(PFNGLUNIFORM4IPROC, Uniform4i) \
    X(PFNGLUNIFORMMATRIX4FVPROC, UniformMatrix4fv)

namespace gl
{
#define RT_GL_DECL(type, name) extern type name;
    RT_GL_FUNCS(RT_GL_DECL)
#undef RT_GL_DECL

    extern PFNGLTEXTUREBARRIERPROC TextureBarrier;   // optional, may be null
    extern PFNGLCLIPCONTROLPROC ClipControl;         // optional, may be null
    // Optional (GL 4.3): batched model draws (one multi-draw per run of model batches in one state).
    extern PFNGLTEXBUFFERPROC TexBuffer;
    extern PFNGLVERTEXATTRIBIPOINTERPROC VertexAttribIPointer;
    extern PFNGLVERTEXATTRIBDIVISORPROC VertexAttribDivisor;
    extern PFNGLMULTIDRAWELEMENTSINDIRECTPROC MultiDrawElementsIndirect;
    extern PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC DrawArraysInstancedBaseInstance;
    using GetProc = void *(*)(const char *);
    bool load(GetProc getProc);
}
