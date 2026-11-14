// postfx.cpp
//   The shader pipeline (see postfx.h for where its information comes from).
//
//   Per frame, once the 3D scene is complete:
//     capture     the window's colour and depth are copied into our textures (a blit, which also
//                 resolves multisampling, or a texture copy if the blit is refused)
//     lighting    half resolution: ambient occlusion and screen-space sun shadows, by marching
//                 from each pixel toward the sun through the depth buffer
//     composite   full resolution, linear colour: dirt detail on the ground (only where the depth
//                 still matches the snapshot taken when the terrain pass ended), AO and shadows,
//                 and the time of day: grading, haze, and a sunset / night sky with stars and moon
//     bloom       six-level downsample / upsample chain of what's bright
//     sun rays    radial blur of the bright sky around the sun
//     DOF         auto-focus (eased on the GPU), half resolution bokeh gather, merged by depth
//     motion blur camera motion: every pixel reprojected into last frame's view through depth
//     grade       bloom and rays added, exposure, saturation, contrast, vignette -> display
//     final       FXAA and contrast-adaptive sharpening, written back into the window
//
//   Like the settings panel: every GL call goes straight to the driver, and every piece of GL
//   state touched is saved and restored around the work.

#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <share.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "bike.h"
#include "config.h"
#include "dab.h"
#include "hooks.h"
#include "lobby_wheels.h"
#include "log.h"
#include "mathlib.h"
#include "motion.h"
#include "postfx.h"
#include "postfx_shaders.h"
#include "replay.h"
#include "timeofday.h"

namespace mxcam {
namespace postfx {
namespace {

// ===========================================================================
// OpenGL constants past 1.1 and the driver's entry points
// ===========================================================================
constexpr GLenum kFramebuffer               = 0x8D40;
constexpr GLenum kReadFramebuffer           = 0x8CA8;
constexpr GLenum kDrawFramebuffer           = 0x8CA9;
constexpr GLenum kReadFramebufferBinding    = 0x8CAA;
constexpr GLenum kDrawFramebufferBinding    = 0x8CA6;
constexpr GLenum kColorAttachment0          = 0x8CE0;
constexpr GLenum kDepthAttachment           = 0x8D00;
constexpr GLenum kDepthStencilAttachment    = 0x821A;
constexpr GLenum kFramebufferComplete       = 0x8CD5;
constexpr GLenum kRgba16f                   = 0x881A;
constexpr GLenum kRg8                       = 0x822B;
constexpr GLenum kRg                        = 0x8227;
constexpr GLenum kR32f                      = 0x822E;
constexpr GLenum kRed                       = 0x1903;
constexpr GLenum kHalfFloat                 = 0x140B;
constexpr GLenum kDepth24Stencil8           = 0x88F0;
constexpr GLenum kDepthStencil              = 0x84F9;
constexpr GLenum kUnsignedInt248            = 0x84FA;
constexpr GLenum kDepthComponent24          = 0x81A6;
constexpr GLenum kTextureCompareMode        = 0x884C;
constexpr GLenum kClampToEdge               = 0x812F;
constexpr GLenum kArrayBuffer               = 0x8892;
constexpr GLenum kArrayBufferBinding        = 0x8894;
constexpr GLenum kStaticDraw                = 0x88E4;
constexpr GLenum kVertexShader              = 0x8B31;
constexpr GLenum kFragmentShader            = 0x8B30;
constexpr GLenum kCompileStatus             = 0x8B81;
constexpr GLenum kLinkStatus                = 0x8B82;
constexpr GLenum kInfoLogLength             = 0x8B84;
constexpr GLenum kCurrentProgram            = 0x8B8D;
constexpr GLenum kTexture0                  = 0x84C0;
constexpr GLenum kActiveTexture             = 0x84E0;
constexpr GLenum kVertexArrayBinding        = 0x85B5;
constexpr GLenum kSamplerBinding            = 0x8919;
constexpr GLenum kBlendDstRgb               = 0x80C8;
constexpr GLenum kBlendSrcRgb               = 0x80C9;
constexpr GLenum kBlendDstAlpha             = 0x80CA;
constexpr GLenum kBlendSrcAlpha             = 0x80CB;
constexpr GLenum kBlendEquationRgb          = 0x8009;
constexpr GLenum kBlendEquationAlpha        = 0x883D;
constexpr GLenum kFuncAdd                   = 0x8006;
constexpr GLenum kFuncMax                   = 0x8008;
constexpr GLenum kFuncMin                   = 0x8007;
constexpr GLenum kPixelPackBuffer         = 0x88EB;  // GL_PIXEL_PACK_BUFFER
constexpr GLenum kPixelPackBufferBinding  = 0x88ED;
constexpr GLenum kR8                        = 0x8229;
constexpr GLenum kFramebufferSrgb           = 0x8DB9;
constexpr GLenum kSampleAlphaToCoverage     = 0x809E;
constexpr GLenum kRasterizerDiscard         = 0x8C89;
constexpr GLenum kPixelUnpackBuffer         = 0x88EC;
constexpr GLenum kPixelUnpackBufferBinding  = 0x88EF;
constexpr GLenum kTextureMaxAnisotropy      = 0x84FE;
constexpr GLenum kAlphaTest                 = 0x0BC0;
constexpr GLenum kTextureCompareFunc        = 0x884D;
constexpr GLenum kCompareRefToTexture       = 0x884E;

struct GLApi {
    decltype(&::glGetIntegerv)         GetIntegerv;
    decltype(&::glGetBooleanv)         GetBooleanv;
    decltype(&::glGetError)            GetError;
    decltype(&::glIsEnabled)           IsEnabled;
    decltype(&::glEnable)              Enable;
    decltype(&::glDisable)             Disable;
    decltype(&::glViewport)            Viewport;
    decltype(&::glColorMask)           ColorMask;
    decltype(&::glDepthMask)           DepthMask;
    decltype(&::glDepthFunc)           DepthFunc;
    decltype(&::glDepthRange)          DepthRange;
    decltype(&::glClear)               Clear;
    decltype(&::glClearDepth)          ClearDepth;
    decltype(&::glGetFloatv)           GetFloatv;
    decltype(&::glGenTextures)         GenTextures;
    decltype(&::glDeleteTextures)      DeleteTextures;
    decltype(&::glBindTexture)         BindTexture;
    decltype(&::glTexParameteri)       TexParameteri;
    decltype(&::glTexParameterf)       TexParameterf;
    decltype(&::glTexImage2D)          TexImage2D;
    decltype(&::glPixelStorei)         PixelStorei;
    decltype(&::glDrawArrays)          DrawArrays;
    decltype(&::glReadBuffer)          ReadBuffer;
    decltype(&::glDrawBuffer)          DrawBuffer;
    decltype(&::glCopyTexSubImage2D)   CopyTexSubImage2D;
    decltype(&::wglGetCurrentContext)  GetCurrentContext;
    decltype(&::wglGetCurrentDC)       GetCurrentDC;
    GLuint (APIENTRY* CreateShader)(GLenum);
    void   (APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
    void   (APIENTRY* CompileShader)(GLuint);
    void   (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    void   (APIENTRY* DeleteShader)(GLuint);
    GLuint (APIENTRY* CreateProgram)();
    void   (APIENTRY* DeleteProgram)(GLuint);
    void   (APIENTRY* AttachShader)(GLuint, GLuint);
    void   (APIENTRY* BindAttribLocation)(GLuint, GLuint, const char*);
    void   (APIENTRY* LinkProgram)(GLuint);
    void   (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    void   (APIENTRY* UseProgram)(GLuint);
    GLint  (APIENTRY* GetUniformLocation)(GLuint, const char*);
    void   (APIENTRY* Uniform1i)(GLint, GLint);
    void   (APIENTRY* Uniform1f)(GLint, GLfloat);
    void   (APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void   (APIENTRY* Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
    void   (APIENTRY* Uniform1fv)(GLint, GLsizei, const GLfloat*);
    void   (APIENTRY* UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    void   (APIENTRY* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    void   (APIENTRY* GenBuffers)(GLsizei, GLuint*);
    void   (APIENTRY* BindBuffer)(GLenum, GLuint);
    void   (APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
    void   (APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void   (APIENTRY* EnableVertexAttribArray)(GLuint);
    void   (APIENTRY* ActiveTexture)(GLenum);
    void   (APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void   (APIENTRY* BlendEquationSeparate)(GLenum, GLenum);
    void   (APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
    void   (APIENTRY* BindVertexArray)(GLuint);
    void   (APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
    void   (APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
    void   (APIENTRY* BindFramebuffer)(GLenum, GLuint);
    void   (APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (APIENTRY* CheckFramebufferStatus)(GLenum);
    void   (APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void   (APIENTRY* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
    void   (APIENTRY* GenerateMipmap)(GLenum);
    void   (APIENTRY* BindSampler)(GLuint, GLuint);  // optional (GL 3.3)
    decltype(&::glScissor)             Scissor;
    // Optional (GL 3.2): reading the depth back without waiting for it (FillTerrainFromDepth).
    void*  (APIENTRY* FenceSync)(GLenum, GLbitfield);
    void   (APIENTRY* GenQueries)(GLsizei, GLuint*);           // (optional: the trace's GPU timings)
    void   (APIENTRY* BeginQuery)(GLenum, GLuint);
    void   (APIENTRY* EndQuery)(GLenum);
    void   (APIENTRY* GetQueryObjectiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* GetQueryObjectui64v)(GLuint, GLenum, unsigned long long*);
    GLenum (APIENTRY* ClientWaitSync)(void*, GLbitfield, unsigned long long);
    void   (APIENTRY* DeleteSync)(void*);
    void   (APIENTRY* GetBufferSubData)(GLenum, ptrdiff_t, ptrdiff_t, void*);
    void   (APIENTRY* DeleteBuffers)(GLsizei, const GLuint*);
};

GLApi g_gl{};
int   g_glState = 0;  // 0 = not loaded yet, 1 = ready, -1 = missing functions

template <class T>
bool LoadFunction(T& slot, const char* name)
{
    slot = reinterpret_cast<T>(GetRealGLFunction(name));
    if (!slot)
        Log("postfx: %s is missing", name);
    return slot != nullptr;
}

bool LoadGL()
{
    if (g_glState != 0)
        return g_glState > 0;
    bool ok = true;
#define MXCAM_LOAD(field, name) ok = LoadFunction(g_gl.field, name) && ok
    MXCAM_LOAD(GetIntegerv, "glGetIntegerv");
    MXCAM_LOAD(GetBooleanv, "glGetBooleanv");
    MXCAM_LOAD(GetError, "glGetError");
    MXCAM_LOAD(IsEnabled, "glIsEnabled");
    MXCAM_LOAD(Enable, "glEnable");
    MXCAM_LOAD(Disable, "glDisable");
    MXCAM_LOAD(Viewport, "glViewport");
    MXCAM_LOAD(ColorMask, "glColorMask");
    MXCAM_LOAD(DepthMask, "glDepthMask");
    MXCAM_LOAD(DepthFunc, "glDepthFunc");
    MXCAM_LOAD(DepthRange, "glDepthRange");
    MXCAM_LOAD(Clear, "glClear");
    MXCAM_LOAD(ClearDepth, "glClearDepth");
    MXCAM_LOAD(GetFloatv, "glGetFloatv");
    MXCAM_LOAD(GenTextures, "glGenTextures");
    MXCAM_LOAD(DeleteTextures, "glDeleteTextures");
    MXCAM_LOAD(BindTexture, "glBindTexture");
    MXCAM_LOAD(TexParameteri, "glTexParameteri");
    MXCAM_LOAD(TexParameterf, "glTexParameterf");
    MXCAM_LOAD(TexImage2D, "glTexImage2D");
    MXCAM_LOAD(PixelStorei, "glPixelStorei");
    MXCAM_LOAD(DrawArrays, "glDrawArrays");
    MXCAM_LOAD(ReadBuffer, "glReadBuffer");
    MXCAM_LOAD(DrawBuffer, "glDrawBuffer");
    MXCAM_LOAD(CopyTexSubImage2D, "glCopyTexSubImage2D");
    MXCAM_LOAD(GetCurrentContext, "wglGetCurrentContext");
    MXCAM_LOAD(GetCurrentDC, "wglGetCurrentDC");
    MXCAM_LOAD(CreateShader, "glCreateShader");
    MXCAM_LOAD(ShaderSource, "glShaderSource");
    MXCAM_LOAD(CompileShader, "glCompileShader");
    MXCAM_LOAD(GetShaderiv, "glGetShaderiv");
    MXCAM_LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    MXCAM_LOAD(DeleteShader, "glDeleteShader");
    MXCAM_LOAD(CreateProgram, "glCreateProgram");
    MXCAM_LOAD(DeleteProgram, "glDeleteProgram");
    MXCAM_LOAD(AttachShader, "glAttachShader");
    MXCAM_LOAD(BindAttribLocation, "glBindAttribLocation");
    MXCAM_LOAD(LinkProgram, "glLinkProgram");
    MXCAM_LOAD(GetProgramiv, "glGetProgramiv");
    MXCAM_LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    MXCAM_LOAD(UseProgram, "glUseProgram");
    MXCAM_LOAD(GetUniformLocation, "glGetUniformLocation");
    MXCAM_LOAD(Uniform1i, "glUniform1i");
    MXCAM_LOAD(Uniform1f, "glUniform1f");
    MXCAM_LOAD(Uniform2f, "glUniform2f");
    MXCAM_LOAD(Uniform3f, "glUniform3f");
    MXCAM_LOAD(Uniform1fv, "glUniform1fv");
    MXCAM_LOAD(UniformMatrix3fv, "glUniformMatrix3fv");
    MXCAM_LOAD(UniformMatrix4fv, "glUniformMatrix4fv");
    MXCAM_LOAD(GenBuffers, "glGenBuffers");
    MXCAM_LOAD(BindBuffer, "glBindBuffer");
    MXCAM_LOAD(BufferData, "glBufferData");
    MXCAM_LOAD(VertexAttribPointer, "glVertexAttribPointer");
    MXCAM_LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    MXCAM_LOAD(ActiveTexture, "glActiveTexture");
    MXCAM_LOAD(BlendFuncSeparate, "glBlendFuncSeparate");
    MXCAM_LOAD(BlendEquationSeparate, "glBlendEquationSeparate");
    MXCAM_LOAD(GenVertexArrays, "glGenVertexArrays");
    MXCAM_LOAD(BindVertexArray, "glBindVertexArray");
    MXCAM_LOAD(GenFramebuffers, "glGenFramebuffers");
    MXCAM_LOAD(DeleteFramebuffers, "glDeleteFramebuffers");
    MXCAM_LOAD(BindFramebuffer, "glBindFramebuffer");
    MXCAM_LOAD(FramebufferTexture2D, "glFramebufferTexture2D");
    MXCAM_LOAD(CheckFramebufferStatus, "glCheckFramebufferStatus");
    MXCAM_LOAD(BlitFramebuffer, "glBlitFramebuffer");
    MXCAM_LOAD(ReadPixels, "glReadPixels");
    MXCAM_LOAD(GenerateMipmap, "glGenerateMipmap");
    MXCAM_LOAD(Scissor, "glScissor");
#undef MXCAM_LOAD
    g_gl.BindSampler = reinterpret_cast<decltype(g_gl.BindSampler)>(GetRealGLFunction("glBindSampler"));
    g_gl.FenceSync = reinterpret_cast<decltype(g_gl.FenceSync)>(GetRealGLFunction("glFenceSync"));
    g_gl.GenQueries = reinterpret_cast<decltype(g_gl.GenQueries)>(GetRealGLFunction("glGenQueries"));
    g_gl.BeginQuery = reinterpret_cast<decltype(g_gl.BeginQuery)>(GetRealGLFunction("glBeginQuery"));
    g_gl.EndQuery = reinterpret_cast<decltype(g_gl.EndQuery)>(GetRealGLFunction("glEndQuery"));
    g_gl.GetQueryObjectiv = reinterpret_cast<decltype(g_gl.GetQueryObjectiv)>(GetRealGLFunction("glGetQueryObjectiv"));
    g_gl.GetQueryObjectui64v = reinterpret_cast<decltype(g_gl.GetQueryObjectui64v)>(GetRealGLFunction("glGetQueryObjectui64v"));
    g_gl.ClientWaitSync = reinterpret_cast<decltype(g_gl.ClientWaitSync)>(GetRealGLFunction("glClientWaitSync"));
    g_gl.DeleteSync = reinterpret_cast<decltype(g_gl.DeleteSync)>(GetRealGLFunction("glDeleteSync"));
    g_gl.GetBufferSubData = reinterpret_cast<decltype(g_gl.GetBufferSubData)>(GetRealGLFunction("glGetBufferSubData"));
    g_gl.DeleteBuffers = reinterpret_cast<decltype(g_gl.DeleteBuffers)>(GetRealGLFunction("glDeleteBuffers"));
    g_glState = ok ? 1 : -1;
    if (!ok)
        Log("postfx: OpenGL 3.0 (framebuffer objects) is needed for the shaders; they stay off");
    return ok;
}

// Clears the driver's error flags (the probes below look at errors of their own).
void DrainErrors()
{
    for (int i = 0; i < 32 && g_gl.GetError() != GL_NO_ERROR; ++i) {
    }
}

// ===========================================================================
// Saved GL state
// ===========================================================================
constexpr int kUnits = 8;  // texture units the passes use

struct SavedState {
    GLint     program = 0, vao = 0, arrayBuffer = 0, activeTexture = 0, readFb = 0, drawFb = 0, readBuffer0 = 0;
    GLint     unpackBuffer = 0;
    GLint     textures[kUnits] = {}, samplers[kUnits] = {};
    GLint     viewport[4] = {}, scissor[4] = {};
    GLint     blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0, blendEqRgb = 0, blendEqAlpha = 0;
    GLboolean colorMask[4] = {}, depthMask = GL_TRUE;
    bool      blend = false, depthTest = false, cullFace = false, scissorTest = false, stencilTest = false;
    bool      srgb = false, alphaToCoverage = false, rasterizerDiscard = false, alphaTest = false;
};

bool g_compatibility = true;  // the context has fixed-function state (GL_ALPHA_TEST)

void Save(SavedState& s)
{
    g_gl.GetIntegerv(kCurrentProgram, &s.program);
    g_gl.GetIntegerv(kVertexArrayBinding, &s.vao);
    g_gl.GetIntegerv(kArrayBufferBinding, &s.arrayBuffer);
    g_gl.GetIntegerv(kPixelUnpackBufferBinding, &s.unpackBuffer);
    g_gl.GetIntegerv(kActiveTexture, &s.activeTexture);
    for (int unit = 0; unit < kUnits; ++unit) {
        g_gl.ActiveTexture(kTexture0 + unit);
        g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.textures[unit]);
        if (g_gl.BindSampler)
            g_gl.GetIntegerv(kSamplerBinding, &s.samplers[unit]);
    }
    g_gl.GetIntegerv(kReadFramebufferBinding, &s.readFb);
    g_gl.GetIntegerv(kDrawFramebufferBinding, &s.drawFb);
    g_gl.BindFramebuffer(kReadFramebuffer, 0);
    g_gl.GetIntegerv(GL_READ_BUFFER, &s.readBuffer0);  // the window's own read buffer
    g_gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    g_gl.GetIntegerv(GL_SCISSOR_BOX, s.scissor);
    g_gl.GetIntegerv(kBlendSrcRgb, &s.blendSrcRgb);
    g_gl.GetIntegerv(kBlendDstRgb, &s.blendDstRgb);
    g_gl.GetIntegerv(kBlendSrcAlpha, &s.blendSrcAlpha);
    g_gl.GetIntegerv(kBlendDstAlpha, &s.blendDstAlpha);
    g_gl.GetIntegerv(kBlendEquationRgb, &s.blendEqRgb);
    g_gl.GetIntegerv(kBlendEquationAlpha, &s.blendEqAlpha);
    g_gl.GetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    g_gl.GetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    s.blend             = g_gl.IsEnabled(GL_BLEND) != GL_FALSE;
    s.depthTest         = g_gl.IsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    s.cullFace          = g_gl.IsEnabled(GL_CULL_FACE) != GL_FALSE;
    s.scissorTest       = g_gl.IsEnabled(GL_SCISSOR_TEST) != GL_FALSE;
    s.stencilTest       = g_gl.IsEnabled(GL_STENCIL_TEST) != GL_FALSE;
    s.srgb              = g_gl.IsEnabled(kFramebufferSrgb) != GL_FALSE;
    s.alphaToCoverage   = g_gl.IsEnabled(kSampleAlphaToCoverage) != GL_FALSE;
    s.rasterizerDiscard = g_gl.IsEnabled(kRasterizerDiscard) != GL_FALSE;
    s.alphaTest         = g_compatibility && g_gl.IsEnabled(kAlphaTest) != GL_FALSE;
}

void SetCapability(GLenum capability, bool enabled)
{
    if (enabled)
        g_gl.Enable(capability);
    else
        g_gl.Disable(capability);
}

// The state every pass draws with.
void PrepareState()
{
    g_gl.Disable(GL_BLEND);
    g_gl.Disable(GL_DEPTH_TEST);
    g_gl.Disable(GL_CULL_FACE);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_STENCIL_TEST);
    g_gl.Disable(kFramebufferSrgb);
    g_gl.Disable(kSampleAlphaToCoverage);
    g_gl.Disable(kRasterizerDiscard);
    if (g_compatibility)
        g_gl.Disable(kAlphaTest);
    g_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_gl.DepthMask(GL_TRUE);  // depth blits honour it
    g_gl.BindBuffer(kPixelUnpackBuffer, 0);
}

void Restore(const SavedState& s)
{
    g_gl.BindVertexArray(s.vao);
    g_gl.BindBuffer(kArrayBuffer, s.arrayBuffer);
    g_gl.BindBuffer(kPixelUnpackBuffer, s.unpackBuffer);
    g_gl.UseProgram(s.program);
    for (int unit = 0; unit < kUnits; ++unit) {
        g_gl.ActiveTexture(kTexture0 + unit);
        g_gl.BindTexture(GL_TEXTURE_2D, s.textures[unit]);
        if (g_gl.BindSampler)
            g_gl.BindSampler(unit, s.samplers[unit]);
    }
    g_gl.ActiveTexture(s.activeTexture);
    g_gl.BindFramebuffer(kReadFramebuffer, 0);
    g_gl.ReadBuffer(s.readBuffer0);
    g_gl.BindFramebuffer(kReadFramebuffer, s.readFb);
    g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFb);
    g_gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    g_gl.Scissor(s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
    g_gl.BlendFuncSeparate(s.blendSrcRgb, s.blendDstRgb, s.blendSrcAlpha, s.blendDstAlpha);
    g_gl.BlendEquationSeparate(s.blendEqRgb, s.blendEqAlpha);
    g_gl.ColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    g_gl.DepthMask(s.depthMask);
    SetCapability(GL_BLEND, s.blend);
    SetCapability(GL_DEPTH_TEST, s.depthTest);
    SetCapability(GL_CULL_FACE, s.cullFace);
    SetCapability(GL_SCISSOR_TEST, s.scissorTest);
    SetCapability(GL_STENCIL_TEST, s.stencilTest);
    SetCapability(kFramebufferSrgb, s.srgb);
    SetCapability(kSampleAlphaToCoverage, s.alphaToCoverage);
    SetCapability(kRasterizerDiscard, s.rasterizerDiscard);
    if (g_compatibility)
        SetCapability(kAlphaTest, s.alphaTest);
}

// ===========================================================================
// GPU resources (per GL context)
// ===========================================================================
struct Program {
    GLuint id = 0;
    std::unordered_map<std::string, GLint> locations;

    GLint operator[](const char* name)
    {
        const auto found = locations.find(name);
        if (found != locations.end())
            return found->second;
        const GLint location = g_gl.GetUniformLocation(id, name);
        locations.emplace(name, location);
        return location;
    }
};

enum ProgramId { kLighting, kComposite, kBloomDown, kBloomUp, kRays, kFocus, kDof, kDofMerge, kMotion, kGrade,
                 kFinal, kStamp, kProgramCount };

struct ProgramSource {
    const char* name;
    const char* body;
};
const ProgramSource kSources[kProgramCount] = {
    { "lighting", shaders::kLighting },   { "composite", shaders::kComposite }, { "bloom down", shaders::kBloomDown },
    { "bloom up", shaders::kBloomUp },    { "sun rays", shaders::kRays },        { "focus", shaders::kFocus },
    { "dof", shaders::kDof },             { "dof merge", shaders::kDofMerge },   { "motion blur", shaders::kMotion },
    { "grade", shaders::kGrade },         { "final", shaders::kFinal },          { "tire marks", shaders::kStamp },
};

struct Target {
    GLuint tex = 0, fbo = 0;
    int    w = 0, h = 0;
};

constexpr int kBloomLevels = 6;
constexpr int kDetailSize  = 256;

struct Resources {
    HGLRC   context = nullptr;
    bool    failed  = false;
    GLuint  vao = 0, vbo = 0;
    Program programs[kProgramCount];
    GLuint  detail = 0;
    int     width = 0, height = 0;
    GLuint  captureFbo = 0, color = 0, depth = 0;  // the window's image
    GLuint  terrainFbo = 0, terrainDepth = 0;      // depth when the terrain pass ended
    GLuint  sampleFbo = 0, sampleDepth = 0;        // that depth, downsampled, to read back
    int     samplePacking = -1;                    // the depth format it was made in (depthMode's)
    bool    sampleFailed = false;                  // reading it back didn't work: tried once
    // That read, without waiting for it: into one of these buffers, taken a frame or two later when
    // the GPU has done it (with the view and projection of the frame it was read in).
    struct SampleRead {
        GLuint buffer = 0;
        void*  fence  = nullptr;  // set while the read is on its way
        double invProj[16] = {}, view[16] = {};
    };
    static constexpr int kSampleReads = 3;
    SampleRead sampleReads[kSampleReads];
    int        sampleNext = 0;                     // the slot the next read goes in
    GLuint  shadowFbo = 0, shadowMap = 0;          // depth seen from the sun
    Target  marks;                                 // tire marks: the map of the ground around you (TireMarks)
    bool    marksCleared = false;
    int     shadowSize = 0;
    int     colorMode = 0;  // 0 = untried, 1 = blit, 2 = copy, -1 = nothing works
    int     depthMode = 0;  // 0 = untried, 1 = blit (depth24 + stencil8), 2 = blit (depth24), 3 = copy, -1 = none
    bool    colorVerified = false, depthVerified = false;
    Target  hdr, a, b, ldr, light, dof, rays, focus[2];
    Target  bloom[kBloomLevels];
    int     focusIndex = 0;
    bool    focusValid = false;
    GLuint  wheelFbo = 0, wheelDepth = 0;          // your wheels as the game drew them (tire dirt's mask)
    int     wheelWidth = 0, wheelHeight = 0;
    GLuint  headProgram = 0, headVao = 0;          // the shadow stand-ins (DrawStandIns)
    GLint   shapeLight = -1, shapeModel = -1, shapeKind = -1, shapeMinor = -1;
    bool    headTried = false;
    Program snow;                                  // the snowfall (DrawSnowfall)
    GLuint  snowVao = 0;
    bool    snowTried = false;
    Program rain;                                  // the rainfall (DrawRainfall; its vertex array is snowVao's kind)
    GLuint  rainVao = 0;
    bool    rainTried = false;
};
Resources g_res;
// The tire dirt's mask this frame (see BeginWheelMask).
struct WheelMaskState { bool cleared = false, drawn = false; GLint viewport[4] = {}; GLint drawFb = 0, depthFunc = 0;
                        GLboolean depthMask = GL_TRUE; bool depthTest = false, scissor = false, stencil = false, cull = false; };
WheelMaskState g_wheelMask;

// ---- Per-frame scene information (from the hooks) -----------------------------------------
struct Frame {
    bool   haveView = false;
    double raw[16] = {}, rendered[16] = {};
    bool   applied = false;
    bool   terrainSnapshot = false;
    bool   haveLight = false;
    float  lightEye[3] = {};
};
Frame  g_frame;
double g_lastRaw[16] = {};
bool   g_haveLastRaw = false;
bool   g_lastFrame3D = false;  // the last frame drew the 3D scene (for draws before this one's camera)

float  g_projCamera[16] = {}, g_projOther[16] = {};
bool   g_haveProjCamera = false, g_haveProjOther = false;

Vec3   g_gameSun{ 0.35, 0.8, 0.45 };  // world, toward the game's sun (a guess until the light is seen)
bool   g_haveGameSun = false;

struct Motion {
    bool   valid = false;
    double viewProj[16] = {};
    Vec3   eye, forward;
};
Motion g_motion;

// The sun's camera for this frame's shadow map (set up by the first object that casts).
// Cascades: the map is a 2 x 2 grid of four, each the same square seen from the sun but a bigger
// one each time, from a sharp one close around you out to the whole shadow distance. Each point on
// screen takes the smallest that has it: sharp shadows near you, shadows still far out.
constexpr int kCascades = 4;
struct ShadowFrame {
    bool   setup = false, usable = false, cleared = false, drawn = false;
    double toLight[16] = {};    // the drawn camera's eye space -> the sun's view (centred on the camera)
    double half = 25.0;         // game units, half the side of the biggest cascade (the shadow distance)
    double depth = 100.0;       // game units on either side of the camera, along the light
    double cascadeHalf[kCascades] = {};          // half the side of each cascade's square
    double cascadeX[kCascades] = {}, cascadeY[kCascades] = {};  // its centre in the sun's view
    float  cascadeProjection[kCascades][16] = {};
};
ShadowFrame g_shadow;

double g_lastApplyTime = -1;
double g_lastDt = 1.0 / 60.0;
char   g_status[128] = "";
bool   g_lastDepthOk = false, g_lastTerrainMask = false;

// Logged once each.
bool g_loggedBeforeHud = false, g_loggedEndOfFrame = false, g_loggedNoProjection = false, g_loggedNoLight = false;
bool g_loggedTerrainFbo = false, g_loggedFirstApply = false;

double Seconds()
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

// ---- Shaders --------------------------------------------------------------------------------
void LogInfo(const char* what, const std::string& text)
{
    Log("postfx: %s:", what);
    size_t start = 0;
    for (int lines = 0; start < text.size() && lines < 24; ++lines) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        if (end > start)
            Log("    %.*s", static_cast<int>(end - start), text.c_str() + start);
        start = end + 1;
    }
}

GLuint CompileShader(GLenum type, const char* version, const char* a, const char* b, std::string& error)
{
    const char* sources[] = { version, a, b ? b : "" };
    const GLuint shader = g_gl.CreateShader(type);
    g_gl.ShaderSource(shader, 3, sources, nullptr);
    g_gl.CompileShader(shader);
    GLint ok = 0;
    g_gl.GetShaderiv(shader, kCompileStatus, &ok);
    if (!ok) {
        GLint length = 0;
        g_gl.GetShaderiv(shader, kInfoLogLength, &length);
        error.assign(static_cast<size_t>(length > 1 ? length : 1), '\0');
        g_gl.GetShaderInfoLog(shader, length, nullptr, &error[0]);
        g_gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint BuildProgram(const char* version, const char* body, std::string& error)
{
    const GLuint vs = CompileShader(kVertexShader, version, shaders::kVertex, nullptr, error);
    if (!vs)
        return 0;
    const GLuint fs = CompileShader(kFragmentShader, version, shaders::kPrelude, body, error);
    if (!fs) {
        g_gl.DeleteShader(vs);
        return 0;
    }
    const GLuint program = g_gl.CreateProgram();
    g_gl.AttachShader(program, vs);
    g_gl.AttachShader(program, fs);
    g_gl.BindAttribLocation(program, 0, "a_pos");
    g_gl.LinkProgram(program);
    g_gl.DeleteShader(vs);  // freed with the program
    g_gl.DeleteShader(fs);
    GLint linked = 0;
    g_gl.GetProgramiv(program, kLinkStatus, &linked);
    if (!linked) {
        GLint length = 0;
        g_gl.GetProgramiv(program, kInfoLogLength, &length);
        error.assign(static_cast<size_t>(length > 1 ? length : 1), '\0');
        g_gl.GetProgramInfoLog(program, length, nullptr, &error[0]);
        g_gl.DeleteProgram(program);
        return 0;
    }
    return program;
}

bool BuildPrograms()
{
    const char* const versions[] = { "#version 150\n", "#version 130\n" };
    for (const char* version : versions) {
        bool ok = true;
        std::string error;
        int failed = -1;
        for (int i = 0; i < kProgramCount && ok; ++i) {
            g_res.programs[i] = Program{};
            g_res.programs[i].id = BuildProgram(version, kSources[i].body, error);
            if (!g_res.programs[i].id) {
                ok = false;
                failed = i;
            }
        }
        if (ok) {
            Log("postfx: shaders ready (GLSL %.3s)", version + 9);
            return true;
        }
        LogInfo((std::string("the '") + kSources[failed].name + "' shader failed with GLSL " + std::string(version + 9, 3)).c_str(), error);
        for (Program& program : g_res.programs) {
            if (program.id)
                g_gl.DeleteProgram(program.id);
            program = Program{};
        }
    }
    return false;
}

// ---- Dirt detail texture: tiling, made once on the CPU ----------------------------------------
uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

double Lattice(int x, int y, int period, uint32_t seed)
{
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    return (Hash(static_cast<uint32_t>(x) * 73856093U ^ static_cast<uint32_t>(y) * 19349663U ^ seed) & 0xFFFF) / 65535.0;
}

// Smooth value noise that repeats every `period` lattice cells.
double ValueNoise(double u, double v, int period, uint32_t seed)
{
    const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
    double fx = u - x0, fy = v - y0;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const double a = Lattice(x0, y0, period, seed), b = Lattice(x0 + 1, y0, period, seed);
    const double c = Lattice(x0, y0 + 1, period, seed), d = Lattice(x0 + 1, y0 + 1, period, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// RGBA: the detail's normal (x, y), its height, and an albedo variation; all centred on 0.5.
std::vector<uint8_t> MakeDetailTexture(int n)
{
    std::vector<double> height(static_cast<size_t>(n) * n), tone(height.size());
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const double u = static_cast<double>(x) / n, v = static_cast<double>(y) / n;
            double sum = 0, amplitude = 0.5, total = 0;
            for (int octave = 0; octave < 5; ++octave) {
                const int period = 8 << octave;
                sum += amplitude * ValueNoise(u * period, v * period, period, 0x9E37U * (octave + 1));
                total += amplitude;
                amplitude *= 0.55;
            }
            const double fbm = sum / total;
            const double speckle = ValueNoise(u * 128, v * 128, 128, 0xBEEFU);
            height[y * n + x] = fbm * 0.6;
            tone[y * n + x] = 0.5 + (speckle - 0.5) * 0.6 + (fbm - 0.5) * 0.35;
        }
    }
    // Pebbles and small clods.
    uint32_t seed = 12345;
    const auto random = [&seed]() { seed = Hash(seed + 0x9E3779B9U); return (seed & 0xFFFFFF) / 16777215.0; };
    for (int p = 0; p < 340; ++p) {
        const double cx = random() * n, cy = random() * n;
        const double radius = 1.0 + random() * random() * 6.0;
        const double lift = 0.4 + random() * 0.6;
        const double shade = random();
        const int reach = static_cast<int>(std::ceil(radius));
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const int px = static_cast<int>(std::floor(cx)) + dx, py = static_cast<int>(std::floor(cy)) + dy;
                const double ex = px + 0.5 - cx, ey = py + 0.5 - cy;
                const double d = std::sqrt(ex * ex + ey * ey) / radius;
                if (d >= 1.0)
                    continue;
                const size_t i = static_cast<size_t>(((py % n) + n) % n) * n + ((px % n) + n) % n;
                const double bump = std::sqrt(1.0 - d * d);
                height[i] = std::fmax(height[i], 0.22 + 0.6 * bump * lift);
                const double edge = Clamp(bump / 0.35, 0.0, 1.0);
                tone[i] = tone[i] + ((0.25 + 0.55 * shade) - tone[i]) * edge;
            }
        }
    }
    double lo = 1e9, hi = -1e9, mean = 0;
    for (double h : height) {
        lo = std::fmin(lo, h);
        hi = std::fmax(hi, h);
    }
    for (double& h : height) {
        h = (h - lo) / std::fmax(hi - lo, 1e-9);
        mean += h;
    }
    mean /= static_cast<double>(height.size());

    std::vector<uint8_t> rgba(height.size() * 4);
    const auto H = [&](int x, int y) { return height[static_cast<size_t>(((y % n) + n) % n) * n + ((x % n) + n) % n]; };
    const auto Byte = [](double v) { return static_cast<uint8_t>(Clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const double strength = 6.0;
            const Vec3 normal = Normalize(Vec3{ -(H(x + 1, y) - H(x - 1, y)) * strength, -(H(x, y + 1) - H(x, y - 1)) * strength, 1.0 });
            uint8_t* out = &rgba[(static_cast<size_t>(y) * n + x) * 4];
            out[0] = Byte(normal.x * 0.5 + 0.5);
            out[1] = Byte(normal.y * 0.5 + 0.5);
            out[2] = Byte(H(x, y) - mean + 0.5);
            out[3] = Byte(tone[static_cast<size_t>(y) * n + x]);
        }
    }
    return rgba;
}

bool CreateDetailTexture()
{
    const std::vector<uint8_t> pixels = MakeDetailTexture(kDetailSize);
    GLint alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0;
    g_gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    g_gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    g_gl.GetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    g_gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    g_gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    g_gl.GenTextures(1, &g_res.detail);
    g_gl.BindTexture(GL_TEXTURE_2D, g_res.detail);
    g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kDetailSize, kDetailSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    g_gl.GenerateMipmap(GL_TEXTURE_2D);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    DrainErrors();
    g_gl.TexParameterf(GL_TEXTURE_2D, kTextureMaxAnisotropy, 8.0f);  // grazing angles; ignored if unsupported
    DrainErrors();

    g_gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
    g_gl.PixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    return true;
}

// ---- Render targets -------------------------------------------------------------------------
void DeleteTarget(Target& t)
{
    if (t.fbo)
        g_gl.DeleteFramebuffers(1, &t.fbo);
    if (t.tex)
        g_gl.DeleteTextures(1, &t.tex);
    t = Target{};
}

GLuint NewTexture(GLint internal, int w, int h, GLenum format, GLenum type, GLint filter)
{
    GLuint tex = 0;
    g_gl.GenTextures(1, &tex);
    g_gl.BindTexture(GL_TEXTURE_2D, tex);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    g_gl.TexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
    return tex;
}

bool MakeTarget(Target& t, int w, int h, GLint internal, GLenum format, GLenum type)
{
    DeleteTarget(t);
    t.w = w > 0 ? w : 1;
    t.h = h > 0 ? h : 1;
    t.tex = NewTexture(internal, t.w, t.h, format, type, GL_LINEAR);
    g_gl.GenFramebuffers(1, &t.fbo);
    g_gl.BindFramebuffer(kFramebuffer, t.fbo);
    g_gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, t.tex, 0);
    return g_gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
}

// The two depth textures, in the format the current depthMode copies into (untried: the first).
bool MakeDepthTextures()
{
    const bool packed = g_res.depthMode <= 1;
    for (GLuint* tex : { &g_res.depth, &g_res.terrainDepth }) {
        if (*tex)
            g_gl.DeleteTextures(1, tex);
        *tex = packed ? NewTexture(kDepth24Stencil8, g_res.width, g_res.height, kDepthStencil, kUnsignedInt248, GL_NEAREST)
                      : NewTexture(kDepthComponent24, g_res.width, g_res.height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST);
        g_gl.TexParameteri(GL_TEXTURE_2D, kTextureCompareMode, GL_NONE);
    }
    const GLenum attachment = packed ? kDepthStencilAttachment : kDepthAttachment;
    const GLenum other = packed ? kDepthAttachment : kDepthStencilAttachment;
    g_gl.BindFramebuffer(kFramebuffer, g_res.captureFbo);
    g_gl.FramebufferTexture2D(kFramebuffer, other, GL_TEXTURE_2D, 0, 0);
    g_gl.FramebufferTexture2D(kFramebuffer, attachment, GL_TEXTURE_2D, g_res.depth, 0);
    const bool capture = g_gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    g_gl.BindFramebuffer(kFramebuffer, g_res.terrainFbo);
    g_gl.FramebufferTexture2D(kFramebuffer, other, GL_TEXTURE_2D, 0, 0);
    g_gl.FramebufferTexture2D(kFramebuffer, attachment, GL_TEXTURE_2D, g_res.terrainDepth, 0);
    g_gl.DrawBuffer(GL_NONE);  // depth only (framebuffer object state, not the window's)
    g_gl.ReadBuffer(GL_NONE);
    const bool terrain = g_gl.CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
    return capture && terrain;
}

bool MakeSizedTargets(int w, int h)
{
    g_res.width = w;
    g_res.height = h;
    const int hw = (w + 1) / 2, hh = (h + 1) / 2;
    bool ok = true;
    if (g_res.color)
        g_gl.DeleteTextures(1, &g_res.color);
    g_res.color = NewTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
    if (!g_res.captureFbo)
        g_gl.GenFramebuffers(1, &g_res.captureFbo);
    if (!g_res.terrainFbo)
        g_gl.GenFramebuffers(1, &g_res.terrainFbo);
    g_gl.BindFramebuffer(kFramebuffer, g_res.captureFbo);
    g_gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, g_res.color, 0);
    if (g_res.depthMode < 0)
        g_res.depthMode = 0;  // nothing worked at the old size: try everything again
    MakeDepthTextures();
    g_res.depthVerified = g_res.colorVerified = false;

    ok = MakeTarget(g_res.hdr, w, h, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    ok = MakeTarget(g_res.a, w, h, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    ok = MakeTarget(g_res.b, w, h, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    ok = MakeTarget(g_res.ldr, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) && ok;
    ok = MakeTarget(g_res.light, hw, hh, kRg8, kRg, GL_UNSIGNED_BYTE) && ok;
    ok = MakeTarget(g_res.dof, hw, hh, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    ok = MakeTarget(g_res.rays, hw, hh, kRgba16f, GL_RGBA, kHalfFloat) && ok;
    for (int i = 0; i < kBloomLevels; ++i)
        ok = MakeTarget(g_res.bloom[i], w >> (i + 1), h >> (i + 1), kRgba16f, GL_RGBA, kHalfFloat) && ok;
    if (!ok)
        Log("postfx: could not create the %dx%d render targets", w, h);
    return ok;
}

bool EnsureResources(int w, int h)
{
    const HGLRC context = g_gl.GetCurrentContext();
    if (context != g_res.context) {
        g_res = Resources{};  // a new context: the old objects went with the old one
        g_res.context = context;
        DrainErrors();
        g_gl.IsEnabled(kAlphaTest);
        g_compatibility = g_gl.GetError() == GL_NO_ERROR;
    }
    if (g_res.failed)
        return false;

    if (!g_res.programs[0].id) {
        if (!BuildPrograms()) {
            Log("postfx: the shaders could not be compiled; they stay off (details above)");
            g_res.failed = true;
            return false;
        }
        const float triangle[] = { -1, -1, 3, -1, -1, 3 };
        g_gl.GenVertexArrays(1, &g_res.vao);
        g_gl.GenBuffers(1, &g_res.vbo);
        g_gl.BindVertexArray(g_res.vao);
        g_gl.BindBuffer(kArrayBuffer, g_res.vbo);
        g_gl.BufferData(kArrayBuffer, sizeof triangle, triangle, kStaticDraw);
        g_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        g_gl.EnableVertexAttribArray(0);
        g_gl.ActiveTexture(kTexture0);
        CreateDetailTexture();
        for (Target& focus : g_res.focus)
            MakeTarget(focus, 1, 1, kR32f, kRed, GL_FLOAT);
    }
    if (w != g_res.width || h != g_res.height) {
        g_gl.ActiveTexture(kTexture0);
        if (!MakeSizedTargets(w, h)) {
            g_res.failed = true;
            return false;
        }
    }
    return true;
}

// ---- Copies out of the window ---------------------------------------------------------------
bool CaptureColor()
{
    while (g_res.colorMode >= 0) {
        if (g_res.colorMode == 0)
            g_res.colorMode = 1;
        if (!g_res.colorVerified)
            DrainErrors();
        g_gl.BindFramebuffer(kReadFramebuffer, 0);
        g_gl.ReadBuffer(GL_BACK);
        if (g_res.colorMode == 1) {
            g_gl.BindFramebuffer(kDrawFramebuffer, g_res.captureFbo);
            g_gl.BlitFramebuffer(0, 0, g_res.width, g_res.height, 0, 0, g_res.width, g_res.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else {
            g_gl.ActiveTexture(kTexture0);
            g_gl.BindTexture(GL_TEXTURE_2D, g_res.color);
            g_gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, g_res.width, g_res.height);
        }
        if (g_res.colorVerified || g_gl.GetError() == GL_NO_ERROR) {
            if (!g_res.colorVerified)
                Log("postfx: the window's image is read with a %s", g_res.colorMode == 1 ? "blit" : "texture copy");
            g_res.colorVerified = true;
            return true;
        }
        g_res.colorMode = g_res.colorMode == 1 ? 2 : -1;
    }
    Log("postfx: the window's image can't be read; shaders off");
    g_res.failed = true;
    return false;
}

// Copies the window's depth buffer into the texture attached to fbo (one of ours).
bool CaptureDepth(GLuint fbo, GLuint tex)
{
    while (g_res.depthMode >= 0) {
        if (g_res.depthMode == 0)
            g_res.depthMode = 1;  // the textures were made for it
        if (!g_res.depthVerified)
            DrainErrors();
        g_gl.BindFramebuffer(kReadFramebuffer, 0);
        if (g_res.depthMode <= 2) {
            g_gl.BindFramebuffer(kDrawFramebuffer, fbo);
            g_gl.BlitFramebuffer(0, 0, g_res.width, g_res.height, 0, 0, g_res.width, g_res.height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        } else {
            g_gl.ActiveTexture(kTexture0);
            g_gl.BindTexture(GL_TEXTURE_2D, tex);
            g_gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, g_res.width, g_res.height);
        }
        if (g_res.depthVerified || g_gl.GetError() == GL_NO_ERROR) {
            if (!g_res.depthVerified)
                Log("postfx: the depth buffer is read with a %s", g_res.depthMode <= 2 ? "blit" : "texture copy");
            g_res.depthVerified = true;
            return true;
        }
        if (++g_res.depthMode > 3) {
            g_res.depthMode = -1;
            Log("postfx: the depth buffer can't be read; AO, shadows, dirt detail, sky, DOF and motion blur are off");
            return false;
        }
        MakeDepthTextures();
        tex = fbo == g_res.captureFbo ? g_res.depth : g_res.terrainDepth;
    }
    return false;
}

// ---- Drawing helpers --------------------------------------------------------------------------
struct Matrices {
    float  proj[16] = {}, invProj[16] = {};
    float  texel[2] = {};
};
Matrices g_m;

Program& Use(ProgramId id)
{
    Program& p = g_res.programs[id];
    g_gl.UseProgram(p.id);
    g_gl.UniformMatrix4fv(p["u_proj"], 1, GL_FALSE, g_m.proj);
    g_gl.UniformMatrix4fv(p["u_invProj"], 1, GL_FALSE, g_m.invProj);
    g_gl.Uniform2f(p["u_texel"], g_m.texel[0], g_m.texel[1]);
    g_gl.Uniform1f(p["u_skyDepth"], 0.999999f);
    return p;
}

void Bind(Program& p, const char* name, int unit, GLuint tex)
{
    g_gl.ActiveTexture(kTexture0 + unit);
    g_gl.BindTexture(GL_TEXTURE_2D, tex);
    if (g_gl.BindSampler)
        g_gl.BindSampler(unit, 0);
    g_gl.Uniform1i(p[name], unit);
}

void Draw(const Target* target, int w, int h)
{
    g_gl.BindFramebuffer(kDrawFramebuffer, target ? target->fbo : 0);
    g_gl.Viewport(0, 0, w, h);
    g_gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

void Draw(const Target& target) { Draw(&target, target.w, target.h); }

void Vec(Program& p, const char* name, Vec3 v)
{
    g_gl.Uniform3f(p[name], static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
}

// ---- Matrix helpers (column-major, double) ----------------------------------------------------
bool Invert(const double m[16], double out[16])
{
    double inv[16];
    inv[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8]  =  m[4] * m[9]  * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9]  * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9]  = -m[0] * m[9]  * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] =  m[0] * m[9]  * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2]  =  m[1] * m[6]  * m[15] - m[1] * m[7]  * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7]  - m[13] * m[3] * m[6];
    inv[6]  = -m[0] * m[6]  * m[15] + m[0] * m[7]  * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7]  + m[12] * m[3] * m[6];
    inv[10] =  m[0] * m[5]  * m[15] - m[0] * m[7]  * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7]  - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5]  * m[14] + m[0] * m[6]  * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6]  + m[12] * m[2] * m[5];
    inv[3]  = -m[1] * m[6]  * m[11] + m[1] * m[7]  * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9]  * m[2] * m[7]  + m[9]  * m[3] * m[6];
    inv[7]  =  m[0] * m[6]  * m[11] - m[0] * m[7]  * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8]  * m[2] * m[7]  - m[8]  * m[3] * m[6];
    inv[11] = -m[0] * m[5]  * m[11] + m[0] * m[7]  * m[9]  + m[4] * m[1] * m[11] - m[4] * m[3] * m[9]  - m[8]  * m[1] * m[7]  + m[8]  * m[3] * m[5];
    inv[15] =  m[0] * m[5]  * m[10] - m[0] * m[6]  * m[9]  - m[4] * m[1] * m[10] + m[4] * m[2] * m[9]  + m[8]  * m[1] * m[6]  - m[8]  * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-30)
        return false;
    for (int i = 0; i < 16; ++i)
        out[i] = inv[i] / det;
    return true;
}

void ToFloat(const double* in, float* out, int n)
{
    for (int i = 0; i < n; ++i)
        out[i] = static_cast<float>(in[i]);
}

// A view matrix's rotation applied to a world direction, and back.
Vec3 WorldToEye(const double v[16], Vec3 w)
{
    return { v[0] * w.x + v[4] * w.y + v[8] * w.z, v[1] * w.x + v[5] * w.y + v[9] * w.z, v[2] * w.x + v[6] * w.y + v[10] * w.z };
}
Vec3 EyeToWorld(const double v[16], Vec3 e)
{
    return { v[0] * e.x + v[1] * e.y + v[2] * e.z, v[4] * e.x + v[5] * e.y + v[6] * e.z, v[8] * e.x + v[9] * e.y + v[10] * e.z };
}
Vec3 CameraPosition(const double v[16])
{
    return EyeToWorld(v, Vec3{ -v[12], -v[13], -v[14] });
}

Vec3 WorldUp()
{
    const Config& cfg = GetConfig();
    return Normalize(Vec3{ cfg.worldUp[0], cfg.worldUp[1], cfg.worldUp[2] });
}

const double* BestRawView()
{
    return g_frame.haveView ? g_frame.raw : (g_haveLastRaw ? g_lastRaw : nullptr);
}

// The game's sun from a light uniform in its eye space.
bool SunFromEye(const double raw[16], const float* eye, Vec3& sun)
{
    const Vec3 e{ eye[0], eye[1], eye[2] };
    if (Length(e) < 1e-6)
        return false;
    sun = Normalize(EyeToWorld(raw, e)) * (GetConfig().invertLight ? -1.0 : 1.0);
    return true;
}

timeofday::Sky CurrentSky()
{
    const Config& cfg = GetConfig();
    return cfg.timeOfDay >= 0 ? timeofday::Compute(cfg.timeOfDay, g_gameSun, WorldUp(), cfg.nightBrightness)
                              : timeofday::Neutral(g_gameSun, WorldUp());
}

// What lights the scene: the game's sun, or ours when the time of day turns it.
Vec3 SceneLight(const timeofday::Sky& sky)
{
    const Config& cfg = GetConfig();
    return (cfg.timeOfDay >= 0 && cfg.moveSun) ? sky.light : g_gameSun;
}

// ---- The sun's camera for the shadow map ---------------------------------------------------
// A square of 2 x ShadowDistance around (and mostly ahead of) the camera, seen straight along
// the light. Its centre moves in whole texels, so shadow edges don't crawl as the camera moves.
double UnitsPerMetre();  // (the game's unit, measured from your bike's wheelbase: below)
constexpr double kShadowDepth = 100.0;  // game units on either side of the centre, along the light, at least

void SetupShadowFrame()
{
    const Config& cfg = GetConfig();
    g_shadow.setup = true;
    g_shadow.usable = false;
    if (!cfg.shadowMap || cfg.shadows <= 0.0 || !g_frame.haveView || (!g_haveGameSun && cfg.timeOfDay < 0))
        return;
    const Vec3 up = WorldUp();
    const Vec3 light = Normalize(SceneLight(CurrentSky()));
    if (Dot(light, up) < 0.05)
        return;  // the light is on the horizon (or under it): shadows would be endless
    const double* V = g_frame.rendered;
    Vec3 ahead = EyeToWorld(V, Vec3{ 0, 0, -1 });
    ahead = ahead - up * Dot(ahead, up);
    ahead = Length(ahead) > 1e-3 ? Normalize(ahead) : Vec3{};
    // (ShadowDistance is in metres; the game's unit is feet.) Deep enough along the light for what
    // stands in the square to cast into it, however far it reaches.
    const double half = cfg.shadowDistance * UnitsPerMetre();
    const double depth = std::fmax(kShadowDepth, 2.0 * half);
    const Vec3 camera = CameraPosition(V);

    const Vec3 z = light;  // the sun's camera looks down -z, at the scene
    const Vec3 side = std::fabs(Dot(z, up)) > 0.99 ? Vec3{ 1, 0, 0 } : up;
    const Vec3 x = Normalize(Cross(side, z));
    const Vec3 y = Cross(z, x);
    // The sun's view, centred on the camera; each cascade a square in it.
    const double ox = Dot(camera, x), oy = Dot(camera, y), oz = Dot(camera, z);
    const double lightView[16] = { x.x, y.x, z.x, 0,  x.y, y.y, z.y, 0,  x.z, y.z, z.z, 0,  -ox, -oy, -oz, 1 };
    double invView[16];
    if (!AffineInverse(V, invView))
        return;
    MatMul(lightView, invView, g_shadow.toLight);
    // From 12 m around you out to the shadow distance, each about the same times bigger than the
    // last; each mostly ahead of the camera. Their centres move in whole texels of their own, so
    // shadow edges don't crawl as the camera moves.
    const double tile = ShadowMapSize(cfg) / 2.0;
    const double nearest = std::fmin(half, 12.0 * UnitsPerMetre());
    for (int c = 0; c < kCascades; ++c) {
        const double h = nearest * std::pow(half / nearest, double(c) / (kCascades - 1));
        const Vec3 centre = camera + ahead * (h * 0.45);
        const double texel = 2.0 * h / tile;
        const double cx = std::floor(Dot(centre, x) / texel) * texel - ox;
        const double cy = std::floor(Dot(centre, y) / texel) * texel - oy;
        g_shadow.cascadeHalf[c] = h;
        g_shadow.cascadeX[c] = cx;
        g_shadow.cascadeY[c] = cy;
        const float projection[16] = { static_cast<float>(1.0 / h), 0, 0, 0,  0, static_cast<float>(1.0 / h), 0, 0,
                                       0, 0, static_cast<float>(-1.0 / depth), 0,
                                       static_cast<float>(-cx / h), static_cast<float>(-cy / h), 0, 1 };
        std::copy(projection, projection + 16, g_shadow.cascadeProjection[c]);
    }
    g_shadow.half = half;
    g_shadow.depth = depth;
    g_shadow.usable = true;
}

// ---- The headlight: on the bike's front plate ----------------------------------------------
// The rainbow headlight's colour now (Config::headlightColor 2): every hue in turn, once round
// every kRainbowSeconds, at full brightness (its strongest channel 1) with a little white in it, so
// even the blue and the purple still light the track.
constexpr double kRainbowSeconds = 4.0;
Vec3 RainbowColor(double seconds)
{
    const double h = std::fmod(seconds / kRainbowSeconds, 1.0) * 6.0;
    const double x = 1.0 - std::fabs(std::fmod(h, 2.0) - 1.0);
    Vec3 c;
    switch (static_cast<int>(h)) {
    case 0:  c = { 1, x, 0 }; break;
    case 1:  c = { x, 1, 0 }; break;
    case 2:  c = { 0, 1, x }; break;
    case 3:  c = { 0, x, 1 }; break;
    case 4:  c = { x, 0, 1 }; break;
    default: c = { 1, 0, x }; break;
    }
    constexpr double kWhite = 0.12;
    return { kWhite + (1.0 - kWhite) * c.x, kWhite + (1.0 - kWhite) * c.y, kWhite + (1.0 - kWhite) * c.z };
}
Vec3 TransformPoint(const double m[16], Vec3 p)
{
    return { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
             m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
}
Vec3 TransformDirection(const double m[16], Vec3 d)
{
    return { m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z, m[2] * d.x + m[6] * d.y + m[10] * d.z };
}

// ---- Skeletons: your rider and your bike ------------------------------------------------------
// Every skinned draw of the frame with its bones and the u_modelview it's drawn with: riders are
// drawn with the bare view (their bones are world positions), bikes with a model of their own (their
// bones are in it). Both end up in world space here.
constexpr int kSkeletonBones = 32;
struct SkeletonRecord {
    unsigned program = 0;
    float    viewModel[16] = {};
    int      bones = 0, torso = -1, hips = -1;
    bool     rider = false, transposed = false;
    float    position[kSkeletonBones * 3] = {};
    float    rotation[kSkeletonBones * 9] = {};
    float    centre[kSkeletonBones * 3] = {};  // each bone's rest position (u_bone_c): which joint it is
};
std::vector<SkeletonRecord> g_skeletons;

// Every rider out to kRiderPointRange (not just those near the camera, as above): where their hips
// are and how their torso is turned, for a headlight on bikes that aren't found by their wheels (a
// model whose wheels haven't been seen turning yet: everyone waiting at the gate).
constexpr float kRiderPointRange = 500.0f;  // game units (~150 m)
struct RiderPoint {
    float viewModel[16];
    float hips[3], torso[9];
    bool  transposed;
};
std::vector<RiderPoint> g_riderPoints;
std::vector<Vec3>       g_lastRiderHips;  // last frame's riders' hips (world), for NearAnyRiderEye

// World transform of a record's model (camera -> world times its View * Model).
bool RecordWorld(const SkeletonRecord& rec, double world[16])
{
    double invRaw[16], vm[16];
    if (!AffineInverse(g_frame.raw, invRaw))
        return false;
    for (int i = 0; i < 16; ++i)
        vm[i] = rec.viewModel[i];
    MatMul(invRaw, vm, world);
    return true;
}

Vec3 BonePosition(const SkeletonRecord& rec, const double world[16], int b)
{
    return TransformPoint(world, Vec3{ rec.position[b * 3], rec.position[b * 3 + 1], rec.position[b * 3 + 2] });
}

// The bone's own axes in world space (columns of its rotation, turned by the model's).
void BoneRotation(const SkeletonRecord& rec, const double world[16], int b, Vec3 axes[3])
{
    const float* r = rec.rotation + b * 9;
    for (int c = 0; c < 3; ++c) {
        const Vec3 col = rec.transposed ? Vec3{ r[c], r[3 + c], r[6 + c] } : Vec3{ r[c * 3], r[c * 3 + 1], r[c * 3 + 2] };
        axes[c] = TransformDirection(world, col);
    }
}

// This frame's objects near the camera (View * Model as the game sent it), and what the bike
// tracker taught about your bike while it was locked in first person: which program draws it,
// and where the rider's eyes and the rider sit on it, in its own space. In third person (or
// with the camera mod off) the bike is the object that carries your rider in that same place.
struct SceneObject { unsigned program; float viewModel[16]; };
std::vector<SceneObject> g_objects;

struct BikeModel {
    bool     learned = false;             // the bike tracker taught it this session
    bool     loaded = false;              // read from the ini's [Learned] section
    unsigned program = 0;                 // 0: any
    // MX Simulator's bikes: the origin sits about 1.5 m under the ground, the bike faces -Z.
    Vec3     eye{ 0.0, 3.05, -1.0 };      // the rider's eyes at rest, in the bike's space
    Vec3     rider{ 0.0, 2.4, -0.85 };    // the middle of the rider and bike, in the bike's space
    Vec3     forward{ 0.0, 0.0, -1.0 };   // the bike model's own axes
    Vec3     up{ 0.0, 1.0, 0.0 };
    double   samples = 0;                 // seconds learnt
    double   savedAt = -1e9;              // when it was last written to the ini
};
BikeModel g_bikeModel;
bool      g_thirdPerson = false;          // the camera is away from your rider (with some hysteresis)

// Your bike this frame (world transform), however it was found.
struct BikeNow { bool found = false; int how = 0; double model[16] = {}; };
// For the trace (F11): which of this frame's objects FindBike took (-1: none), how (0 followed, 1
// searched), how far your rider was from where it sits on it (m), and the headlight's placement.
struct BikeTrace { int object = -1, pass = -1, objects = 0; double carry = -1; int headlightMode = -1; Vec3 headlight; bool lit = false; };
BikeTrace g_bikeTrace;
BikeNow g_bikeNow;

// ---- Your bike's wheels ----------------------------------------------------------------------
// Every part drawn near your rider (rigid objects, and every bone of skinned ones) is followed
// from frame to frame by its place in the draw order. Seen from your bike (or, until the bike is
// found, from your rider), a wheel turns about an axle across the bike: each frame's turn gives
// that axle (the line that stays put), wherever the wheel model's own origin is (MX Simulator's
// models have theirs far below the ground). The two parts that keep turning, a wheelbase apart,
// are your wheels; the tires touch the ground a wheel's radius below their axles.
struct PartTrack {
    bool     used = false, seenNow = false;
    unsigned long long key = 0;   // program, kind, place in the draw order, bone
    unsigned program = 0;
    int      kind = 0, bone = 0;  // rigid object (0) or skinned (1), and which bone
    int      reference = -1;      // what it was seen from last: 0 your bike, 1 your rider
    int      localAxis = -1;      // which of its own axes is the axle (the one that doesn't turn)
    Vec3     axleLocal;           // its axle in its own model (a wheel turns about the same line in
    double   localWeight = 0;     // it, whatever the bike lock locks onto): averaged, steady
    int      localRejected = 0;
    double   rel[16] = {};        // last frame's transform, seen from that
    double   lastSeen = -1;
    Vec3     axleEst;             // its axle (seen from the reference): on the bike's centreline,
    double   axleWeight = 0;      // a steady average that ignores the odd wild estimate
    int      rejected = 0;        // wild estimates in a row (many: the average was the wild one, start over)
    double   spin = 0;            // radians turned in about the last second
    double   roll = 0;            // the same, about the reference's across (signed): a wheel rolling turns
                                  // that way on end; steering turns about another line, back and forth
    double   total = 0;           // radians turned in all
    Vec3     axes[3];             // its own axes (world) this frame: they turn with it
    Vec3     origin;              // its origin (world) this frame
    double   world[16] = {};      // its whole transform (world) this frame
    // A rigid draw (one bone at its origin): its u_modelview as the game sent it is all of its
    // transform, so the shadow map can recognise its draw call by it (see OwnWheelCast).
    bool     drawnAsIs = false;
    float    viewModel[16] = {};
};
PartTrack g_partTracks[192];

struct Wheel {
    bool   on = false, logged = false;
    int    track = -1;
    Vec3   up{ 0, 1, 0 };     // across its axle, toward the sky (leans with the bike)
    Vec3   fromRider;         // where it was against your rider (along, across, up)
    bool   haveFromRider = false;
    unsigned long long trackKey = 0;  // the part it is (its slot may be reused once it's gone)
    bool   fromPair = false;  // its part was found with the other wheel's (both spinning, a wheelbase apart),
                              // not on its own (that can be the sprocket or a brake disc on the same axle)
    double lateral = 0;       // along its axle, from its own axle point to the bike's centre plane
    bool   haveLateral = false;
    unsigned long long lateralKey = 0;  // the part lateral was learnt for (another part: another offset)
    double lateralWeight = 0;           // seconds of it learnt upright
    bool   lateralFromBike = false;     // learnt from your bike measured precisely (first person)
    Vec3   axleRef[2];        // the axle seen from your bike / your rider: kept through dropouts
    bool   haveAxle[2] = { false, false };
    double missing = 0;       // seconds its part hasn't been seen
    Vec3   axle;              // world
    Vec3   axes[3];           // the wheel's own axes (world): they turn with it
    int    axleAxis = 0;      // which of them is the axle
    double radius = 0.35;     // m, to the outside of the tire
    double stretch = 1.0;     // tire dirt: how much of its pattern turns with it (see u_wheelStretch)
    bool   farOff = false;    // its part was far from where it sits on your bike (pulled back)
    double unseen = 0;        // seconds its part hasn't been seen (the game not drawing it)
    double seenFor = 0;       // seconds it has been seen on end
    bool   standIn = false;   // a shape of ours stands in for it in the shadow map (DrawStandIns): on
                              // once it's gone unseen a moment, off only once it's been seen half a
                              // second on end (a part blipping into view for a frame took it away)
    int    maskParts = 0;     // drawn by the game this frame and yours: its parts go into the tire dirt's mask
    float  maskViewModel[kWheelMaskParts][16] = {};
    Vec3   maskAxleLocal[kWheelMaskParts];
    Vec3   contact;           // world
    Vec3   lastContact;       // world, the last one stamped
    int    trailSkipped = 0;  // frames on end its contact was off the trail sideways (not stamped)
    bool   haveLast = false;  // the last contact stamped into the tire marks
    double lastU = 0, lastV = 0;
    double runArc = 0;        // how far it has pressed in its current run of ruts, in the mark
                              // map's texels of trail, counted from the run's own first contact
                              // (the turns at a run's ends are read off this - see kMarkRunFade
                              // and u_arcA in postfx_shaders.h)
    double off = 0.0;         // seconds it has been off the tracker (the rut keeps joining up)
    // Where its tire is on the ground, in the world (see the ground field in the tire marks):
    // the marks' own test is the terrain's depth, which only exists where the game drew the
    // wheel, so this answers for the contacts the screen can't see: how fast the contact's path
    // and, steadier, your bike's own height are falling.
    double heightVel = 0;     // its contact's velocity and acceleration, game units/s and s^2, from
    double heightAccel = 0;   // a least-squares fit over kAirFitWindow of its positions: differencing
    MotionFit fit;            // them twice amplifies the contact's jitter into whole gravities, and
    double fallG = 0;         // that acceleration along WorldUp, in g (negative falling), and the
                              // deeper of it and your bike's own - what the fall's score is built
                              // from, since a bike is rigid and its wheels aren't
    double airScore = 0;      // how long a free fall has been read, 0 to 1 (0: none at all, 1.0:
                              // half a second of it at the game's gravity); it gains the fall's
                              // depth times kAirScoreGain and loses kAirScoreDrain a second, or
                              // kAirLandedDrain once nothing is falling any more
    double airPeak = 0;       // the most its hold got in this flight: what's back on the ground has
                              // to have lost half of it, which is the difference between a landing
                              // and the state flapping back through the lip it just left
    bool   grounded = true;   // where this frame says its tire is: what the marks hold to
    double gap = 0;           // how far this frame's contact sat above the ground near here, and
    bool   gapKnown = false;  // whether anything was known near there (the air readout below)
    // Where it sits on your bike (the bike's own space), learnt while its part is seen turning.
    // The game doesn't draw a wheel that is out of view (in first person the rear one, and often
    // the front, are under you), and the part can be lost for other reasons; your bike itself is
    // found all along, so the wheel is then placed from it.
    Vec3   onBike;
    Vec3   axisOnBike;        // its axle's direction there
    double onBikeWeight = 0;  // seconds learnt this session (or kLoadedWheelWeight when read from the ini)
    int    onBikeRejected = 0;  // samples in a row far from what was learnt
    bool   fromBike = false;  // placed from your bike this frame (its part wasn't seen)
    double roll = 0;          // placed from your bike: how far it has rolled (rad), so its dirt keeps turning
    double savedWeight = -1;  // onBikeWeight when it was last written to the ini
    bool   settled = false;   // learnt (or read from the ini): kept as it is, so it can't creep away
    // For the shadow map (OwnWheelCast): its part as the game draws it this frame, and its axle in
    // its own model (the point and which of its axes), to put that model on the axle where the
    // wheel sits on your bike when the game doesn't draw it.
    bool     drawnNow = false;
    unsigned partProgram = 0;
    float    drawnViewModel[16] = {};
    Vec3     partAxleLocal;
    int      partLocalAxis = -1;
};
Wheel  g_wheels[2];            // 0 rear, 1 front
// A wheel read from the ini counts as this many seconds learnt: enough to place it from your bike,
// little enough that what it's seen doing this session soon takes over.
constexpr double kLoadedWheelWeight = 1.0;
constexpr double kWheelOnBikeMin = 0.5;  // seconds learnt before it's placed from your bike
bool WheelPlausibleOnBike(int i, Vec3 point);

// Game units per metre. MX Simulator measures in feet (a bike's 1.5 m wheelbase comes out ~4.9
// units): taken from your wheels once they're found (wheelbase / 1.48 m), feet until then.
constexpr double kWheelbase = 1.48;
double g_unitsPerMetre = 3.2808;
double UnitsPerMetre() { return g_unitsPerMetre; }
bool   g_unitsMeasured = false;
bool   g_unitsLocked = false;     // measured steadily for a while: never changes again (the rut map's
int    g_unitsAgreeing = 0;       // scale hangs on it)
int    g_wheelLogLines = 0;       // lost / found again lines written (capped, per session)
constexpr int kWheelLogLinesMax = 200;
bool   g_wheelsWereOn = false;

// A one-off report after riding a while (MXS_REMASTERED_bike.txt next to the log): what the wheel search
// saw under your rider, for bug reports.
struct BikeReport {
    double riding = 0;         // seconds ridden (moving) this session
    bool   written = false, capturing = false;
    int    framesCaptured = 0;
    std::vector<std::string> lines;
};
BikeReport g_report;
char g_tireStatus[64] = "TIRES: WAITING FOR YOUR RIDER";
double g_movingWithoutWheels = 0;
bool   g_loggedNoWheels = false;

// Airborne: the bike falls freely (its height accelerates downwards at about 1 g).
struct Air {
    bool   haveLast = false, haveVelocity = false, airborne = false;
    double lastHeight = 0, velocity = 0, accel = 0, freeFall = 0;
};
Air g_air;

struct OwnRider {
    bool   seen = false;           // found this frame
    Vec3   position;               // world: the middle of the rider and bike
    double nearestBone = 1e9;      // m from the camera to its nearest bone (first person: tiny)
    bool   haveLast = false;
    Vec3   last;
    Vec3   velocity;               // smoothed, m/s
    bool   haveHeading = false;
    Vec3   heading;                // unit, level: the way the bike points
    bool   haveLean = false;
    Vec3   lean;                   // unit: the rider's (and so the bike's) up, leaning in corners
    Vec3   hips;                   // world: on the seat, in the bike's centre plane
    Vec3   moved;                  // world: how far it moved since last frame
    bool   haveTorso = false;      // this frame: its torso bone's whole transform (world), which
    double torsoWorld[16] = {};    // carries the head (and the helmet the game draws on it)
    bool   haveJoints = false;     // this frame: where its arms and legs join the body (world), left
    Vec3   shoulder[2], hip[2];    // and right, from the limb bones (see FindJoints)
    Vec3   inEye;                  // position in the game camera's eye space (the raw view's)
};
OwnRider g_rider;
void PartWorld(const SkeletonRecord& rec, const double world[16], int b, double out[16]);

// Where a rider's arms and legs join its body. Each bone turns about its rest position (u_bone_c),
// so that point, moved, is its joint. The limb bones: four on each side of the body's midline (by
// their rest positions); of each side's four, the two highest at rest are the arm, the other two the
// leg. The shoulder is the arm bone nearer the midline (the elbow is further out), the hip the leg
// bone nearer the torso bone (the knee is a thigh's length away). The two sides needn't mirror
// exactly. False unless it's that, with the shoulders above the hips along the torso's up.
bool FindJoints(const SkeletonRecord& rec, const double world[16], Vec3 shoulder[2], Vec3 hip[2])
{
    if (rec.torso < 0)
        return false;
    double halfSpan = 0;
    for (int b = 0; b < rec.bones; ++b)
        halfSpan = std::fmax(halfSpan, std::fabs(double(rec.centre[b * 3])));
    if (halfSpan < 1e-3)
        return false;
    const float* torsoRest = rec.centre + rec.torso * 3;
    for (int side = 0; side < 2; ++side) {
        std::vector<int> limb;
        for (int b = 0; b < rec.bones; ++b) {
            const float x = rec.centre[b * 3];
            if (side ? x > 0.05 * halfSpan : x < -0.05 * halfSpan)
                limb.push_back(b);
        }
        if (limb.size() != 4)
            return false;
        std::sort(limb.begin(), limb.end(), [&](int x, int y) { return rec.centre[x * 3 + 1] > rec.centre[y * 3 + 1]; });
        const auto Out = [&](int b) { return std::fabs(double(rec.centre[b * 3])); };
        const auto FromTorso = [&](int b) {
            const float* c = rec.centre + b * 3;
            return Length(Vec3{ c[0] - torsoRest[0], c[1] - torsoRest[1], c[2] - torsoRest[2] });
        };
        const int shoulderBone = Out(limb[0]) <= Out(limb[1]) ? limb[0] : limb[1];
        const int hipBone = FromTorso(limb[2]) <= FromTorso(limb[3]) ? limb[2] : limb[3];
        shoulder[side] = BonePosition(rec, world, shoulderBone);
        hip[side] = BonePosition(rec, world, hipBone);
    }
    const Vec3 spine = (shoulder[0] + shoulder[1]) * 0.5 - (hip[0] + hip[1]) * 0.5;
    const double length = Length(spine);
    if (length < 1e-4)
        return false;
    Vec3 axes[3];
    BoneRotation(rec, world, rec.torso, axes);
    return Dot(spine, Normalize(axes[1])) > 0.3 * length;
}

// Your rider's joints on its torso bone (its own frame): they don't move on it (the arms and legs
// turn about them), so once found they're kept there and the torso shape is built from them every
// frame, never switching between two shapes. Averaged, ignoring an odd frame far from the rest.
struct Joints {
    bool   have = false, logged = false;
    int    samples = 0;
    Vec3   shoulder[2], hip[2];   // in the torso bone's frame
};
Joints g_joints;

void LearnJoints(const double torsoWorld[16], const Vec3 shoulder[2], const Vec3 hip[2])
{
    double inv[16];
    if (!AffineInverse(torsoWorld, inv))
        return;
    Vec3 local[4] = { TransformPoint(inv, shoulder[0]), TransformPoint(inv, shoulder[1]), TransformPoint(inv, hip[0]),
                      TransformPoint(inv, hip[1]) };
    Joints& j = g_joints;
    if (j.samples >= 60 || replay::Current() != replay::Mode::Off)
        return;  // known: kept as it is (a replay's rider can be someone else)
    Vec3* kept[4] = { &j.shoulder[0], &j.shoulder[1], &j.hip[0], &j.hip[1] };
    if (j.samples > 30) {
        for (int i = 0; i < 4; ++i)
            if (Length(local[i] - *kept[i]) > 0.15 * g_unitsPerMetre)
                return;  // an odd frame
    }
    const double k = j.samples == 0 ? 1.0 : 0.1;
    for (int i = 0; i < 4; ++i)
        *kept[i] = *kept[i] + (local[i] - *kept[i]) * k;
    ++j.samples;
    if (j.samples >= 10)
        j.have = true;
    if (j.have && !j.logged) {
        j.logged = true;
        const double u = g_unitsPerMetre;
        Log("postfx: your rider's joints on its torso bone (m): shoulders %.2f %.2f %.2f / %.2f %.2f %.2f, hips %.2f %.2f %.2f / "
            "%.2f %.2f %.2f",
            j.shoulder[0].x / u, j.shoulder[0].y / u, j.shoulder[0].z / u, j.shoulder[1].x / u, j.shoulder[1].y / u,
            j.shoulder[1].z / u, j.hip[0].x / u, j.hip[0].y / u, j.hip[0].z / u, j.hip[1].x / u, j.hip[1].y / u, j.hip[1].z / u);
    }
}

// The dab indicator (dab.cpp) needs your speed when the effects are off: FindOwnRider is then run
// at the end of the frame (see FrameEnd).
bool   g_riderThisFrame = false;  // FindOwnRider ran this frame
double g_riderTime = -1;          // when it last ran

void FindOwnRider(double dt)
{
    OwnRider& r = g_rider;
    g_riderThisFrame = true;
    g_riderTime = Seconds();
    r.seen = false;
    r.haveTorso = false;
    r.haveJoints = false;
    if (g_skeletons.empty() || !g_frame.haveView)
        return;
    const Vec3 eye = CameraPosition(g_frame.raw);
    // Your rider: the one whose nearest bone is nearest the camera.
    int best = -1;
    double bestDistance = 40.0;  // further than this, it isn't you (the far chase camera sits ~12 m back)
    std::vector<Vec3> centroids(g_skeletons.size());
    for (size_t i = 0; i < g_skeletons.size(); ++i) {
        const SkeletonRecord& rec = g_skeletons[i];
        double world[16];
        if (!rec.rider || !RecordWorld(rec, world))
            continue;
        Vec3 sum;
        for (int b = 0; b < rec.bones; ++b) {
            const Vec3 bone = BonePosition(rec, world, b);
            sum = sum + bone;
            const double d = Length(bone - eye);
            if (d < bestDistance) {
                bestDistance = d;
                best = static_cast<int>(i);
            }
        }
        centroids[i] = sum / rec.bones;
    }
    if (best < 0)
        return;
    // In first person the camera is in your rider's head: the nearest one is you. In third person it
    // needn't be (someone between the camera and you, or close behind you, took your wheels, tire
    // marks and dirt): there your rider is followed from frame to frame, and found again as the one
    // the camera looks at (the chase camera follows you), not the nearest.
    if (bestDistance > 1.4) {
        int pick = -1;
        if (r.haveLast) {
            const Vec3 expected = r.last + r.velocity * dt;
            double nearest = 0.6 * g_unitsPerMetre;
            for (size_t i = 0; i < g_skeletons.size(); ++i) {
                if (!g_skeletons[i].rider)
                    continue;
                const double d = Length(centroids[i] - expected);
                if (d < nearest) {
                    nearest = d;
                    pick = static_cast<int>(i);
                }
            }
        }
        if (pick < 0) {
            const Vec3 ahead = Normalize(EyeToWorld(g_frame.raw, Vec3{ 0, 0, -1 }));
            double straightest = -1.0;
            for (size_t i = 0; i < g_skeletons.size(); ++i) {
                if (!g_skeletons[i].rider || Length(centroids[i] - eye) > 40.0)
                    continue;
                const double along = Dot(Normalize(centroids[i] - eye), ahead);
                if (along > straightest) {
                    straightest = along;
                    pick = static_cast<int>(i);
                }
            }
        }
        if (pick >= 0)
            best = pick;
    }
    Vec3 sum;
    int n = 0;
    for (size_t i = 0; i < g_skeletons.size(); ++i) {
        if (g_skeletons[i].rider && Length(centroids[i] - centroids[best]) < 1.2) {
            sum = sum + centroids[i];
            ++n;
        }
    }
    const Vec3 position = sum / n;
    {
        const SkeletonRecord& rec = g_skeletons[best];
        double world[16];
        const bool haveWorld = RecordWorld(rec, world);
        r.hips = rec.hips >= 0 && haveWorld ? BonePosition(rec, world, rec.hips) : position;
        if (rec.torso >= 0 && haveWorld) {
            PartWorld(rec, world, rec.torso, r.torsoWorld);
            r.haveTorso = true;
        }
        if (r.haveTorso && FindJoints(rec, world, r.shoulder, r.hip))
            LearnJoints(r.torsoWorld, r.shoulder, r.hip);
        r.haveJoints = r.haveTorso && g_joints.have;
        if (r.haveJoints)
            for (int side = 0; side < 2; ++side) {
                r.shoulder[side] = TransformPoint(r.torsoWorld, g_joints.shoulder[side]);
                r.hip[side] = TransformPoint(r.torsoWorld, g_joints.hip[side]);
            }
    }
    r.moved = r.haveLast ? position - r.last : Vec3{};
    r.seen = true;
    {
        // (The chosen rider's nearest bone: in third person it needn't be the nearest one.)
        const SkeletonRecord& rec = g_skeletons[best];
        double world[16];
        r.nearestBone = bestDistance;
        if (RecordWorld(rec, world)) {
            double nearest = 1e9;
            for (int b = 0; b < rec.bones; ++b)
                nearest = std::fmin(nearest, Length(BonePosition(rec, world, b) - eye));
            r.nearestBone = nearest;
        }
    }
    if (r.haveLast && dt > 0.0) {
        const Vec3 step = position - r.last;
        if (Length(step) > 3.0 * g_unitsPerMetre) {  // a respawn or a camera cut: start over
            r.velocity = Vec3{};
            r.haveHeading = false;
        } else {
            const double a = 1.0 - std::exp(-dt * 6.0);
            r.velocity = r.velocity + (step / dt - r.velocity) * a;
        }
    }
    r.last = position;
    r.haveLast = true;
    r.position = position;
    r.inEye = TransformPoint(g_frame.raw, position);
    // First person: the camera is at your rider's head (its nearest bone ~0.7 m away); third
    // person: metres behind. Hysteresis, so it never flips back and forth.
    if (r.nearestBone > 2.0)
        g_thirdPerson = true;
    else if (r.nearestBone < 1.4)
        g_thirdPerson = false;
    // Which way the bike points, level: the way your rider's torso faces (known standing still,
    // and it turns with the bike in slides), or else the way it's travelling. Never up or down:
    // a drop onto the start gate or a landing would aim the light at the ground.
    const Vec3 worldUp = WorldUp();
    const auto Level = [&](Vec3 v, Vec3& out) {
        v = v - worldUp * Dot(v, worldUp);
        if (Length(v) < 1e-3)
            return false;
        out = Normalize(v);
        return true;
    };
    Vec3 target;
    bool haveTarget = false;
    double rate = 10.0;
    {
        const SkeletonRecord& rec = g_skeletons[best];
        double world[16];
        Vec3 axes[3];
        Vec3 forward;
        if (rec.torso >= 0 && RecordWorld(rec, world)) {
            BoneRotation(rec, world, rec.torso, axes);
            forward = axes[2] * -1.0;
        }
        if (rec.torso >= 0 && Level(forward, target)) {
            haveTarget = true;
            // The torso's up, without its lean forward / back: the bike's lean.
            Vec3 lean = axes[1] - target * Dot(axes[1], target);
            if (Length(lean) > 0.3 && Dot(lean, worldUp) > 0.3) {
                lean = Normalize(lean);
                const double a = r.haveLean ? 1.0 - std::exp(-dt * 12.0) : 1.0;
                r.lean = Normalize(r.lean + (lean - r.lean) * a);
                r.haveLean = true;
            }
        }
    }
    if (!haveTarget) {
        Vec3 flatVelocity;
        const double flatSpeed = Level(r.velocity, flatVelocity) ? Length(r.velocity - worldUp * Dot(r.velocity, worldUp)) : 0.0;
        if (flatSpeed > 3.0) {  // slower than this the heading is held (at the gate, after a crash)
            target = flatVelocity;
            haveTarget = true;
            rate = 8.0 * Clamp((flatSpeed - 3.0) / 3.0, 0.0, 1.0);
        }
    }
    if (haveTarget) {
        if (!r.haveHeading || Dot(r.heading, target) < 0.0) {
            r.heading = target;
            r.haveHeading = true;
        } else {
            r.heading = Normalize(r.heading + (target - r.heading) * (1.0 - std::exp(-dt * rate)));
        }
    }
}

// World position of the front plate, the bike's forward and its up. First person with the
// helmet cam: from the bike tracker, placed from where the rider's eyes sit on the bike.
// Otherwise (third person, helmet cam off, replays): from your rider's skeleton and direction
// of travel. Before either is known it rides on the game's camera.
int g_headlightMode = -1;  // how the headlight was last placed (for the log)

void NoteHeadlightMode(int mode, const Vec3& position)
{
    g_bikeTrace.headlightMode = mode;
    g_bikeTrace.headlight = position;
    g_bikeTrace.lit = true;
    if (mode == g_headlightMode)
        return;
    g_headlightMode = mode;
    static const char* const kModes[] = { "on your bike (bike tracker, first person)", "on your bike (found under your rider)",
                                          "on your rider (bike not found)", "on the camera (no rider found yet)",
                                          "on your bike where it was last found, carried along with your rider (lost for a moment)" };
    const Vec3 offset = position - CameraPosition(g_frame.raw);
    Log("postfx: headlight placed %s, %.2f m from the camera (rider's nearest bone %.2f m away; bike %s)", kModes[mode],
        Length(offset), g_rider.seen ? g_rider.nearestBone : -1.0,
        g_bikeModel.learned ? "learnt in first person" : "not learnt yet: ride in first person for a few seconds to teach it");
}

// The front plate on a bike, from its front axle (m): up the fork (raked back) and in front of it.
constexpr double kPlateAboveAxle  = 0.62;
constexpr double kPlateBehindAxle = 0.22;

// The plate on the bike, in the bike's own space.
//   First person: HeadlightForward / HeadlightHeight from your eyes, applied as they always have
//   been (in the game's units, which are feet, though the settings say metres: from the eyes the
//   light's start can't be seen, and the beam looks right, so it's kept exactly as it was).
//   Third person, where the bike is in view: on the front plate itself, up the fork from your front
//   axle (learnt on your bike), in metres. (From the eyes it sat 17 cm ahead of your rider's face.)
Vec3 PlateOnBike()
{
    const Config& cfg = GetConfig();
    const BikeModel& b = g_bikeModel;
    if (!g_thirdPerson)
        return b.eye + b.forward * cfg.headlightForward + b.up * cfg.headlightHeight;
    const double u = g_unitsPerMetre;
    const Wheel& front = g_wheels[1];
    if (front.onBikeWeight > 0) {
        Vec3 plate = front.onBike + b.up * (kPlateAboveAxle * u) - b.forward * (kPlateBehindAxle * u);
        plate.x = b.eye.x;  // (on the bike's centre line)
        return plate;
    }
    // Front axle not learnt yet: as far from the eyes as the plate usually is.
    return b.eye + b.forward * (0.33 * u) - b.up * (0.65 * u);
}

void PoseFromBike(const double model[16], Vec3& position, Vec3& forward, Vec3& up)
{
    position = TransformPoint(model, PlateOnBike());
    forward = Normalize(TransformDirection(model, g_bikeModel.forward));
    up = Normalize(TransformDirection(model, g_bikeModel.up));
}

// First person with the bike tracker locked: learn where the eyes and the rider sit on the bike.
void LoadLearnedBike()
{
    BikeModel& b = g_bikeModel;
    if (b.loaded)
        return;
    b.loaded = true;
    double v[3];
    if (ReadLearned("BikeEye", v))
        b.eye = Vec3{ v[0], v[1], v[2] };
    if (ReadLearned("BikeRider", v))
        b.rider = Vec3{ v[0], v[1], v[2] };
    ForgetLearned("FrontAxle");  // from an earlier version, in another space
    ForgetLearned("RearAxle");
    for (int i = 0; i < 2; ++i) {
        Wheel& w = g_wheels[i];
        if (ReadLearned(i ? "FrontWheelOnBike" : "RearWheelOnBike", v)) {
            if (!WheelPlausibleOnBike(i, Vec3{ v[0], v[1], v[2] })) {
                Log("postfx: your %s wheel: (%.2f, %.2f, %.2f) on your bike can't be right; learning it again", i ? "front" : "rear",
                    v[0], v[1], v[2]);
                ForgetLearned(i ? "FrontWheelOnBike" : "RearWheelOnBike");
                continue;
            }
            w.onBike = Vec3{ v[0], v[1], v[2] };
            w.axisOnBike = Normalize(Cross(b.forward, b.up));  // the bike's own across
            w.onBikeWeight = kLoadedWheelWeight;
            w.settled = true;
            w.savedWeight = kLoadedWheelWeight;
            Log("postfx: your %s wheel: at (%.2f, %.2f, %.2f) on your bike (learnt)", i ? "front" : "rear", v[0], v[1], v[2]);
        }
    }
    // Both wheels learnt: their wheelbase gives the game's unit from the start (feet, or metres in a
    // test scene; anything else isn't trusted).
    if (g_wheels[0].onBikeWeight > 0 && g_wheels[1].onBikeWeight > 0) {
        const double unit = Dot(g_wheels[1].onBike - g_wheels[0].onBike, b.forward) / kWheelbase;
        if (std::fabs(unit - 3.2808) < 0.15 * 3.2808 || std::fabs(unit - 1.0) < 0.15) {
            g_unitsPerMetre = std::fabs(unit - 3.2808) < 0.15 * 3.2808 ? 3.2808 : 1.0;
            g_unitsMeasured = true;
        }
    }
    Log("postfx: your bike: eyes at (%.2f, %.2f, %.2f) on it (%s)", b.eye.x, b.eye.y, b.eye.z,
        ReadLearned("BikeEye", v) ? "learnt" : "MX Simulator's usual place until it's learnt in first person");
}

void SaveLearnedBike(double now, bool force)
{
    BikeModel& b = g_bikeModel;
    if (!force && now - b.savedAt < 20.0)
        return;
    b.savedAt = now;
    const double eye[3] = { b.eye.x, b.eye.y, b.eye.z }, rider[3] = { b.rider.x, b.rider.y, b.rider.z };
    if (b.learned) {
        WriteLearned("BikeEye", eye);
        WriteLearned("BikeRider", rider);
    }
}

// First person with the bike tracker locked: learn where the eyes and the rider sit on the bike.
void LearnBike(const double model[16], const bike::Pose& pose, double dt)
{
    double inv[16];
    if (!AffineInverse(model, inv))
        return;
    BikeModel& b = g_bikeModel;
    const Vec3 eye = TransformPoint(inv, CameraPosition(g_frame.raw));
    const double a = b.learned ? 1.0 - std::exp(-dt / 3.0) : 1.0;  // a few seconds' average: no head bob
    b.eye = b.eye + (eye - b.eye) * a;
    if (g_rider.seen)
        b.rider = b.rider + (TransformPoint(inv, g_rider.position) - b.rider) * (b.learned ? a : 1.0);
    b.forward = Vec3{ pose.forward[0], pose.forward[1], pose.forward[2] };
    b.up = Vec3{ pose.up[0], pose.up[1], pose.up[2] };
    b.program = pose.program;
    b.samples += dt;
    if (!b.learned) {
        b.learned = true;
        Log("postfx: headlight learnt your bike (program %u): eyes at (%.2f, %.2f, %.2f) in its space", b.program, eye.x,
            eye.y, eye.z);
    }
}

// Third person (or the camera mod off): the object that carries your rider where the bike does.
// Once found it's followed from frame to frame (the rider leans and shifts on it, the bike
// doesn't jump).
bool   g_haveLastBike = false;
Vec3   g_lastBikeOrigin;
double g_lastBikeWorld[16] = {};
unsigned g_lastBikeProgram = 0;

bool FindBike(double model[16])
{
    if (!g_rider.seen || g_objects.empty()) {
        g_haveLastBike = false;
        return false;
    }
    double invRaw[16];
    if (!AffineInverse(g_frame.raw, invRaw))
        return false;
    const Vec3 worldUp = WorldUp();
    const BikeModel& b = g_bikeModel;
    double bestError = 1e9;
    bool found = false;
    g_bikeTrace.object = g_bikeTrace.pass = -1;
    g_bikeTrace.carry = -1;
    g_bikeTrace.objects = static_cast<int>(g_objects.size());
    // How far your rider may be from their place on the bike (learnt in first person, an average):
    // in third person they sit, stand, lean and slide fore and aft on it by more than 20 cm, and a
    // limit of 0.7 game units (21 cm: the game's unit is the foot) lost the bike every few seconds,
    // the headlight jumping off the plate each time. 50 cm let the swingarm (44 cm off, drawn with
    // the same program, turned nearly the same way) be taken for it. In metres, whatever the unit.
    const double carryLimit = (b.learned || b.loaded ? 0.35 : 0.45) * g_unitsPerMetre;
    double nearestCarry = 1e9;  // (for the trace: how near the nearest was when none was taken)
    for (int pass = 0; pass < 2 && !found; ++pass) {
        const bool follow = pass == 0;
        if (follow && !g_haveLastBike)
            continue;
        bestError = follow ? 1.6 : carryLimit;
        for (const SceneObject& o : g_objects) {
            if (follow ? o.program != g_lastBikeProgram : (b.program && o.program != b.program))
                continue;
            double vm[16], world[16];
            for (int i = 0; i < 16; ++i)
                vm[i] = o.viewModel[i];
            MatMul(invRaw, vm, world);
            if (Dot(Normalize(TransformDirection(world, b.up)), worldUp) < 0.5)
                continue;  // not upright (a wheel, a fork)
            const Vec3 origin{ world[12], world[13], world[14] };
            if (follow && Length(origin - g_lastBikeOrigin) > 2.0)
                continue;
            if (follow) {
                // The same object as last frame: turned the same way (another part of the bike that
                // also stands upright, turned otherwise, once made every part seem to turn at once),
                // the nearest to where it was.
                bool same = true;
                for (int c = 0; c < 3 && same; ++c) {
                    const Vec3 now = Normalize(Vec3{ world[c * 4], world[c * 4 + 1], world[c * 4 + 2] });
                    const Vec3 was = Normalize(Vec3{ g_lastBikeWorld[c * 4], g_lastBikeWorld[c * 4 + 1], g_lastBikeWorld[c * 4 + 2] });
                    same = Dot(now, was) > 0.95;
                }
                // Of those, the one carrying your rider best, as strictly as the search below: the
                // nearest to where it was once kept a neighbouring part 44 cm off (every wheel, mark
                // and bit of tire dirt measured on your bike went that far off with it).
                const double carry = Length(TransformPoint(world, b.rider) - g_rider.position);
                if (same && carry < nearestCarry)
                    nearestCarry = carry;
                if (!same || carry > carryLimit)
                    continue;
                if (!found && carry < bestError) {  // (the first drawn: see below)
                    bestError = carry;
                    std::copy(world, world + 16, model);
                    found = true;
                    g_lastBikeProgram = o.program;
                    g_bikeTrace.object = static_cast<int>(&o - g_objects.data());
                    g_bikeTrace.pass = 0;
                    g_bikeTrace.carry = carry / g_unitsPerMetre;
                }
                continue;
            }
            if (!follow && g_rider.haveHeading) {
                Vec3 f = TransformDirection(world, b.forward);
                f = f - worldUp * Dot(f, worldUp);
                if (Length(f) < 1e-3 || Dot(Normalize(f), g_rider.heading) < 0.7)
                    continue;
            }
            // The first one drawn that carries your rider (the game draws the body first, as the
            // first person tracker's "draw #0"), not the one that does it best: the swingarm, the
            // body turned a little about its pivot, can, and measured from it your front wheel was
            // 60 cm out of place and let go of.
            const double error = Length(TransformPoint(world, b.rider) - g_rider.position);
            if (error < nearestCarry)
                nearestCarry = error;
            if (!found && error < bestError) {
                bestError = error;
                std::copy(world, world + 16, model);
                found = true;
                g_lastBikeProgram = o.program;
                g_bikeTrace.object = static_cast<int>(&o - g_objects.data());
                g_bikeTrace.pass = 1;
                g_bikeTrace.carry = error / g_unitsPerMetre;
            }
        }
    }
    g_haveLastBike = found;
    if (!found && nearestCarry < 1e9)
        g_bikeTrace.carry = nearestCarry / g_unitsPerMetre;
    if (found) {
        g_lastBikeOrigin = Vec3{ model[12], model[13], model[14] };
        std::copy(model, model + 16, g_lastBikeWorld);
    }
    return found;
}

// Your bike this frame: first person with the bike tracker locked (teaching the headlight
// where things sit on it), else the object under your rider.
void UpdateBike(double dt)
{
    const Config& cfg = GetConfig();
    LoadLearnedBike();
    g_bikeNow.found = false;
    double invRaw[16];
    if (!g_frame.haveView || !AffineInverse(g_frame.raw, invRaw))
        return;
    // (g_thirdPerson: set as your rider is found, FindOwnRider.)
    const bike::Pose& pose = bike::Current();
    if (cfg.enabled && !g_thirdPerson && pose.tracked) {
        MatMul(invRaw, pose.cameraSpace, g_bikeNow.model);
        LearnBike(g_bikeNow.model, pose, dt);
        g_bikeNow.found = true;
        g_bikeNow.how = 0;
    } else if (FindBike(g_bikeNow.model)) {
        g_bikeNow.found = true;
        g_bikeNow.how = 1;
    }
}

Vec3 Column(const double m[16], int c) { return Vec3{ m[c * 4], m[c * 4 + 1], m[c * 4 + 2] }; }
Vec3 Origin(const double m[16]) { return Vec3{ m[12], m[13], m[14] }; }

// The axle of a turn from a to b (both seen from the same reference): the points the turn leaves
// where they are. False when it barely turned.
bool AxleOfTurn(const double a[16], const double b[16], Vec3 across, Vec3& point)
{
    double inv[16], d[16];
    if (!AffineInverse(a, inv))
        return false;
    MatMul(b, inv, d);  // p_b = d * p_a
    // The axle runs across the bike: in the plane across it the turn is a 2D rotation, and its
    // centre is the solution of (I - R) p = t there.
    const Vec3 e1 = Normalize(Cross(across, std::fabs(across.y) < 0.9 ? Vec3{ 0, 1, 0 } : Vec3{ 0, 0, 1 }));
    const Vec3 e2 = Cross(across, e1);
    const Vec3 r1 = TransformDirection(d, e1), r2 = TransformDirection(d, e2);
    const double m00 = 1.0 - Dot(r1, e1), m01 = -Dot(r2, e1), m10 = -Dot(r1, e2), m11 = 1.0 - Dot(r2, e2);
    const Vec3 t{ d[12], d[13], d[14] };
    const double t0 = Dot(t, e1), t1 = Dot(t, e2);
    const double det = m00 * m11 - m01 * m10;
    if (std::fabs(det) < 0.004)
        return false;  // under ~3.5 degrees: too little to tell
    const double x = (t0 * m11 - m01 * t1) / det, y = (m00 * t1 - m10 * t0) / det;
    point = e1 * x + e2 * y + across * Dot(Vec3{ a[12], a[13], a[14] }, across);
    return true;
}

// A part's full transform (world): its record's model, then its bone.
void PartWorld(const SkeletonRecord& rec, const double world[16], int b, double out[16])
{
    const float* r = rec.rotation + b * 9;
    double bone[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k)
            bone[c * 4 + k] = rec.transposed ? r[k * 3 + c] : r[c * 3 + k];
    bone[12] = rec.position[b * 3];
    bone[13] = rec.position[b * 3 + 1];
    bone[14] = rec.position[b * 3 + 2];
    MatMul(world, bone, out);
}

// A wheel seen turning: where its axle sits on your bike, averaged over a couple of seconds (it
// moves with the suspension). A sample far from what's learnt is another part's (or a crash) and
// is ignored, unless it keeps coming: then the bike was changed.
// Whether a point in your bike's space is somewhere wheel i's axle can be: on the bike's centreline,
// within 3 m of its origin (which can sit well under the ground), and (once the other wheel is learnt too) a wheelbase from
// it, the front ahead of the rear. Anything else came from a bad moment (a crash, a reset, the bike
// mistaken for another part).
bool WheelPlausibleOnBike(int i, Vec3 point)
{
    const BikeModel& b = g_bikeModel;
    const Vec3 across = Normalize(Cross(b.forward, b.up));
    const double u = g_unitsPerMetre;
    if (std::fabs(Dot(point, across)) > 0.15 * u || Length(point) > 3.0 * u)
        return false;  // a wheel sits on the bike's centreline (a crash once put one 25 cm off it)
    const Wheel& other = g_wheels[1 - i];
    if (other.onBikeWeight < kWheelOnBikeMin || std::fabs(Dot(other.onBike, across)) > 0.15 * u)
        return true;  // nothing to check it against
    const double ahead = Dot((i == 1 ? point - other.onBike : other.onBike - point), b.forward);  // front minus rear
    return ahead > 0.5 * kWheelbase * u && ahead < 1.5 * kWheelbase * u;
}

void LearnWheelOnBike(int index, Wheel& w, Vec3 axis, double dt)
{
    double inv[16];
    // Not in a replay: the bike and wheels followed there can be someone else's.
    if (!g_bikeNow.found || dt <= 0.0 || replay::Current() != replay::Mode::Off || !AffineInverse(g_bikeNow.model, inv))
        return;
    const Vec3 point = TransformPoint(inv, w.axle);
    if (!WheelPlausibleOnBike(index, point))
        return;  // not where a wheel can be on a bike: this frame's bike (or wheel) is off
    // Once learnt it stays put: refining it all session let odd moments (a crash, a replay) drag it
    // further and further off, and the wheels' shadows with it. Only seen somewhere else for ten
    // seconds on end (another bike) is it learnt again.
    if (w.settled) {
        // (Only with your bike measured precisely, first person: in third person it's found as a
        // part under your rider, a little off and turned, and ten seconds of that once had your
        // rear wheel learnt again somewhere it isn't.)
        if (g_bikeNow.how != 0)
            return;
        if (Length(point - w.onBike) <= 0.3 * g_unitsPerMetre) {
            w.onBikeRejected = 0;
        } else if (++w.onBikeRejected >= 600) {
            w.settled = false;
            w.onBikeWeight = 0;
            w.onBikeRejected = 0;
            Log("postfx: your %s wheel has been somewhere else on your bike for ten seconds (another bike?): learning it again",
                index ? "front" : "rear");
        }
        if (w.settled)
            return;
    }
    Vec3 dir = Normalize(TransformDirection(inv, axis));
    if (w.onBikeWeight > 0 && Dot(dir, w.axisOnBike) < 0)
        dir = dir * -1.0;
    if (w.onBikeWeight >= kWheelOnBikeMin && Length(point - w.onBike) > 0.3 * g_unitsPerMetre) {
        if (++w.onBikeRejected < 90)
            return;
        w.onBikeWeight = 0;  // start over
    }
    w.onBikeRejected = 0;
    const double k = w.onBikeWeight > 0 ? 1.0 - std::exp(-dt / 2.0) : 1.0;
    w.onBike = w.onBike + (point - w.onBike) * k;
    w.axisOnBike = Normalize(w.axisOnBike + (dir - w.axisOnBike) * k);
    w.onBikeWeight += dt;
    if (w.onBikeWeight >= 3.0)
        w.settled = true;
}

// A wheel whose part isn't seen: placed from your bike, rolling on at your speed.
void PlaceWheelFromBike(Wheel& w, int index, double dt)
{
    const Vec3 worldUp = WorldUp();
    {
        // On the bike's centreline (where it sits, without the little it's off it).
        const Vec3 across = Normalize(Cross(g_bikeModel.forward, g_bikeModel.up));
        w.axle = TransformPoint(g_bikeNow.model, w.onBike - across * Dot(w.onBike, across));
    }
    const Vec3 axis = Normalize(TransformDirection(g_bikeNow.model, w.axisOnBike));
    Vec3 bikeUp = Normalize(TransformDirection(g_bikeNow.model, g_bikeModel.up));
    bikeUp = Normalize(bikeUp - axis * Dot(bikeUp, axis));
    const Vec3 ahead = Cross(bikeUp, axis);
    const double radius = (index == 1 ? 0.355 : 0.345) * g_unitsPerMetre;
    w.roll = std::fmod(w.roll + Length(g_rider.velocity) * dt / radius, 2.0 * kPi);
    w.axleAxis = 0;
    w.axes[0] = axis;
    w.axes[1] = bikeUp * std::cos(w.roll) + ahead * std::sin(w.roll);
    w.axes[2] = Cross(w.axes[0], w.axes[1]);
    Vec3 wheelUp = Normalize(Cross(Normalize(Cross(axis, worldUp)), axis));
    if (Dot(wheelUp, worldUp) < 0)
        wheelUp = wheelUp * -1.0;
    w.up = wheelUp;
}

// What's learnt about the wheels goes into the ini (a few seconds' worth first, then at most once a
// minute), so first person has them from the start next time.
void SaveLearnedWheels(double now)
{
    static double savedAt = -1e9;
    if (now - savedAt < 60.0)
        return;
    bool wrote = false;
    for (int i = 0; i < 2; ++i) {
        Wheel& w = g_wheels[i];
        if (w.onBikeWeight < 3.0 || w.onBikeWeight - w.savedWeight < 3.0)
            continue;
        const double v[3] = { w.onBike.x, w.onBike.y, w.onBike.z };
        WriteLearned(i ? "FrontWheelOnBike" : "RearWheelOnBike", v);
        if (w.savedWeight < kLoadedWheelWeight + 0.5)
            Log("postfx: learnt where your %s wheel sits on your bike: (%.2f, %.2f, %.2f)", i ? "front" : "rear", v[0], v[1], v[2]);
        w.savedWeight = w.onBikeWeight;
        wrote = true;
    }
    if (wrote)
        savedAt = now;
}

void UpdateWheels(double now, double dt)
{
    const Vec3 worldUp = WorldUp();
    for (Wheel& w : g_wheels) {
        w.on = w.drawnNow = false;
        w.maskParts = 0;
    }
    for (PartTrack& t : g_partTracks) {
        t.seenNow = false;
        if (t.used && now - t.lastSeen > 3.0)
            t = PartTrack{};
    }
    if (!g_rider.seen || !g_rider.haveHeading) {
        snprintf(g_tireStatus, sizeof g_tireStatus, "TIRES: %s", g_rider.seen ? "WAITING FOR YOU TO MOVE" : "LOOKING FOR YOUR RIDER");
        if (g_wheelsWereOn && g_wheelLogLines < kWheelLogLinesMax) {
            ++g_wheelLogLines;
            Log("postfx: tire marks: your wheels lost: %s", g_rider.seen ? "your rider's heading isn't known (standing still after a reset?)"
                                                                         : "your rider wasn't found this frame");
        }
        g_wheelsWereOn = false;
        return;
    }
    // The report: three frames, a few apart, once you've ridden 15 s.
    if (Length(g_rider.velocity) > 3.0)
        g_report.riding += dt;
    g_report.capturing = !g_report.written && g_report.riding > 15.0 && g_report.framesCaptured < 3 &&
                         (static_cast<int>(g_report.riding * 60.0) % 6 == 0);
    const Vec3 forward = g_rider.heading, across = Normalize(Cross(forward, worldUp));
    const Vec3 up = g_rider.haveLean ? g_rider.lean : worldUp;

    // What the parts are seen from: your bike when it's found, else your rider (its x across the
    // bike, y up, -z forward).
    const int reference = g_bikeNow.found ? 0 : 1;
    double refWorld[16];
    Vec3 refAcross, refForward;
    if (reference == 0) {
        std::copy(g_bikeNow.model, g_bikeNow.model + 16, refWorld);
        refForward = g_bikeModel.forward;
        refAcross = Normalize(Cross(g_bikeModel.forward, g_bikeModel.up));
    } else {
        const double frame[16] = { across.x, across.y, across.z, 0, worldUp.x, worldUp.y, worldUp.z, 0,
                                   -forward.x, -forward.y, -forward.z, 0, g_rider.position.x, g_rider.position.y,
                                   g_rider.position.z, 1 };
        std::copy(frame, frame + 16, refWorld);
        refForward = Vec3{ 0, 0, -1 };
        refAcross = Vec3{ 1, 0, 0 };
    }
    double refInverse[16];
    if (!AffineInverse(refWorld, refInverse))
        return;
    const Vec3 riderInRef = TransformPoint(refInverse, g_rider.position);
    if (g_report.capturing) {
        char line[256];
        snprintf(line, sizeof line, "frame %d: rider %.2f m from the camera, %.1f m/s, seen from your %s; %d skinned draw(s), %d object(s) near the camera",
                 g_report.framesCaptured + 1, g_rider.nearestBone, Length(g_rider.velocity), reference == 0 ? "bike" : "rider",
                 static_cast<int>(g_skeletons.size()), static_cast<int>(g_objects.size()));
        g_report.lines.push_back(line);
    }

    // The candidates: rigid objects (any program) and skinned draws' bones near your rider.
    std::vector<SkeletonRecord> objects;
    for (const SceneObject& o : g_objects) {
        double vm[16], world[16], invRaw[16];
        if (!AffineInverse(g_frame.raw, invRaw))
            break;
        for (int i = 0; i < 16; ++i)
            vm[i] = o.viewModel[i];
        MatMul(invRaw, vm, world);
        if (Length(Vec3{ world[12], world[13], world[14] } - g_rider.position) > 3.0 * g_unitsPerMetre)
            continue;
        SkeletonRecord rec;
        rec.program = o.program;
        rec.bones = 1;
        std::copy(o.viewModel, o.viewModel + 16, rec.viewModel);
        rec.rotation[0] = rec.rotation[4] = rec.rotation[8] = 1.0f;
        objects.push_back(rec);
    }
    std::unordered_map<unsigned long long, int> ordinals;  // (program, kind) -> how many seen so far
    const double nearRange = 3.0 * g_unitsPerMetre;
    for (int kind = 0; kind < 2; ++kind) {
        const std::vector<SkeletonRecord>& list = kind == 0 ? objects : g_skeletons;
        for (const SkeletonRecord& rec : list) {
            double world[16];
            if (rec.rider || !RecordWorld(rec, world))
                continue;
            if (kind == 1) {  // a skinned draw: near your rider?
                bool nearRider = false;
                for (int b = 0; b < rec.bones && !nearRider; ++b)
                    nearRider = Length(BonePosition(rec, world, b) - g_rider.position) < nearRange;
                if (!nearRider)
                    continue;
            }
            const unsigned long long group = (static_cast<unsigned long long>(rec.program) << 1) | kind;
            const int ordinal = ordinals[group]++;
            for (int b = 0; b < rec.bones; ++b) {
                const unsigned long long key = (group << 24) ^ (static_cast<unsigned long long>(ordinal) << 8) ^ b;
                double part[16], rel[16];
                PartWorld(rec, world, b, part);
                MatMul(refInverse, part, rel);
                const Vec3 origin{ part[12], part[13], part[14] };
                // Which track: the same part last frame, where it should be now (it moved with your
                // rider); else the same place in the draw order; else a new one.
                PartTrack* track = nullptr;
                double nearest = 0.45 * g_unitsPerMetre;
                for (PartTrack& t : g_partTracks) {
                    if (!t.used || t.seenNow || t.program != rec.program || t.kind != kind || t.bone != b)
                        continue;
                    const double d = Length(origin - (t.origin + g_rider.moved));
                    if (d < nearest) {
                        nearest = d;
                        track = &t;
                    }
                }
                if (!track)
                    for (PartTrack& t : g_partTracks)
                        if (t.used && !t.seenNow && t.key == key) {
                            track = &t;
                            break;
                        }
                if (!track) {
                    for (PartTrack& t : g_partTracks)
                        if (!t.used) {
                            track = &t;
                            break;
                        }
                    if (!track)
                        continue;
                    *track = PartTrack{};
                    track->used = true;
                    track->key = key;
                    track->program = rec.program;
                    track->kind = kind;
                    track->bone = b;
                }
                PartTrack& t = *track;
                double turn = 0, rolled = 0;
                if (t.lastSeen >= 0 && now - t.lastSeen < 0.25 && t.reference == reference) {
                    double inv[16], d[16];
                    if (AffineInverse(t.rel, inv)) {
                        MatMul(rel, inv, d);
                        turn = std::acos(Clamp((d[0] + d[5] + d[10] - 1.0) * 0.5, -1.0, 1.0));
                        const Vec3 about{ d[6] - d[9], d[8] - d[2], d[1] - d[4] };  // 2 sin(turn) along its axis
                        if (Length(about) > 1e-9)
                            rolled = turn * Dot(Normalize(about), refAcross);
                    }
                    Vec3 axle;
                    // Near your rider (about a metre from a wheel's axle). Not near the reference's
                    // origin: your bike's sits well away from its middle, as far as the axles.
                    if (turn > 0.06 && turn < 2.8 && AxleOfTurn(t.rel, rel, refAcross, axle) &&
                        Length(axle - riderInRef) < 1.5 * g_unitsPerMetre) {
                        // In the reference: on the bike's centreline (for picking the pair).
                        const Vec3 centred = axle - refAcross * Dot(axle, refAcross);
                        if (t.axleWeight < 1.5) {
                            t.axleEst = (t.axleEst * t.axleWeight + centred * turn) / (t.axleWeight + turn);
                            t.axleWeight += turn;
                        } else if (Length(centred - t.axleEst) < 0.12 * g_unitsPerMetre) {
                            t.axleEst = t.axleEst + (centred - t.axleEst) * std::fmin(0.2, turn * 0.25);
                            t.axleWeight += turn;
                            t.rejected = 0;
                        } else if (++t.rejected > 30) {
                            t.axleEst = centred;
                            t.axleWeight = turn;
                            t.rejected = 0;
                        }
                        // In its own model: the point on its axle line nearest its origin (constant
                        // for a wheel; the line's direction is its own axis across the bike).
                        double invRel[16];
                        if (AffineInverse(rel, invRel)) {
                            Vec3 q = TransformPoint(invRel, axle);
                            int k = 0;
                            double most = -1;
                            for (int j = 0; j < 3; ++j) {
                                const double m = std::fabs(Dot(Normalize(Vec3{ rel[j * 4], rel[j * 4 + 1], rel[j * 4 + 2] }), refAcross));
                                if (m > most) {
                                    most = m;
                                    k = j;
                                }
                            }
                            (k == 0 ? q.x : k == 1 ? q.y : q.z) = 0.0;
                            t.localAxis = k;
                            if (t.localWeight < 1.5) {
                                t.axleLocal = (t.axleLocal * t.localWeight + q * turn) / (t.localWeight + turn);
                                t.localWeight += turn;
                            } else if (Length(q - t.axleLocal) < 0.12 * g_unitsPerMetre) {
                                t.axleLocal = t.axleLocal + (q - t.axleLocal) * std::fmin(0.2, turn * 0.25);
                                t.localWeight += turn;
                                t.localRejected = 0;
                            } else if (++t.localRejected > 30) {
                                t.axleLocal = q;
                                t.localWeight = turn;
                                t.localRejected = 0;
                            }
                        }
                    }
                } else if (t.reference != reference) {
                    t.axleEst = Vec3{};  // seen from something else now: start over (its own-model axle stays)
                    t.axleWeight = 0;
                }
                t.spin = t.spin * std::exp(-dt / 0.8) + turn;
                t.roll = t.roll * std::exp(-dt / 0.8) + rolled;
                t.total += turn;
                t.reference = reference;
                std::copy(rel, rel + 16, t.rel);
                for (int k = 0; k < 3; ++k)
                    t.axes[k] = Normalize(Vec3{ part[k * 4], part[k * 4 + 1], part[k * 4 + 2] });
                t.origin = origin;
                std::copy(part, part + 16, t.world);
                const float* r = rec.rotation + b * 9;
                const float* bonePos = rec.position + b * 3;
                t.drawnAsIs = rec.bones == 1 && bonePos[0] == 0.0f && bonePos[1] == 0.0f && bonePos[2] == 0.0f && r[0] == 1.0f &&
                              r[4] == 1.0f && r[8] == 1.0f && r[1] == 0.0f && r[2] == 0.0f && r[3] == 0.0f &&
                              r[5] == 0.0f && r[6] == 0.0f && r[7] == 0.0f;
                if (t.drawnAsIs)
                    std::copy(rec.viewModel, rec.viewModel + 16, t.viewModel);
                t.lastSeen = now;
                t.seenNow = true;
                if (g_report.capturing && g_report.lines.size() < 600) {
                    const Vec3 at = t.origin - g_rider.position;
                    char line[256];
                    snprintf(line, sizeof line, "  program %u %s #%d bone %d/%d: origin %.2f along %.2f across %.2f up, turned %.3f rad%s",
                             rec.program, kind == 0 ? "object" : "skinned", ordinal, b, rec.bones, Dot(at, forward),
                             Dot(at, across), Dot(at, worldUp), turn, t.localWeight > 1.5 ? " (axle found)" : "");
                    g_report.lines.push_back(line);
                }
            }
        }
    }
    // The wheels: parts that keep turning, with an axle, a wheelbase apart (front: further forward).
    // Turning fast right now (a wheel rolling: radians in about the last second), not a part that
    // creeps round now and then (the chain, a brake disc), and on the bike's centreline.
    const auto Spinning = [&](int i) {
        const PartTrack& t = g_partTracks[i];
        return t.used && t.seenNow && t.spin > 3.0 && t.axleWeight > 2.0;
    };
    // Where your wheels sit on your bike is settled: a part can only be one of them there (another
    // bike close by has spinning wheels near you too: your tire marks and dirt went to its wheels).
    // (Across the bike from the part's own axle point, not the centred estimate: a wheel a metre
    // to your side is the same as yours once centred.)
    const auto AtYourWheel = [&](int k, int wheel) {
        const Wheel& w = g_wheels[wheel];
        const PartTrack& t = g_partTracks[k];
        if (reference != 0) {
            // Seen from your rider (your bike not found): at least on your rider's centreline.
            const Vec3 point = TransformPoint(refInverse, TransformPoint(t.world, t.axleLocal));
            return std::fabs(Dot(point, refAcross)) < 0.4 * g_unitsPerMetre;
        }
        if (!w.settled)
            return true;
        const Vec3 learnt = w.onBike - refAcross * Dot(w.onBike, refAcross);
        const Vec3 point = TransformPoint(refInverse, TransformPoint(t.world, t.axleLocal));
        // Across your bike always (someone else's wheel alongside is a metre or so to the side);
        // along and up it only with your bike measured precisely (first person: the bike tracker). In
        // third person it's found as a part under your rider, which can be one turned a little
        // about the swingarm's pivot (every wheel then looked 70 cm off and was let go of).
        const bool aside = std::fabs(Dot(point - w.onBike, refAcross)) < 0.4 * g_unitsPerMetre;
        return aside && (g_bikeNow.how != 0 || Length(t.axleEst - learnt) < 0.3 * g_unitsPerMetre);
    };
    const auto Along = [&](int i) { return Dot(g_partTracks[i].axleEst, refForward); };
    // Keep the pair found while it's still good (each wheel is drawn twice, by two programs: hopping
    // between the copies would lose what was learnt about the axle).
    const auto StillGood = [&](const Wheel& w) {
        return w.track >= 0 && g_partTracks[w.track].used && g_partTracks[w.track].key == w.trackKey &&
               g_partTracks[w.track].seenNow && g_partTracks[w.track].localWeight > 1.5;
    };
    if (!(StillGood(g_wheels[0]) && StillGood(g_wheels[1]))) {
        int bestA = -1, bestB = -1;
        double bestScore = 0;
        for (int i = 0; i < 192; ++i) {
            if (!Spinning(i) || !AtYourWheel(i, 1))
                continue;
            for (int j = 0; j < 192; ++j) {
                if (j == i || !Spinning(j) || !AtYourWheel(j, 0))
                    continue;
                // i ahead of j by a real wheelbase: in the game's feet (or metres, in a test scene),
                // and in the unit already measured. Two other parts that spin (a crash, someone
                // else's bike alongside: once 0.95 units apart) would make every size wrong.
                const double unit = (Along(i) - Along(j)) / kWheelbase;
                const bool feet = std::fabs(unit - 3.2808) < 0.15 * 3.2808, metres = std::fabs(unit - 1.0) < 0.15;
                if ((!feet && !metres) || (g_unitsMeasured && std::fabs(unit - g_unitsPerMetre) > 0.2 * g_unitsPerMetre))
                    continue;
                const double si = g_partTracks[i].spin, sj = g_partTracks[j].spin;
                if (si > 3.0 * sj || sj > 3.0 * si)
                    continue;  // both wheels roll at about the same speed
                // Copies whose axle is already learnt first.
                // (And copies drawn as they are, rigid: a skinned bone's has no draw call of its own
                // for the tire dirt's mask.)
                const double score = si + sj + (g_partTracks[i].localWeight > 1.5 && g_partTracks[j].localWeight > 1.5 ? 1000.0 : 0.0) +
                                     (g_partTracks[i].drawnAsIs && g_partTracks[j].drawnAsIs ? 500.0 : 0.0);
                if (score > bestScore) {
                    bestScore = score;
                    bestA = i;
                    bestB = j;
                }
            }
        }
        // Once the unit is known, only a pair a real wheelbase apart (no tumbling parts in a crash).
        if (bestA >= 0 && g_unitsLocked &&
            std::fabs((Along(bestA) - Along(bestB)) - kWheelbase * g_unitsPerMetre) > 0.2 * kWheelbase * g_unitsPerMetre)
            bestA = bestB = -1;
        if (bestA >= 0) {
            if ((g_wheels[1].track != bestA || g_wheels[0].track != bestB) && g_wheels[0].track >= 0 && g_wheelLogLines < kWheelLogLinesMax) {
                ++g_wheelLogLines;
                Log("postfx: tire marks: switched to another pair of wheels (programs %u / %u)", g_partTracks[bestA].program,
                    g_partTracks[bestB].program);
            }
            g_wheels[1].track = bestA;
            g_wheels[0].track = bestB;
            g_wheels[0].fromPair = g_wheels[1].fromPair = true;
            g_wheels[1].trackKey = g_partTracks[bestA].key;
            g_wheels[0].trackKey = g_partTracks[bestB].key;
            // The game's unit, from the wheelbase: measured until it holds steady, then locked (MX
            // Simulator's feet, when it's within 15% of them).
            const double measured = Clamp((Along(bestA) - Along(bestB)) / kWheelbase, 0.6, 6.0);
            if (!g_unitsLocked) {
                if (!g_unitsMeasured) {
                    g_unitsPerMetre = measured;
                    Log("postfx: your bike's wheelbase is %.2f game units: %.2f units to the metre", Along(bestA) - Along(bestB),
                        measured);
                } else {
                    g_unitsAgreeing = std::fabs(measured - g_unitsPerMetre) < 0.1 * g_unitsPerMetre ? g_unitsAgreeing + 1 : 0;
                    g_unitsPerMetre += (measured - g_unitsPerMetre) * 0.05;
                    if (g_unitsAgreeing > 60) {
                        g_unitsLocked = true;
                        if (std::fabs(g_unitsPerMetre - 3.2808) < 0.15 * 3.2808)
                            g_unitsPerMetre = 3.2808;
                        Log("postfx: the game's unit is settled: %.2f units to the metre", g_unitsPerMetre);
                    }
                }
            }
            g_unitsMeasured = true;
            for (int i = 0; i < 2; ++i) {
                Wheel& w = g_wheels[i];
                const PartTrack& t = g_partTracks[w.track];
                const Vec3 axle = t.axleEst;
                if (!w.logged) {
                    w.logged = true;
                    Log("postfx: found your bike's %s wheel (program %u): its axle %.2f units along your %s", i ? "front" : "rear",
                        t.program, Dot(axle, refForward), reference == 0 ? "bike" : "rider");
                }
            }
        }
    }
    // One wheel drawn on its own (first person: the game never draws your rear wheel, so there's no
    // pair to find): the part turning about an axle where that wheel was learnt on your bike.
    if (reference == 0) {
        for (int i = 0; i < 2; ++i) {
            Wheel& w = g_wheels[i];
            if (StillGood(w) || w.onBikeWeight < kWheelOnBikeMin || !WheelPlausibleOnBike(i, w.onBike))
                continue;
            const Vec3 learnt = w.onBike - refAcross * Dot(w.onBike, refAcross);
            const int other = g_wheels[1 - i].track;
            int best = -1;
            double bestDistance = 0.3 * g_unitsPerMetre;
            for (int k = 0; k < 192; ++k) {
                const PartTrack& t = g_partTracks[k];
                // Rolling like a wheel (not a part that only creeps round) where the wheel was learnt.
                if (k == other || !t.used || !t.seenNow || t.reference != 0 || t.localWeight <= 1.5 || t.localAxis < 0 ||
                    t.axleWeight < 0.5 || t.spin < 3.0 || !AtYourWheel(k, i))
                    continue;
                const double d = Length(t.axleEst - learnt);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = k;
                }
            }
            if (best < 0)
                continue;
            w.track = best;
            w.trackKey = g_partTracks[best].key;
            w.fromPair = false;
            if (!w.logged) {
                w.logged = true;
                Log("postfx: found your bike's %s wheel on its own (program %u), where it was learnt on your bike",
                    i ? "front" : "rear", g_partTracks[best].program);
            }
        }
    }
    // Each wheel: placed from its own model (the axle it turns about there), moved along that axle
    // into the bike's centre plane (where your rider's hips sit). A dropout of up to a second keeps
    // it where it was against your rider.
    bool any = false;
    // Where the wheels sit on your bike is only learnt while both are seen together (a wheel found
    // on its own can be another part turning on the same axle: the sprocket, a brake disc).
    bool bothSeen = true;
    for (const Wheel& w : g_wheels)
        bothSeen = bothSeen && w.fromPair && w.track >= 0 && g_partTracks[w.track].used && g_partTracks[w.track].key == w.trackKey &&
                   g_partTracks[w.track].seenNow && g_partTracks[w.track].localWeight > 1.5 &&
                   g_partTracks[w.track].localAxis >= 0;
    for (int i = 0; i < 2; ++i) {
        Wheel& w = g_wheels[i];
        if (w.track >= 0 && (!g_partTracks[w.track].used || g_partTracks[w.track].key != w.trackKey))
            w.track = -1;  // its slot went to another part
        const bool seen = w.track >= 0 && g_partTracks[w.track].seenNow && g_partTracks[w.track].localWeight > 1.5 &&
                          g_partTracks[w.track].localAxis >= 0;
        w.missing = seen ? 0.0 : w.missing + dt;
        w.unseen = seen ? 0.0 : w.unseen + dt;
        w.seenFor = seen ? w.seenFor + dt : 0.0;
        // (In third person the game draws your wheels: unseen there is the wheel tracker letting go
        // of one a moment, found again within a fifth of a second, and a stand-in then was a ghost
        // tire and swingarm beside the real ones, from your bike found roughly under your rider.)
        if (w.unseen >= (g_thirdPerson ? 1.5 : 0.3))
            w.standIn = true;
        else if (w.seenFor >= 0.5)
            w.standIn = false;
        if (seen) {
            const PartTrack& t = g_partTracks[w.track];
            const Vec3 point = TransformPoint(t.world, t.axleLocal);
            for (int k = 0; k < 3; ++k)
                w.axes[k] = t.axes[k];
            w.axleAxis = t.localAxis;  // the axis it was seen turning about (not whichever lines up now)
            const Vec3 axis = w.axes[w.axleAxis];
            // Along its axle to the bike's centre plane: a fixed offset on the part followed. Learnt
            // from your bike in first person, else from your hips, only while the bike is upright: in
            // a corner your rider hangs off the inside, and following the hips there moved the trail
            // to the inside and had it wobble with the rider's body. Once learnt it barely moves.
            if (w.lateralKey != w.trackKey) {
                w.lateralKey = w.trackKey;
                w.haveLateral = false;
                w.lateralWeight = 0;
                w.lateralFromBike = false;
            }
            const double lateral = Dot(g_rider.hips - point, axis);
            const bool upright = std::fabs(Dot(axis, worldUp)) < 0.12;  // leaned under 7 degrees
            if (!w.haveLateral) {
                w.lateral = lateral;
                w.haveLateral = true;
            } else if (!w.lateralFromBike && upright && std::fabs(lateral - w.lateral) < 0.25 * g_unitsPerMetre) {
                w.lateral += (lateral - w.lateral) * (w.lateralWeight < 3.0 ? 0.03 : 0.003);
                w.lateralWeight += dt;
            }
            w.axle = point + axis * w.lateral;
            // The wheel's own up: across its axle, toward the sky (it leans with the bike).
            const Vec3 ahead = Normalize(Cross(axis, worldUp));
            Vec3 wheelUp = Normalize(Cross(ahead, axis));
            if (Dot(wheelUp, worldUp) < 0)
                wheelUp = wheelUp * -1.0;
            w.up = wheelUp;
            if (bothSeen)
                LearnWheelOnBike(i, w, axis, dt);
            w.fromBike = false;
            // Where it sits on your bike is settled: the tire marks and the tire dirt hold to it.
            // Across the bike, always on its centreline there (no sideways drift, whichever part
            // turning on that axle is followed: the wheel, the sprocket, a brake disc); up / down and
            // along it as the part says (the suspension, where the tire really touches the ground).
            // A part far from it isn't the wheel: its draw call isn't kept for the shadow map.
            bool farOff = false;  // the part is far from where the wheel sits: not the wheel itself
            w.farOff = false;
            if (w.settled && g_bikeNow.found) {
                double inv[16];
                if (AffineInverse(g_bikeNow.model, inv)) {
                    const BikeModel& b = g_bikeModel;
                    const Vec3 bikeAcross = Normalize(Cross(b.forward, b.up));
                    Vec3 local = TransformPoint(inv, w.axle);
                    local = local - bikeAcross * Dot(local - w.onBike, bikeAcross);
                    const double aside = std::fabs(Dot(TransformPoint(inv, point) - w.onBike, bikeAcross));
                    // Never further from it than 35 cm (the suspension's travel): pulled back
                    // smoothly, not swapped for another position (that flickered). Along and up
                    // only with your bike measured precisely (first person, see AtYourWheel).
                    const bool precise = g_bikeNow.how == 0;
                    const Vec3 off = precise ? local - w.onBike : Vec3{};
                    const double limit = 0.35 * g_unitsPerMetre;
                    farOff = Length(off) > limit;
                    w.farOff = farOff;
                    static bool lettingGo[2] = { false, false };  // (logged once a spell, not every frame)
                    if (Length(off) > 0.6 * g_unitsPerMetre || aside > 0.4 * g_unitsPerMetre) {
                        // Further than any suspension moves it: not your wheel (someone else's close
                        // by). Let go of it and place yours from your bike.
                        if (!lettingGo[i] && g_wheelLogLines < kWheelLogLinesMax) {
                            ++g_wheelLogLines;
                            Log("postfx: tire marks: let go of a part taken for your %s wheel: %.2f m to the side, %.2f m off "
                                "(someone else's?)", i ? "front" : "rear", aside / g_unitsPerMetre, Length(off) / g_unitsPerMetre);
                        }
                        lettingGo[i] = true;
                        w.track = -1;
                        PlaceWheelFromBike(w, i, dt);
                        w.fromBike = true;
                    } else {
                        lettingGo[i] = false;
                        if (farOff)
                            local = w.onBike + off * (limit / Length(off));
                        // Onto the bike's centreline only with your bike measured precisely (first
                        // person), learning the part's offset along its axle from it. In third person
                        // your bike is found roughly (a part under your rider, a little off and
                        // turned): put on its centreline the trail went a little to the side, and
                        // jumped back and forth as the bike was found and lost.
                        if (precise) {
                            w.axle = TransformPoint(g_bikeNow.model, local);
                            w.lateral += (Dot(w.axle - point, axis) - w.lateral) * 0.1;
                            w.lateralFromBike = true;
                        }
                    }
                }
            }
            // Its draw call, for the shadow map: kept while the game draws it, cast on your bike
            // when it doesn't.
            // Drawn as it is and taken for yours (not let go above): the tire dirt's mask.
            // Only while it really spins (3 radians in the last second and up: rolling at about a
            // metre a second): the front end steering (handlebars, plate, guards, fork) turns about
            // one line too, and a moment a steering part was taken for your front wheel put mud on
            // all of them. (Standing still the parts found while rolling are kept: DrawWheelMasks.)
            if (!w.fromBike && std::fabs(t.roll) >= 3.0 && std::fabs(t.roll) > 0.7 * t.spin) {
                // Every part drawn as it is that spins about the same axle as fast: the tire with the
                // rest (whichever of them is the one followed: a skinned bone has no draw call of its
                // own, and the brake disc or sprocket alone put the dirt on just them).
                for (int k = 0; k < 192 && w.maskParts < kWheelMaskParts; ++k) {
                    const PartTrack& c = g_partTracks[k];
                    if (!c.used || !c.seenNow || !c.drawnAsIs || c.reference != t.reference || c.localWeight <= 1.5 ||
                        (&c != &t && (c.axleWeight < 0.5 || Length(c.axleEst - t.axleEst) > 0.15 * g_unitsPerMetre ||
                                      c.roll * t.roll < 0.6 * t.roll * t.roll)))  // rolling the same way, as fast
                        continue;
                    bool dup = false;  // (the same transform drawn by two programs: found once)
                    for (int m = 0; m < w.maskParts && !dup; ++m)
                        dup = std::equal(c.viewModel, c.viewModel + 16, w.maskViewModel[m]);
                    if (dup)
                        continue;
                    std::copy(c.viewModel, c.viewModel + 16, w.maskViewModel[w.maskParts]);
                    w.maskAxleLocal[w.maskParts] = c.axleLocal;
                    ++w.maskParts;
                }
            }
            if (t.drawnAsIs && w.fromPair && !farOff) {
                w.drawnNow = true;
                w.partProgram = t.program;
                std::copy(t.viewModel, t.viewModel + 16, w.drawnViewModel);
                w.partAxleLocal = t.axleLocal;
                w.partLocalAxis = t.localAxis;
            }
        } else if (g_bikeNow.found && w.onBikeWeight >= kWheelOnBikeMin && WheelPlausibleOnBike(i, w.onBike)) {
            PlaceWheelFromBike(w, i, dt);
            w.fromBike = true;
        } else if (w.haveFromRider && w.missing <= 1.0) {
            w.axle = g_rider.position + forward * w.fromRider.x + across * w.fromRider.y + worldUp * w.fromRider.z;
            w.fromBike = false;
        } else {
            continue;
        }
        if (seen || w.fromBike) {
            // Kept against your rider for a dropout of your bike as well.
            const Vec3 d = w.axle - g_rider.position;
            w.fromRider = Vec3{ Dot(d, forward), Dot(d, across), Dot(d, worldUp) };
            w.haveFromRider = true;
            w.missing = 0.0;
        }
        w.on = true;
        any = true;
        w.radius = (i == 1 ? 0.355 : 0.345) * g_unitsPerMetre;
        w.contact = w.axle - w.up * w.radius;
        {
            // How far the tire turns in a frame, in the dirt pattern's blotches (26 a metre): over a
            // third of one and the pattern is stretched along the tire to keep it at that.
            const double blotches = Length(g_rider.velocity) * dt / g_unitsPerMetre * 26.0;
            w.stretch = blotches > 0.3 ? 0.3 / blotches : 1.0;
        }
    }
    // Your rear wheel placed from your bike (first person: the game doesn't draw it) sits where it
    // does on the bike on average, but the suspension squats in a corner: placed too low along the
    // bike's own down, which leaned over is also sideways, its trail went off to the outside of the
    // turn and wavered with every bump. Its contact is on the same line through the axle whatever
    // the squat, so it's put where that line meets the ground: the ground your front tire (drawn,
    // seen as it is) went over a moment before, there.
    {
        struct GroundMark { Vec3 at; bool used = false; };
        static GroundMark marks[96];
        static int next = 0;
        const Wheel& front = g_wheels[1];
        if (front.on && !front.fromBike) {
            marks[next].at = front.contact;
            marks[next].used = true;
            next = (next + 1) % 96;
        }
        Wheel& rear = g_wheels[0];
        if (rear.on && rear.fromBike) {
            double best = (1.0 * g_unitsPerMetre) * (1.0 * g_unitsPerMetre);
            const GroundMark* ground = nullptr;
            for (const GroundMark& g : marks) {
                if (!g.used)
                    continue;
                const Vec3 d = g.at - rear.contact;
                const Vec3 flat = d - worldUp * Dot(d, worldUp);
                const double d2 = Dot(flat, flat);
                if (d2 < best) {
                    best = d2;
                    ground = &g;
                }
            }
            const double down = Dot(rear.up, worldUp);
            if (ground && down > 0.3) {
                const double t = (Dot(rear.axle, worldUp) - Dot(ground->at, worldUp)) / down;
                if (t > 0.5 * rear.radius && t < 1.8 * rear.radius)
                    rear.contact = rear.axle - rear.up * t;
            }
        }
        // (Leaned over, a round tire touches down on its side: a little to the inside of the turn,
        // about its profile's radius, 6 cm, times the lean.)
        for (Wheel& w : g_wheels)
            if (w.on)
                w.contact = w.contact + (w.up - worldUp * Dot(w.up, worldUp)) * (0.06 * g_unitsPerMetre);
    }
    {
        const bool on = g_wheels[0].on && g_wheels[1].on;
        if (on != g_wheelsWereOn && g_wheelLogLines < kWheelLogLinesMax && g_unitsMeasured) {
            ++g_wheelLogLines;
            static bool everOn = false;
            const bool learnt = g_wheels[0].onBikeWeight >= kWheelOnBikeMin && g_wheels[1].onBikeWeight >= kWheelOnBikeMin;
            const char* why = on ? (everOn ? "found again" : "found")
                            : learnt && !g_bikeNow.found ? "lost: neither the wheels nor your bike were seen for over a second"
                            : g_wheels[0].track < 0 || g_wheels[1].track < 0 ? "lost: their parts went away (where they sit on your bike isn't learnt yet)"
                            : !g_partTracks[g_wheels[0].track].seenNow || !g_partTracks[g_wheels[1].track].seenNow ? "lost: not drawn for over a second (where they sit on your bike isn't learnt yet)"
                            : "lost: their axles aren't known yet";
            Log("postfx: tire marks: your wheels %s", why);
            everOn = everOn || on;
        }
        g_wheelsWereOn = on;
    }
    snprintf(g_tireStatus, sizeof g_tireStatus, "TIRES: %s", g_wheels[0].on && g_wheels[1].on ? "BOTH WHEELS FOUND"
                                                      : any ? "ONE WHEEL FOUND" : "RIDE A FEW METRES TO FIND THE WHEELS");
    SaveLearnedWheels(now);
    if (g_report.capturing && ++g_report.framesCaptured >= 3 && !g_report.written) {
        g_report.written = true;
        wchar_t path[1024];
        swprintf_s(path, L"%sMXS_REMASTERED_bike.txt", LogDirectory());
        FILE* f = _wfsopen(path, L"w", _SH_DENYWR);
        if (f) {
            fprintf(f, "MXS Remastered: what the tire marks / tire dirt saw near your rider\n%s\n\n", g_tireStatus);
            for (int i = 0; i < 2; ++i)
                fprintf(f, "%s wheel: %s\n", i ? "front" : "rear", g_wheels[i].on ? "found" : "not found");
            fprintf(f, "\nparts that turn (total radians; axle along the reference when found):\n");
            for (const PartTrack& t : g_partTracks)
                if (t.used && t.total > 1.0)
                    fprintf(f, "  program %u key %llx: turned %.1f rad (%.1f lately, %.1f of it rolling)%s %.2f\n", t.program, t.key,
                            t.total, t.spin, t.roll, t.axleWeight > 0 ? ", axle at" : ", no axle",
                            t.axleWeight > 0 ? Dot(t.axleEst, refForward) : 0.0);
            fprintf(f, "\neverything drawn near your rider, three frames:\n");
            for (const std::string& l : g_report.lines)
                fprintf(f, "%s\n", l.c_str());
            fclose(f);
            Log("postfx: wrote %ls (what the tire marks saw near your rider: %s)", path, g_tireStatus);
        }
        g_report.lines.clear();
    }
}

// In the air: the bike's height accelerates downwards at about 1 g for a moment.
void UpdateAir(double dt)
{
    Air& a = g_air;
    if (!g_rider.seen || dt <= 0.0) {
        a = Air{};
        return;
    }
    const double height = Dot(g_rider.position, WorldUp());
    if (a.haveLast) {
        const double velocity = (height - a.lastHeight) / dt;
        if (std::fabs(height - a.lastHeight) > 3.0 * g_unitsPerMetre) {
            a = Air{};
        } else {
            if (a.haveVelocity)
                a.accel += ((velocity - a.velocity) / dt - a.accel) * (1.0 - std::exp(-dt / 0.06));
            a.velocity = velocity;
            a.haveVelocity = true;
        }
    }
    a.lastHeight = height;
    a.haveLast = true;
    const double gravity = kGravity * g_unitsPerMetre;  // in game units
    a.freeFall = a.accel < -0.7 * gravity ? a.freeFall + dt : 0.0;
    if (a.freeFall > 0.12)
        a.airborne = true;
    else if (a.accel > -0.4 * gravity)
        a.airborne = false;
}

// ---- Tire marks ------------------------------------------------------------------------------
// One map of the ground around you that the tires press grooves into, at 4 cm texels, kept
// centred on the camera and wrapping as you ride: the strip that leaves on one side comes back
// cleared on the other. What that clearing forgets, a CPU-side cache of the ruts you pressed
// remembers: each wheel keeps its contact-to-contact segments from the whole session so far
// (their positions in the map's own texels and the rut's true width), and when ground re-
// enters the window the cached segments crossing the cleared strip are pressed into it again
// - the very same stamps a fresh rut gets, so a mark can never be cut or blurred where ground
// is reused, and whatever you rode past earlier comes back exactly as it was, whenever you
// return to it. The cache is bounded by count, not by distance: keeping the whole session is
// what lets the marks survive the far end of a large track, where the bike-anchored window
// has wrapped around many times since. What's on the ground reaches one map: half the window
// to a side, fading out over the last kMarkBlend before its edge.
// Where a tire is down on the ground: the terrain's depth where the screen shows that wheel,
// and otherwise a coarse field of the height of the ground your own tires pressed (below),
// which is what answers in first person - there your wheels sit behind and under the camera,
// hardly any of them is drawn, and a screen test has nothing to say about them.
constexpr int    kMarkSizeWide   = 8192;   // the map, texels to a side (328 m of ground)
constexpr int    kMarkSizeNarrow = 4096;   // if the wide one can't be made
constexpr double kMarkBlend      = 60.0;   // m: how wide the fade band before the window's
                                           // edge is (the shader's kMarkBlend, kept in step
                                           // with this)
constexpr double kMarkCacheMin   = 10000.0;    // segments per wheel the cache keeps at least
constexpr double kMarkCacheMax   = 1000000.0;  // ...and at most: the range the panel and MXS_REMASTERED.ini
                                               // offer for MarkCacheSegs ("Mark cache", which
                                               // shows the memory: kMarkSegBytes a segment times
                                               // both wheels, so a million is about 92 MB). Big
                                               // counts stay cheap because the cache is indexed
                                               // in blocks (below): re-pressing a strip only
                                               // looks at the blocks whose box touches it
constexpr size_t kMarkBlockSegs  = 64;         // segments kept in one block, sharing a box: the
                                               // index the re-press walks, about 2 bytes a
                                               // segment of boxes
constexpr double kMarkRearStrength  = 1.0;   // how deep each wheel's rut goes: the rear carries
constexpr double kMarkFrontStrength = 0.7;   // most of the rider and digs deeper. The re-press
                                             // needs them as well, so they're kept here rather
                                             // than written at both call sites
// How far a wheel's run of ruts turns in and out along the trail, in m: the first of a run's
// segments and its last hold less of the press, spread over this, so a rut that begins where the
// tire touched down and ends where it left the ground fades in and out rather than being cut
// straight across at either - and cut across is what a shoe sole looks like, not a tire. The end
// turns over ground beyond the last contact, which the stamp's own test keeps to where that
// ground really is: over the gap a jump left, nothing is pressed there.
constexpr double kMarkRunFade  = 0.9;   // m of trail a wheel's run of ruts turns in and out over
constexpr double kMarkTailStep = 0.15;  // ...and the end turns out in presses this long
constexpr double kMarkNoFade   = 1e18;  // the distance along a run its end turn runs out at, far
                                        // enough out to stand for "the run was still going": the
                                        // turn is one shared function of that distance, so a
                                        // segment can't hold a value of its own
struct MarkSeg {
    double au, av, bu, bv;  // the two contacts, in the map's own (unwrapped) texels
    float  halfWidth = 0;   // m: the rut's half width (its true world width)
    float  arcA = 0;        // where its first contact lies along the wheel's run of ruts, in the
                            // map's texels of trail counted from the run's own first contact
    float  fadeOut = 1e18f; // how far along that run its end turn runs out (huge: the run was
                            // still going when this was pressed). The turns are read off these
                            // two as one shared function of the distance along the trail rather
                            // than held as this segment's own shares at its ends, which is what
                            // makes a cache re-press give the very same stamps AND keeps a turn
                            // smooth where a run's segments overlap on the ground (a max blend
                            // takes the deeper of two disagreeing straight lines, and they do
                            // disagree, at every contact: see u_arcA and u_fadeOut in kStamp)
};
struct MarkBlock {
    std::vector<MarkSeg> segs;       // up to kMarkBlockSegs of one wheel's trail, oldest first
    double minU, maxU, minV, maxV;   // the box those segments cover, in the map's texels
};
// [0] your rear wheel's, [1] your front's, [2] every other bike's (Config::marksEveryBike): strokes
// of about kOthersStroke each, merged from their frame-to-frame paths, kOthersCapTimes yours' cap.
std::deque<MarkBlock> g_markCache[3];  // each wheel's ruts this session, oldest block first
size_t                g_markCacheSize[3] = {};  // segments held per wheel (what the cap counts)
constexpr double kOthersStroke = 0.5;   // m: every other bike's ruts cached in strokes this long
constexpr double kOthersAtGround = 0.6;  // m: another bike's tire this close to the ground seen there is down on it
constexpr double kMarkSegMaxMetres = 3.0;  // a cached segment longer than this is no tire's path: dropped
constexpr int    kOthersCapTimes = 4;   // their cache: this many times one of your wheels' (a full gate's
                                        // ~44 wheels lay down strokes ~50 times as fast as yours)
struct MarkMap {
    int       size = 0;       // texels to a side (0: this map couldn't be made)
    double    texel = 0;      // one texel, in game units
    double    fadeLen = 1;    // how far a wheel's run of ruts turns in and out at its ends, in this
                              // map's texels of trail (kMarkRunFade over): the stamp reads a turn as
                              // a distance along the trail, so it wants the spread in the same unit
    long long originU = 0, originV = 0;  // where the window's corner is, in unwrapped texels
    bool      set = false;    // the window has an origin
};
MarkMap g_mark;  // the map

// How many segments per wheel the cache keeps: the setting's, inside the range offered there.
size_t MarkCacheCap()
{
    return static_cast<size_t>(Clamp(GetConfig().markCacheSegs, kMarkCacheMin, kMarkCacheMax));
}
size_t CacheCapOf(int i) { return i == 2 ? MarkCacheCap() * kOthersCapTimes : MarkCacheCap(); }

// Nothing the cache holds any more: its segments are placed in the texels the map was laid out
// with, so a new layout starts them over.
// Strips of the window cleared for fresh ground, still to have their cached ruts pressed back
// (ProcessRepress): a few hundred grooves a frame rather than every one at once - in a full lobby,
// every stroke of every bike's laps crossing a strip the width of the map was pressed in the frame
// the window moved. The strips are at the window's far edge (well past where the ruts have faded
// out), so pressing them over a few frames can't be seen.
struct RepressStrip {
    int       axis = 0;          // 0: columns (u), 1: rows (v)
    long long lo = 0, hi = 0;    // [lo, hi) unwrapped texels
    int       cache = 0;         // where the walk through the caches is: this cache,
    size_t    block = 0;         // ...this block (counted from the cache's first ever: g_markPopped)
    unsigned  queued = 0;        // g_markFrameNo when it was queued
};
std::deque<RepressStrip> g_repress;
// The ground every cached rut lies on (the map's unwrapped texels): while it fits in the window with
// kMarkFitMargin to spare, the window holds still around it - every rut there is in the map, so
// nothing is ever cleared or pressed back. (Following the camera, a lobby's whole session of ruts
// crossing each strip the window let in was pressed back as you rode: the frame rate halved.)
// Bigger than that (a track wider than the map), the window follows the camera as before.
struct RiddenBox { bool have = false; double loU = 0, hiU = 0, loV = 0, hiV = 0; };
RiddenBox g_ridden;
bool      g_markFit = false;          // the window holds still around g_ridden (for the trace, the fade)
unsigned  g_markFrameNo = 0;          // UpdateMarks' frames (a strip's age)
constexpr double kMarkFitMargin = 25.0;   // m of ground kept round the ruts (and the camera) when it holds still
constexpr long long kRepressChunk = 128;  // texels (5 m): thin strips the window lets in are joined up to
                                          // this before their ruts are pressed back - a stroke crossing a
                                          // few of them was pressed back for each
size_t g_markPopped[3] = {};      // blocks dropped off the front of each cache (over its cap) so far
int    g_markRepressed = 0;       // grooves pressed back this frame (for the trace)
double g_markMs = 0;              // ms UpdateMarks took this frame (for the trace)
constexpr int    kRepressGrooves = 256;    // grooves pressed back a frame, at most (more with a backlog)
constexpr size_t kRepressWalk    = 40000;  // cached segments looked at a frame, at most (likewise)

void ClearMarkCache()
{
    for (int i = 0; i < 3; ++i) {
        g_markCache[i].clear();
        g_markCacheSize[i] = 0;
        g_markPopped[i] = 0;
    }
    g_repress.clear();
    g_ridden = RiddenBox{};
}

// One rut segment more at the back of a wheel's cache, kept in a block with its neighbours on
// the trail and with the block's box grown around it. Over the cap the oldest blocks go whole
// (the block still being filled at the back is never dropped, so a wheel keeps its recent
// strides whatever the count).
void PushMark(int i, const MarkSeg& s)
{
    // (No tire goes this far between two contacts on the ground: whatever joined them, it isn't a rut.)
    if (std::hypot(s.bu - s.au, s.bv - s.av) * g_mark.texel > kMarkSegMaxMetres * g_unitsPerMetre) {
        static int logged = 0;
        if (logged < 5) {
            ++logged;
            Log("postfx: tire marks: dropped a %.1f m segment from the %s cache (no tire's path)",
                std::hypot(s.bu - s.au, s.bv - s.av) * g_mark.texel / g_unitsPerMetre,
                i == 0 ? "rear wheel's" : i == 1 ? "front wheel's" : "other bikes'");
        }
        return;
    }
    std::deque<MarkBlock>& blocks = g_markCache[i];
    const double loU = std::fmin(s.au, s.bu), hiU = std::fmax(s.au, s.bu);
    const double loV = std::fmin(s.av, s.bv), hiV = std::fmax(s.av, s.bv);
    if (!g_ridden.have)
        g_ridden = RiddenBox{ true, loU, hiU, loV, hiV };
    g_ridden.loU = std::fmin(g_ridden.loU, loU);
    g_ridden.hiU = std::fmax(g_ridden.hiU, hiU);
    g_ridden.loV = std::fmin(g_ridden.loV, loV);
    g_ridden.hiV = std::fmax(g_ridden.hiV, hiV);
    if (blocks.empty() || blocks.back().segs.size() >= kMarkBlockSegs)
        blocks.push_back(MarkBlock{ { s }, loU, hiU, loV, hiV });
    else {
        MarkBlock& b = blocks.back();
        b.segs.push_back(s);
        b.minU = std::fmin(b.minU, loU);
        b.maxU = std::fmax(b.maxU, hiU);
        b.minV = std::fmin(b.minV, loV);
        b.maxV = std::fmax(b.maxV, hiV);
    }
    ++g_markCacheSize[i];
    const size_t cap = CacheCapOf(i);
    while (g_markCacheSize[i] > cap && blocks.size() > 1) {
        g_markCacheSize[i] -= blocks.front().segs.size();
        blocks.pop_front();
        ++g_markPopped[i];
    }
}

void PlaneAxes(Vec3& u, Vec3& v)
{
    const Vec3 up = WorldUp();
    u = Normalize(Cross(up, std::fabs(up.z) < 0.9 ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 0 }));
    v = Normalize(Cross(u, up));
}

int MarkWrap(long long t, int size) { return static_cast<int>(((t % size) + size) % size); }

bool g_markDumpWanted = false;

// The map around the camera (1024 texels: 41 m to a side) as a grey picture, deeper ruts brighter,
// the camera a red dot and your wheels' contacts green. The map's own axes: u to the right, v up.
void DumpMarks(long long cu, long long cv)
{
    const MarkMap& m = g_mark;
    constexpr int kSide = 1024;
    if (!m.set || m.size < kSide || !g_res.marks.fbo)
        return;
    std::vector<unsigned char> grey(static_cast<size_t>(kSide) * kSide, 0);
    long long x0 = cu - kSide / 2 - m.originU, y0 = cv - kSide / 2 - m.originV;  // window texels
    x0 = std::max<long long>(0, std::min<long long>(x0, m.size - kSide));
    y0 = std::max<long long>(0, std::min<long long>(y0, m.size - kSide));
    GLint readFbo = 0, packBuffer = 0;
    g_gl.GetIntegerv(kReadFramebufferBinding, &readFbo);
    g_gl.GetIntegerv(kPixelPackBufferBinding, &packBuffer);
    g_gl.BindBuffer(kPixelPackBuffer, 0);
    g_gl.BindFramebuffer(kReadFramebuffer, g_res.marks.fbo);
    g_gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<unsigned char> row(kSide);
    for (int y = 0; y < kSide; ++y) {
        const int py = MarkWrap(m.originV + y0 + y, m.size);
        const int px = MarkWrap(m.originU + x0, m.size);
        const int first = std::min(kSide, m.size - px);
        g_gl.ReadPixels(px, py, first, 1, kRed, GL_UNSIGNED_BYTE, row.data());
        if (first < kSide)
            g_gl.ReadPixels(0, py, kSide - first, 1, kRed, GL_UNSIGNED_BYTE, row.data() + first);
        std::copy(row.begin(), row.end(), grey.begin() + static_cast<std::ptrdiff_t>(y) * kSide);
    }
    g_gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    g_gl.BindFramebuffer(kReadFramebuffer, static_cast<GLuint>(readFbo));
    g_gl.BindBuffer(kPixelPackBuffer, static_cast<GLuint>(packBuffer));
    int deep = 0, any = 0;
    for (unsigned char g : grey) {
        any += g > 10;
        deep += g > 128;
    }
    // 24-bit BMP, bottom row first (v up).
    std::vector<unsigned char> rgb(static_cast<size_t>(kSide) * kSide * 3);
    for (size_t i = 0; i < grey.size(); ++i)
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = grey[i];
    const auto Spot = [&](long long tu, long long tv, unsigned char r, unsigned char g, unsigned char b) {
        const long long x = tu - m.originU - x0, y = tv - m.originV - y0;
        for (long long dy = -4; dy <= 4; ++dy)
            for (long long dx = -4; dx <= 4; ++dx)
                if (x + dx >= 0 && x + dx < kSide && y + dy >= 0 && y + dy < kSide) {
                    const size_t i = static_cast<size_t>((y + dy) * kSide + (x + dx)) * 3;
                    rgb[i] = b;
                    rgb[i + 1] = g;
                    rgb[i + 2] = r;
                }
    };
    Vec3 u, v;
    PlaneAxes(u, v);
    for (const Wheel& w : g_wheels)
        if (w.on)
            Spot(static_cast<long long>(std::floor(Dot(w.contact, u) / m.texel)),
                 static_cast<long long>(std::floor(Dot(w.contact, v) / m.texel)), 0, 255, 0);
    Spot(cu, cv, 255, 0, 0);
    wchar_t path[1024];
    swprintf_s(path, L"%sMXS_REMASTERED_marks.bmp", LogDirectory());
    FILE* out = _wfsopen(path, L"wb", _SH_DENYWR);
    if (!out)
        return;
    const uint32_t imageBytes = static_cast<uint32_t>(rgb.size()), fileBytes = 54 + imageBytes;
    unsigned char header[54] = { 'B', 'M' };
    const auto Put32 = [&](int at, uint32_t v32) {
        for (int k = 0; k < 4; ++k)
            header[at + k] = static_cast<unsigned char>(v32 >> (8 * k));
    };
    Put32(2, fileBytes);
    Put32(10, 54);
    Put32(14, 40);
    Put32(18, kSide);
    Put32(22, kSide);
    header[26] = 1;
    header[28] = 24;
    Put32(34, imageBytes);
    fwrite(header, 1, 54, out);
    fwrite(rgb.data(), 1, rgb.size(), out);
    fclose(out);
    Log("postfx: tire marks: wrote MXS_REMASTERED_marks.bmp (41 m around you: %.1f%% of it rutted, %.1f%% deeper than half; "
        "window %s, %d strips waiting, %d of other bikes' strokes cached)",
        100.0 * any / grey.size(), 100.0 * deep / grey.size(), g_markFit ? "holding still" : "following you",
        static_cast<int>(g_repress.size()), static_cast<int>(g_markCacheSize[2]));
}

bool EnsureMarks()
{
    if (g_mark.size)
        return true;
    GLint active = 0, bound = 0;
    g_gl.GetIntegerv(kActiveTexture, &active);
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    // The wide map first; the narrow one if 64 MB of R8 (or 8192 texels) is refused.
    // MakeTarget leaves a failed try's texture behind: it goes back before the next try.
    bool ok = false;
    for (const int size : { kMarkSizeWide, kMarkSizeNarrow }) {
        if (MakeTarget(g_res.marks, size, size, kR8, kRed, GL_UNSIGNED_BYTE)) {
            g_mark.size = size;
            ok = true;
            break;
        }
        DeleteTarget(g_res.marks);
    }
    // Wrapped sampling: the shader taps (texel coordinate + the window's corner) over the
    // window's size, which goes past 1 where the window's corner is wrapped.
    if (ok) {
        g_gl.BindTexture(GL_TEXTURE_2D, g_res.marks.tex);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    g_gl.BindTexture(GL_TEXTURE_2D, bound);
    g_gl.ActiveTexture(active);
    g_res.marksCleared = false;
    g_mark.set = false;
    g_mark.texel = 0.04 * g_unitsPerMetre;
    g_mark.fadeLen = kMarkRunFade * g_unitsPerMetre / g_mark.texel;
    const double reach = g_mark.size * g_mark.texel / 2.0 / g_unitsPerMetre;
    const size_t cap = MarkCacheCap();
    Log("postfx: tire marks map %dx%d of 4 cm %s: ruts reach %.0f m and fade out over the last %.0f; the whole session's ruts are kept in a cache of %d segments per wheel (%.1f MB) and re-pressed wherever ground is reused",
        g_mark.size, g_mark.size, ok ? "ready" : "could not be made", reach, kMarkBlend,
        static_cast<int>(cap), static_cast<double>(cap) * 2.0 * static_cast<double>(kMarkSegBytes) / (1024.0 * 1024.0));
    return ok;
}

// Writes value (blend off) or presses a groove (max blend) between a and b, in one map's own
// texels (wrapped), over the rectangle x0, y0, w, h. How far the groove's ends lie along the
// wheel's run of ruts comes through as well, which is what turns a rut's beginning and its end
// in and out along the trail.
void StampRect(Target& map, int x0, int y0, int w, int h, float ax, float ay, float bx, float by,
               float halfWidth, float strength, float arcA, float fadeOut)
{
    if (w <= 0 || h <= 0)
        return;
    Program& p = g_res.programs[kStamp];
    g_gl.Uniform2f(p["u_a"], ax, ay);
    g_gl.Uniform2f(p["u_b"], bx, by);
    g_gl.Uniform1f(p["u_half"], halfWidth);
    g_gl.Uniform1f(p["u_strength"], strength);
    g_gl.Uniform1f(p["u_arcA"], arcA);
    g_gl.Uniform1f(p["u_fadeOut"], fadeOut);
    g_gl.Scissor(x0, y0, w, h);
    Draw(map);
}

void ClearMarks(Target& map, int x0, int y0, int w, int h)
{
    StampRect(map, x0, y0, w, h, -1e6f, -1e6f, -1e6f, -1e6f, 1.0f, 0.0f, 0.0f, 0.0f);
}

// A groove from the last contact to this one, in the map's texels (wrapped), drawn once per
// wrap-around copy it touches. How far its ends lie along the wheel's run of ruts comes through
// as well, so a run's beginning and its end turn in and out the same way however often the
// ground was re-pressed.
void Groove(Target& map, const MarkMap& m, double au, double av, double bu, double bv, double halfWidth,
            double strength, double arcA, double fadeOut)
{
    // The turns are read as a distance along the trail, so they want their spread in the same
    // unit the stamp counts trail distances in: this map's texels.
    Program& sp = g_res.programs[kStamp];
    g_gl.Uniform1f(sp["u_fadeLen"], static_cast<float>(m.fadeLen));
    const double pu = MarkWrap(m.originU, m.size), pv = MarkWrap(m.originV, m.size);
    const double ax = au - m.originU + pu, ay = av - m.originV + pv;
    const double bx = bu - m.originU + pu, by = bv - m.originV + pv;
    const double pad = halfWidth * 2.2 + 2.0;  // (the fill reaches the shader's kLipReach half-widths)
    const double minX = std::fmin(ax, bx) - pad, maxX = std::fmax(ax, bx) + pad;
    const double minY = std::fmin(ay, by) - pad, maxY = std::fmax(ay, by) + pad;
    // Two passes: the ruts already here filled in where this tire pushes the dirt aside (min), then
    // this one pressed (max). The callers have the max blend on; it's left that way.
    for (int pass = 0; pass < 2; ++pass) {
    g_gl.BlendEquationSeparate(pass == 0 ? kFuncMin : kFuncMax, pass == 0 ? kFuncMin : kFuncMax);
    g_gl.Uniform1f(sp["u_fill"], pass == 0 ? 1.0f : 0.0f);
    for (int sx = 0; sx < 2; ++sx) {
        for (int sy = 0; sy < 2; ++sy) {
            const double ox = sx * m.size, oy = sy * m.size;
            const int x0 = static_cast<int>(std::floor(minX - ox)), x1 = static_cast<int>(std::ceil(maxX - ox));
            const int y0 = static_cast<int>(std::floor(minY - oy)), y1 = static_cast<int>(std::ceil(maxY - oy));
            const int cx0 = std::max(x0, 0), cy0 = std::max(y0, 0);
            const int cx1 = std::min(x1, m.size), cy1 = std::min(y1, m.size);
            if (cx1 <= cx0 || cy1 <= cy0)
                continue;
            StampRect(map, cx0, cy0, cx1 - cx0, cy1 - cy0, static_cast<float>(ax - ox), static_cast<float>(ay - oy),
                      static_cast<float>(bx - ox), static_cast<float>(by - oy), static_cast<float>(halfWidth),
                      static_cast<float>(strength), static_cast<float>(arcA), static_cast<float>(fadeOut));
        }
    }
    }  // (each pass)
}

// Clears the texel columns (axis 0) or rows (axis 1) [from, from + count) of one map, wrapped.
void ClearBand(int axis, Target& map, const MarkMap& m, long long from, long long count)
{
    if (count >= m.size) {
        ClearMarks(map, 0, 0, m.size, m.size);
        return;
    }
    const int start = MarkWrap(from, m.size);
    const int first = static_cast<int>(std::min<long long>(count, m.size - start));
    const int second = static_cast<int>(count - first);
    if (axis == 0) {
        ClearMarks(map, start, 0, first, m.size);
        ClearMarks(map, 0, 0, second, m.size);
    } else {
        ClearMarks(map, 0, start, m.size, first);
        ClearMarks(map, 0, 0, m.size, second);
    }
}

// The ground your wheels touch, as a height field ------------------------------------------
// The other half of the on/off-ground answer, and the half that works in every view: a coarse
// grid of the ground's height along the marks map's own axes, filled with your wheels' contacts
// as you ride. The terrain doesn't move, so wherever you've ridden keeps its height for the
// session, and a contact well above the height near here is in the air rather than down. The
// contacts the screen does show are checked against the terrain's depth in the stamp itself,
// which is exact, but only there; this answers everywhere, and the two must agree before the rut
// presses - in first person that's nearly all of them, your wheels sit behind and under the
// camera and are barely drawn at all, and the one pixel the screen does read of a rising tire
// holds ground far ahead at the tire's height, which alone would call it down.
// The state holds rather than flipping on one frame's number: a tire rolling on the ground follows
// a path whose height barely accelerates (each face of the terrain is a plane, so its height's
// velocity is steady), while a tire in the air traces a parabola whose height accelerates at the
// game's gravity, all the way through the flight, from the launch's rise through the drop. That is
// the whole signal, and it needs nothing from the screen: in first person the terrain's depth only
// exists where the game drew the wheel, and as the tire rises so does the camera, so the one pixel
// it does read holds ground far ahead at the tire's height - the old reading called a tire in the
// air down, and pressed ruts there.
// Two readings of that fall are kept: the wheel's own contact path and your bike's own height,
// from the tracker, and a fall is only believed where they both say so. Both are fit as parabolas
// through a dozen samples rather than differentiated twice, because neither is a clean sample: a
// wheel stood in from your bike has its contact's height swing as the bike pitches, and the frame
// to-frame difference of that (or of two eases of it) read +-1.5 g of pure noise while rolling,
// more than a jump is - which is what took first person's tires off the ground late and put them
// back in the middle of a flight, so the rut carried on being pressed under a wheel in the air
// while third person, reading the drawn rider's wheels, got the same jump right. Where the two
// disagree, your bike's is the answer to trust: a bike is a rigid body, its wheels hang off it.
// There is a third answer, and where it speaks it's the best one for going up: the terrain's own
// height everywhere the game drew it (FillTerrainFromDepth unprojects this frame's terrain depth
// into a world height field, the same reconstruction the shaders do). It knows the ground ahead of
// you, where no tire has been, so a wheel the game never renders is asked whether its contact, at
// its radius under its axle, clears the ground there - a question that needs no fit and no drawn
// wheel, and cuts the rut at the lip rather than a fit's window after it. It's asked that way only:
// the seen height is the face the camera saw at that column, which isn't the face under a wheel the
// camera barely draws, and in flight the bare ground ahead is well under a rising tire, so asking it
// "is this tire on the ground" called tires down in the middle of jumps and pressed their ruts (what
// the round this was read from did). Going down stays with the readings and with the ridden field,
// which recorded the face your tire actually pressed - prop or ground - and so still answers for a
// wheel resting on a ramp or a crate. Its clearance is dropped where the ridden field does know
// the ground near the wheel, since that nearer face already says how far the tire is from what
// it's on, and the terrain pass draws no bridge for a tire rolling over one to be clear of. Where
// neither field knows the ground near this wheel - off screen and never ridden - the fitted fall
// answers, as above.
// Down to off the ground: that fall, held to kAirScoreAir - on its own, whatever the ground near
// there says. Off to down: the fall's hold gone under kAirScoreDown, which a landing does by itself
// because nothing falling any more drains it at kAirLandedDrain, or the tire back at the ground
// near here having lost half of whatever its flight reached. The gap to the ground near there is only
// for coming back: asked of the fall on the way out as well, it held a wheel on the ground for
// half a second of a jump down out of a ramp, where the only ground known near the rear wheel is
// the ramp face the tire had just left above it, so the contact read under its own reference
// forever. Heights are in game units, so a change of the unit clears the field.
constexpr double kGroundCellMetres = 1.0;   // m to a cell (512 cells over: 512 m, past a track)
constexpr int    kGroundSize       = 512;   // cells to a side
constexpr double kAirFitWindow     = 0.12;  // s of contacts to fit the parabola through: their
                                            // acceleration. The contact isn't a clean sample (a
                                            // wheel stood in from your bike, its height swinging
                                            // as the bike pitches), so its own acceleration is
                                            // mostly jitter: a millimetre or two of it, taken
                                            // twice over dt squared, is on the order of a whole
                                            // gravity, and the fall then built up at random times
                                            // and vanished mid-flight. A fit through a dozen
                                            // samples averages that out, and this is as quick as a
                                            // fit of this length can be: it reaches the fall's depth
                                            // some part of the window after the launch, and that
                                            // part, at riding speed, is the length of rut pressed
                                            // past the lip.
constexpr double kAirFreeFallG     = 0.6;   // g: a reading this deep is a free fall. Shallower is
                                            // not one (a lip's compression, a wheel that popped
                                            // over a crest) and counts as nothing at all, so it
                                            // can neither put a tire in the air nor hold one down;
                                            // a jump is a whole g deep, so this sits past anything
                                            // the ground does to a tire that is still on it - and
                                            // the tire's and your bike's readings both have to be
                                            // that deep before a fall is a fall at all
constexpr double kAirBackG         = 0.4;   // g: the fall this shallow at all: the height has gone
                                            // out of free fall, so the tire is where the ground is
                                            // again (both the tire's and your bike's must say so,
                                            // which is what stops one wheel's own fluke ending a
                                            // flight for both of them)
constexpr double kAirMaxG          = 6.0;   // g: beyond this the reading is a cut or a respawn
constexpr double kAirSpikG         = 2.5;   // g: a reading this deep is the terrain turning the
                                            // path about in one frame (a lip's crest), so that
                                            // frame is held rather than counted: counted, a wheel
                                            // would be put in the air halfway down a run of whoops
constexpr double kAirScoreGain     = 6.0;   // per second, times the fall's depth in g. The rut is cut
                                            // this far past the lip by whatever the fit's lag is plus
                                            // the frames this needs to clear kAirScoreAir: at 3 it
                                            // took eight frames of a whole gravity, some 0.1 s of
                                            // riding, two or three metres of rut pressed past the
                                            // takeoff's end, and that lag is most of what made the
                                            // mark of a jump start beyond its lip rather than at it.
                                            // The fit's window is what it can be while it beats the
                                            // contact's jitter, so this is the half that could move.
constexpr double kAirScoreDrain    = 1.0;   // per second, what the score loses whatever it reads
constexpr double kAirScoreAir      = 0.2;   // the fall's score this high: off the ground. Nothing
                                            // else is asked of it: requiring the contact to be above
                                            // the ground near there as well, as this was first, kept
                                            // a wheel on the ground for half a second of a jump down
                                            // out of a ramp - the ground known there was the ramp
                                            // behind, which the tire had left below, so the contact
                                            // read under it and the rut went on being pressed.
constexpr double kAirScoreDown     = 0.02;  // this low: back on it. Two numbers rather than one so
                                            // the state can't flap through a flight: thresholding the
                                            // reading, as this was first, put the wheel back on the
                                            // ground halfway through a jump because the eased reading
                                            // wandered about -0.5 g, and the ruts carried on there.
constexpr double kAirGap           = 0.3;   // m of the ground you rode near here: this far under or
                                            // over it, and the contact is back at it (the only
                                            // geometric answer for coming down; the seen terrain
                                            // isn't asked this question, see the note by seenClear,
                                            // and a landing's hold is gone by the readings anyway)
constexpr double kAirClearMetres   = 0.4;   // m above the ground near here: clear of it, a launch.
                                            // Both fields answer this one (the bare terrain the game
                                            // drew ahead of you, and the ground your tires last
                                            // pressed) and it's the only geometric answer for going
                                            // up, which is what cuts the rut at the lip rather than a
                                            // fit's window past it, and answers for a wheel the game
                                            // never renders in first person. It has to be a band and
                                            // not a line because the contact isn't a clean sample:
                                            // the game's suspension compresses a wheel, and a wheel
                                            // stood in from your bike reads within tens of centimetres
                                            // of the face it rolls on whatever its pitch, so a rolling
                                            // wheel has to be allowed that much of slack or it would
                                            // be read clear of the ground all along a rough stretch.
                                            // (Only a launch gets a tire this far above the ground its
                                            // own cell says is there: MXS's lips and whoops move it by
                                            // far less over the ground the tire stays on.)
constexpr double kAirLandedDrain   = 10.0;  // per second the fall's score loses while both readings
                                            // are out of free fall: the fall is over, so whatever
                                            // hold it had goes in a tenth of a second. This is how
                                            // a landing is told, rather than by the slam it makes:
                                            // the slam was the contact's own acceleration turned
                                            // about, which a standing wheel does at several g when
                                            // it's compressed and at a lip's crest at tens of them,
                                            // so it read a slam in the middle of a flight and put
                                            // the tire back on the ground there, where the rut
                                            // carried on being pressed. Out of free fall at all, in
                                            // both readings, is the landing (a bike is rigid, its
                                            // wheels aren't, so one wheel's own fluke can't end a
                                            // flight), and it has to hold for a few frames before
                                            // it is one: this is what a flight keeps of its hold
                                            // against those frames, and what it can cost of the
                                            // rut just after a touchdown
constexpr double kAirClimbMetres   = 1.5;   // m this far above the ground near here, at once, is air
                                            // whatever the fall says: more than any MXS hill climbs
                                            // over a metre travelled, so it can only be a launch
constexpr double kAirTeleportMetres = 20;   // m the bike moves in one frame, more: that was a cut
                                            // or a respawn, so the fit of its fall starts over
constexpr double kGroundKeep       = 0.25;  // how far a fresh sample moves a cell that's known
double g_markTime = 0;                      // s of riding the marks have seen: a clock to stamp the
                                            // fits' samples with, so their windows are in time and
                                            // not in frames (a hitch keeps the fit's samples honest)
bool g_airLogging = false;                  // Whether the marks' air test is written to the log at
                                            // all: it was, the whole time this was being worked on,
                                            // and the notes below (by kAirFitWindow, kAirScoreGain,
                                            // kAirLandedDrain and seenClear) are written from what
                                            // those lines said - a wheel called down with its fall
                                            // still a whole gravity deep, a hold that flapped back
                                            // through the lip, and so on. Off in a normal build: a
                                            // session's worth of them is noise, and nothing in the
                                            // game reads them. Set it to true to watch the test
                                            // decide, which is how a wrong call is reported as
                                            // evidence rather than as a symptom.
int g_airLogLines = 0;                      // where a wheel's tire is off and back on, written to the
                                            // log with the numbers that decided it, so a wrong call
                                            // can be reported as evidence rather than as a symptom
constexpr int    kAirLogLinesMax   = 600;   // (a whole session stays quiet)
int g_airReadLines = 0;                     // the same numbers, written whatever the state does: the
                                            // first twenty seconds of riding, and every tenth second
                                            // while any fall is being read, so the whole of a jump the
                                            // user has just made is in the log beside the marks it made
constexpr double kAirReadEvery     = 0.1;   // s between those lines
constexpr int    kAirReadLinesMax  = 1500;  // (a long session stays quiet; three hundred stopped
                                            // short of the end of the run this was read from, which
                                            // is why the half of it after the view went third person
                                            // had nothing in it to compare against)
double g_airReadTimer = 0;                  // s since the last such line
std::vector<double> g_ground(kGroundSize * kGroundSize, std::numeric_limits<double>::quiet_NaN());
                                            // WorldUp height per cell, game units; not-a-number:
                                            // never ridden near there. Cells are addressed by the
                                            // absolute cell wrapped, so the field reaches over the
                                            // world however big it is: only cells near your wheels
                                            // are ever read, and two places 512 m apart sharing a
                                            // cell says nothing about either of them.
constexpr double kTerrainCellMetres = 0.5;  // m to a cell of the field of ground seen on screen:
                                            // half the ridden one, because this one answers where
                                            // a tire that was never here is: a cell of a whole
                                            // metre on a steep ramp stands half a metre off the
                                            // face it covers, which is more than the tire's own
                                            // clearance, and the answer would be wrong on ramps.
constexpr int kTerrainSize        = 1024;   // cells to a side (512 m over, past a track)
std::vector<double> g_terrain(kTerrainSize * kTerrainSize, std::numeric_limits<double>::quiet_NaN());
                                            // The terrain's own WorldUp height per cell, game units
                                            // - written from what the game drew of the terrain this
                                            // frame (see FillTerrainFromDepth), so unlike the field
                                            // above it knows the ground ahead of you, where your
                                            // tires haven't been: that's what answers for a wheel
                                            // the game never renders, in first person. Not-a-number:
                                            // not on screen this frame and never seen since.

size_t GroundSlot(long long i, long long j)
{
    return static_cast<size_t>(MarkWrap(i, kGroundSize)) * static_cast<size_t>(kGroundSize) +
           static_cast<size_t>(MarkWrap(j, kGroundSize));
}

void ClearGroundField()
{
    std::fill(g_ground.begin(), g_ground.end(), std::numeric_limits<double>::quiet_NaN());
}

// The ground's height along WorldUp at the field's cell coordinates cu, cv: a local average of
// the known cells around it, nearer ones weighted far more, its window grown until it has any
// to say (out to 17x17 cells, 17 m). False where nothing was ever ridden that near.
bool GroundHeightAt(double cu, double cv, double& height)
{
    const long long i0 = static_cast<long long>(std::floor(cu));
    const long long j0 = static_cast<long long>(std::floor(cv));
    for (int span = 1; span <= 8; span *= 2) {
        double sum = 0, wsum = 0;
        for (long long j = j0 - span; j <= j0 + span; ++j) {
            for (long long i = i0 - span; i <= i0 + span; ++i) {
                const double known = g_ground[GroundSlot(i, j)];
                if (std::isnan(known))
                    continue;
                const double du = static_cast<double>(i) - cu, dv = static_cast<double>(j) - cv;
                const double w = std::exp(-0.5 * (du * du + dv * dv));
                sum += known * w;
                wsum += w;
            }
        }
        if (wsum > 0.0) {
            height = sum / wsum;
            return true;
        }
    }
    return false;
}

// This frame's contact goes into the cell it sits in, but never far above the ground known all
// around it: the ground doesn't move, so a sample that high above its neighbours is a tire flying
// over them, and letting it in would raise the ground there for the rest of the session - and then
// keep calling that tire down, which is what pressed ruts in the air. Downward steps are let
// through whatever their steepness (the ground only gets lower as you ride down a drop or into a
// whoop's underside), so those stay honest.
void GroundWrite(double cu, double cv, double height)
{
    const long long i0 = static_cast<long long>(std::floor(cu));
    const long long j0 = static_cast<long long>(std::floor(cv));
    double highest = 0;
    bool haveNear = false;
    for (long long j = j0 - 1; j <= j0 + 1; ++j) {
        for (long long i = i0 - 1; i <= i0 + 1; ++i) {
            const double knownNear = g_ground[GroundSlot(i, j)];
            if (std::isnan(knownNear))
                continue;
            if (!haveNear || knownNear > highest)
                highest = knownNear;
            haveNear = true;
        }
    }
    if (haveNear && height > highest + kAirClimbMetres * g_unitsPerMetre)
        return;  // too far above the ground around here to be ground: a tire in the air
    double& cell = g_ground[GroundSlot(i0, j0)];
    cell = std::isnan(cell) ? height : cell + (height - cell) * kGroundKeep;
}

// What the game drew of the terrain this frame, turned into world heights (see FillTerrainFromDepth
// below): the ground your tires haven't been on yet, which is what makes the air test answer for a
// wheel the game never renders. The window's depth is the terrain as of the end of the terrain pass
// (before your rider, your bike and the props are drawn over it), so the height it gives is the
// bare ground, whatever stands on it not asked of it. A wheel resting on a prop is then read above
// the ground there - which is why the two fields are combined with the higher of the two answers,
// the ridden one having recorded the prop.
size_t TerrainSlot(long long i, long long j)
{
    return static_cast<size_t>(MarkWrap(i, kTerrainSize)) * static_cast<size_t>(kTerrainSize) +
           static_cast<size_t>(MarkWrap(j, kTerrainSize));
}

void ClearTerrainField()
{
    std::fill(g_terrain.begin(), g_terrain.end(), std::numeric_limits<double>::quiet_NaN());
}

// The seen terrain's height at the field's cell coordinates: the highest of the known cells in the
// 3x3 around it. Highest rather than an average because the question this answers is whether a tire
// clears the surface: a face that runs past the cell to one side is as much under the tire as the
// cell itself, and an average of a ramp's two sides would sit above the ground the tire is on and
// call it down.
bool TerrainHeightAt(double cu, double cv, double& height)
{
    const long long i0 = static_cast<long long>(std::floor(cu));
    const long long j0 = static_cast<long long>(std::floor(cv));
    bool known = false;
    for (long long j = j0 - 1; j <= j0 + 1; ++j) {
        for (long long i = i0 - 1; i <= i0 + 1; ++i) {
            const double seen = g_terrain[TerrainSlot(i, j)];
            if (std::isnan(seen))
                continue;
            if (!known || seen > height)
                height = seen;
            known = true;
        }
    }
    return known;
}

constexpr int    kSampleWidth   = 160;    // samples across the window
constexpr int    kSampleHeight  = 90;     // and down
constexpr double kTerrainGrazing = 0.12;  // a line of sight this close to the ground: dropped
constexpr double kTerrainFarMetres = 60;  // m from the camera a sample may be and still be your
                                          // ground: past it the depth is either the far plane, a
                                          // hill over the sky, or too coarse to say anything where
                                          // your wheels are
std::vector<float> g_sample(kSampleWidth * kSampleHeight, 0.0f);
bool g_sampleLogged = false;              // the first fill logs what it made, once, with g_airLogging
bool g_sampleQuiet  = false;              // nothing accepted for a whole spell: stop trying
int  g_sampleNothing = 0;                 // frames in a row that took no sample at all

// Puts one frame's depth samples where they are in the world (the frame's inverse projection and
// view). False if none of them was ground.
bool PlaceSamples(const float* samples, const double invP[16], const double view[16])
{
    const Vec3 up = WorldUp();
    Vec3 au, av;
    PlaneAxes(au, av);
    const double cell = kTerrainCellMetres * g_unitsPerMetre;
    const Vec3 eyePosition = CameraPosition(view);
    int taken = 0;
    for (int sy = 0; sy < kSampleHeight; ++sy) {
        const double y = (static_cast<double>(sy) + 0.5) / kSampleHeight * 2.0 - 1.0;
        for (int sx = 0; sx < kSampleWidth; ++sx) {
            const float depth = samples[sy * kSampleWidth + sx];
            if (depth >= 0.999999f)
                continue;  // the sky: no ground there
            const double x = (static_cast<double>(sx) + 0.5) / kSampleWidth * 2.0 - 1.0;
            const double z = static_cast<double>(depth) * 2.0 - 1.0;
            const double cw = invP[3] * x + invP[7] * y + invP[11] * z + invP[15];
            if (std::fabs(cw) < 1e-9)
                continue;
            Vec3 eye{ (invP[0] * x + invP[4] * y + invP[8] * z + invP[12]) / cw,
                      (invP[1] * x + invP[5] * y + invP[9] * z + invP[13]) / cw,
                      (invP[2] * x + invP[6] * y + invP[10] * z + invP[14]) / cw };
            const double length = Length(eye);
            if (length < 1e-6)
                continue;
            // Where the line of sight meets the ground: dropped when it grazes it (see above).
            const Vec3 sight = EyeToWorld(view, Vec3{ eye.x / length, eye.y / length, eye.z / length });
            if (std::fabs(Dot(sight, up)) < kTerrainGrazing)
                continue;
            const Vec3 here = eyePosition + EyeToWorld(view, eye);  // the view is the camera's own: the
                                                                     // rotation applied, plus the eye
            const double height = Dot(here, up);
            if (Length(here - eyePosition) > kTerrainFarMetres * g_unitsPerMetre)
                continue;  // the far plane, or a hill seen over the sky: not near your wheels
            g_terrain[TerrainSlot(static_cast<long long>(std::floor(Dot(here, au) / cell)),
                                 static_cast<long long>(std::floor(Dot(here, av) / cell)))] = height;
            ++taken;
        }
    }
    if (taken == 0) {
        if (++g_sampleNothing > 90 && !g_sampleQuiet) {
            g_sampleQuiet = true;  // this game's depth can't be read: the ridden field answers alone
            Log("postfx: the ground seen on screen is never read; the air test keeps to what you've "
                "ridden and to the fall of your bike");
        }
        return false;
    }
    g_sampleNothing = 0;
    if (g_airLogging && !g_sampleLogged) {
        g_sampleLogged = true;
        Log("postfx: the ground is read from what the game drew of the terrain: %d samples a frame", taken);
    }
    return true;
}

constexpr GLenum kStreamRead              = 0x88E1;
constexpr GLenum kSyncGpuCommandsComplete = 0x9117;
constexpr GLenum kAlreadySignaled         = 0x911A;
constexpr GLenum kConditionSatisfied      = 0x911C;

bool CanReadLater()
{
    return g_gl.FenceSync && g_gl.ClientWaitSync && g_gl.DeleteSync && g_gl.GetBufferSubData;
}

// The reads of earlier frames the GPU has finished, oldest first, placed in the world (never
// waiting for one that isn't done: it's taken next frame). True if any of them had ground.
bool TakeFinishedReads()
{
    bool any = false;
    for (int n = 0; n < Resources::kSampleReads; ++n) {
        const int i = (g_res.sampleNext + n) % Resources::kSampleReads;  // the oldest first
        Resources::SampleRead& r = g_res.sampleReads[i];
        if (!r.fence)
            continue;
        const GLenum state = g_gl.ClientWaitSync(r.fence, 0, 0);
        if (state != kAlreadySignaled && state != kConditionSatisfied)
            break;  // not done yet (and the ones after it aren't either)
        g_gl.DeleteSync(r.fence);
        r.fence = nullptr;
        g_gl.BindBuffer(kPixelPackBuffer, r.buffer);
        g_gl.GetBufferSubData(kPixelPackBuffer, 0, kSampleWidth * kSampleHeight * sizeof(float), g_sample.data());
        any = PlaceSamples(g_sample.data(), r.invProj, r.view) || any;
    }
    return any;
}

// Turns the terrain's depth into heights in the world. The depth is downsampled to a small texture
// and read back (a few thousand samples: the whole window would be), and each sample is put where
// it is in the world: the window's depth unprojected through the frame's projection and view, the
// same reconstruction the shaders do. Samples where the line of sight runs along the ground rather
// than meeting it are dropped: there a whole metre of height follows from a rounding of the depth,
// so they'd write ground metres away from where it is. The read goes into a buffer the GPU fills
// when it gets there, and is taken a frame or two later: reading it straight away made the game
// wait for the GPU to finish everything before it, every frame (half the frame rate on some PCs).
bool FillTerrainFromDepth()
{
    if (g_res.sampleFailed || g_sampleQuiet || !g_frame.haveView || g_res.terrainFbo == 0 ||
        g_res.width <= 0 || g_res.height <= 0)
        return false;
    // The depth is read in whatever format the depth textures were made in, and remade if that
    // changed with the window.
    const int packing = g_res.depthMode <= 1 ? 1 : 0;
    if (g_res.sampleDepth == 0 || g_res.samplePacking != packing) {
        if (g_res.sampleDepth)
            g_gl.DeleteTextures(1, &g_res.sampleDepth);
        g_res.sampleDepth = packing ? NewTexture(kDepth24Stencil8, kSampleWidth, kSampleHeight, kDepthStencil,
                                                 kUnsignedInt248, GL_NEAREST)
                                    : NewTexture(kDepthComponent24, kSampleWidth, kSampleHeight, GL_DEPTH_COMPONENT,
                                                 GL_UNSIGNED_INT, GL_NEAREST);
        if (!g_res.sampleFbo)
            g_gl.GenFramebuffers(1, &g_res.sampleFbo);
        g_gl.BindFramebuffer(kFramebuffer, g_res.sampleFbo);
        g_gl.FramebufferTexture2D(kFramebuffer, packing ? kDepthStencilAttachment : kDepthAttachment,
                                  GL_TEXTURE_2D, g_res.sampleDepth, 0);
        g_res.samplePacking = packing;
        if (!g_res.sampleDepth || g_gl.CheckFramebufferStatus(kFramebuffer) != kFramebufferComplete) {
            g_res.sampleFailed = true;
            Log("postfx: the ground can't be read from what the game drew of the terrain");
            return false;
        }
    }
    const float* proj = g_haveProjCamera ? g_projCamera : (g_haveProjOther ? g_projOther : nullptr);
    if (!proj)
        return false;
    double P[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, invP[16];
    for (int i = 0; i < 16; ++i)
        P[i] = proj[i];
    if (!Invert(P, invP))
        return false;

    GLint packBuffer = 0;
    g_gl.GetIntegerv(kPixelPackBufferBinding, &packBuffer);
    const bool later = CanReadLater();
    bool placed = later && TakeFinishedReads();

    // This frame's read: into the next free buffer (none free: skipped this frame, never waited for).
    Resources::SampleRead* slot = nullptr;
    if (later) {
        Resources::SampleRead& r = g_res.sampleReads[g_res.sampleNext];
        if (!r.fence) {
            if (!r.buffer) {
                g_gl.GenBuffers(1, &r.buffer);
                g_gl.BindBuffer(kPixelPackBuffer, r.buffer);
                g_gl.BufferData(kPixelPackBuffer, kSampleWidth * kSampleHeight * sizeof(float), nullptr, kStreamRead);
            }
            slot = &r;
        }
    }
    if (!later || slot) {
        g_gl.BindFramebuffer(kReadFramebuffer, g_res.terrainFbo);
        DrainErrors();  // the read below is checked: nothing of its own counted beforehand
        g_gl.BindFramebuffer(kDrawFramebuffer, g_res.sampleFbo);
        g_gl.BlitFramebuffer(0, 0, g_res.width, g_res.height, 0, 0, kSampleWidth, kSampleHeight,
                             GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        g_gl.BindFramebuffer(kReadFramebuffer, g_res.sampleFbo);
        if (slot) {
            g_gl.BindBuffer(kPixelPackBuffer, slot->buffer);
            g_gl.ReadPixels(0, 0, kSampleWidth, kSampleHeight, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        } else {
            g_gl.BindBuffer(kPixelPackBuffer, 0);
            g_gl.ReadPixels(0, 0, kSampleWidth, kSampleHeight, GL_DEPTH_COMPONENT, GL_FLOAT, g_sample.data());
        }
        if (g_gl.GetError() != GL_NO_ERROR) {
            g_gl.BindBuffer(kPixelPackBuffer, packBuffer);
            g_res.sampleFailed = true;
            Log("postfx: the ground can't be read from what the game drew of the terrain");
            return false;
        }
        if (slot) {
            slot->fence = g_gl.FenceSync(kSyncGpuCommandsComplete, 0);
            for (int i = 0; i < 16; ++i) {
                slot->invProj[i] = invP[i];
                slot->view[i] = g_frame.rendered[i];
            }
            g_res.sampleNext = (g_res.sampleNext + 1) % Resources::kSampleReads;
        } else {
            placed = PlaceSamples(g_sample.data(), invP, g_frame.rendered);
        }
    }
    g_gl.BindBuffer(kPixelPackBuffer, packBuffer);
    return placed;
}

// After a cleared strip let fresh ground into the window, press back from the cache what was
// ruted there: every cached segment crossing the strip [from, from + count) (unwrapped texels)
// of the map's columns (axis 0) or rows (axis 1). The blocks are the index: a block whose box
// misses the strip is skipped whole, so a whole session's ruts stay cheap to re-press whatever
// the count. The stamps are the ones a fresh rut gets, so the re-pressed mark is identical to
// what was there before the ground was wiped - no seam, no blur, whatever the turn. Max blend
// cannot lower what the strip still holds, so a segment sticking out past the strip simply
// re-lays its own rut where it also lies. The cache holds the whole session, so this is what
// carries a lap's ruts across many wrap-arounds.
void RepressCache(int axis, long long from, long long count)
{
    if (!g_mark.set || count <= 0)
        return;
    // Joined to the last strip of this axis while that one hasn't been started and is still thin.
    for (auto it = g_repress.rbegin(); it != g_repress.rend(); ++it) {
        if (it->axis != axis)
            continue;
        if (it->cache == 0 && it->block == 0 && it->hi - it->lo < kRepressChunk) {
            if (it->hi == from) {
                it->hi = from + count;
                return;
            }
            if (it->lo == from + count) {
                it->lo = from;
                return;
            }
        }
        break;
    }
    RepressStrip strip;
    strip.axis = axis;
    strip.lo = from;
    strip.hi = from + count;
    strip.queued = g_markFrameNo;
    g_repress.push_back(strip);
}

// Presses back what the queued strips are owed, a budget's worth a frame (see RepressStrip). Each
// strip is held to the window as it is now: ground that has left the window since isn't pressed
// (it would land, wrapped, on the ground at the window's other side).
void ProcessRepress()
{
    g_markRepressed = 0;
    if (!g_mark.set || g_repress.empty())
        return;
    MarkMap& m = g_mark;
    Program& stamp = g_res.programs[kStamp];
    g_gl.Enable(GL_BLEND);
    g_gl.BlendEquationSeparate(kFuncMax, kFuncMax);
    g_gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    g_gl.Uniform1f(stamp["u_check"], 0.0f);  // the cache only holds ruts the tires really pressed
    g_gl.Uniform1f(stamp["u_fallback"], 1.0f);
    // A block's box holds its segments' contacts only, so the test is widened by the widest
    // rut's half (the rear's, the wider of the two): a segment reaching into the strip from
    // outside the box still gets pressed. In the map's texels.
    const double widest = 0.12 * g_unitsPerMetre / m.texel;
    // (A backlog - riding fast past a lot of ruts - gets through faster.)
    const size_t scale = std::min<size_t>(1 + g_repress.size() / 8, 3);
    const int grooveBudget = static_cast<int>(kRepressGrooves * scale);
    const size_t walkBudget = kRepressWalk * scale;
    int grooves = 0;
    size_t walked = 0;
    bool out = false;
    for (size_t at = 0; at < g_repress.size() && !out;) {
        RepressStrip& st = g_repress[at];
        const long long wLo = st.axis == 0 ? m.originU : m.originV;
        const double stripLo = static_cast<double>(std::max(st.lo, wLo));
        const double stripHi = static_cast<double>(std::min(st.hi, wLo + m.size));
        if (stripHi <= stripLo) {
            g_repress.erase(g_repress.begin() + static_cast<std::ptrdiff_t>(at));  // (its ground has left the window)
            continue;
        }
        // Still being joined (the last of its axis, thin, and young): left for a later frame.
        bool newest = true;
        for (size_t k = at + 1; k < g_repress.size() && newest; ++k)
            newest = g_repress[k].axis != st.axis;
        if (newest && st.cache == 0 && st.block == 0 && st.hi - st.lo < kRepressChunk && g_markFrameNo - st.queued < 30) {
            ++at;
            continue;
        }
        // (Every other bike's ruts first, then your front's, your rear's last: each groove fills in
        // the ruts beside it, so yours come out on top, as they were ridden last where you are.)
        static constexpr int kOrder[3] = { 2, 1, 0 };
        for (; st.cache < 3 && !out; ++st.cache, st.block = 0) {
            const int ci = kOrder[st.cache];
            const std::deque<MarkBlock>& blocks = g_markCache[ci];
            st.block = std::max(st.block, g_markPopped[ci]);
            for (; st.block < g_markPopped[ci] + blocks.size(); ++st.block) {
                if (grooves >= grooveBudget || walked >= walkBudget) {
                    out = true;
                    break;
                }
                const MarkBlock& b = blocks[st.block - g_markPopped[ci]];
                const double bLo = st.axis == 0 ? b.minU - widest : b.minV - widest;
                const double bHi = st.axis == 0 ? b.maxU + widest : b.maxV + widest;
                if (bHi < stripLo || bLo > stripHi)
                    continue;  // the whole block is clear of the strip: its segments can't be in it
                walked += b.segs.size();
                for (const MarkSeg& sg : b.segs) {
                    const double hw = sg.halfWidth * g_unitsPerMetre / m.texel;
                    const double lo = st.axis == 0 ? std::fmin(sg.au, sg.bu) - hw : std::fmin(sg.av, sg.bv) - hw;
                    const double hi = st.axis == 0 ? std::fmax(sg.au, sg.bu) + hw : std::fmax(sg.av, sg.bv) + hw;
                    if (hi < stripLo || lo > stripHi)
                        continue;
                    // Nor one off the window the other way: it would wrap round onto the ground at
                    // the window's opposite side and press a rut there that was never ridden.
                    const double oLo = st.axis == 0 ? std::fmin(sg.av, sg.bv) - hw : std::fmin(sg.au, sg.bu) - hw;
                    const double oHi = st.axis == 0 ? std::fmax(sg.av, sg.bv) + hw : std::fmax(sg.au, sg.bu) + hw;
                    const double oWin = static_cast<double>(st.axis == 0 ? m.originV : m.originU);
                    if (oHi < oWin || oLo > oWin + m.size)
                        continue;
                    Groove(g_res.marks, m, sg.au, sg.av, sg.bu, sg.bv, hw,
                           ci == 0 ? kMarkRearStrength : kMarkFrontStrength, sg.arcA, sg.fadeOut);
                    ++grooves;
                }
            }
            if (out)
                break;  // (keep the cache this strip's walk is in)
        }
        if (!out)
            g_repress.erase(g_repress.begin() + static_cast<std::ptrdiff_t>(at));
    }
    g_markRepressed = grooves;
    g_gl.Disable(GL_BLEND);
}

// Your bike's own vertical acceleration, in g (negative falling, free fall: -1) ------------------
// The wheels aren't rigid: they hang off the bike and the terrain, and in first person they're
// stood in from it at all, so the contact's path is the noisiest sample there is. Your bike's own
// origin is a single rigid body, so where the tracker has it, its height's acceleration is the
// same answer steadier: the same least-squares fit motion.h keeps for the same reason. It's fit
// here from the pose's positions rather than read off the pose, which fits over metres while the
// world is in the mod's units, and the two signals then can't drift apart in scale.
MotionFit    g_bikeAirFit;                    // the bike's origins, to fit its fall through
Vec3         g_bikeLast;                      // its origin last frame, to see a cut
double       g_bikeFallG = 0;                 // that acceleration along WorldUp, in g
bool         g_bikeHave = false;              // whether it was fit this frame
void UpdateBikeFall()
{
    const bike::Pose& pose = bike::Current();
    if (!pose.tracked) {
        g_bikeAirFit.Reset();
        g_bikeHave = false;
        g_bikeFallG = 0;
        return;
    }
    const Vec3 here{ pose.worldPosition[0], pose.worldPosition[1], pose.worldPosition[2] };
    if (g_bikeHave && std::fabs(Dot(here, WorldUp()) -
                        Dot(g_bikeLast, WorldUp())) > kAirTeleportMetres * g_unitsPerMetre)
        g_bikeAirFit.Reset();  // a cut or a respawn: don't fit the jump between the two places
    g_bikeLast = here;
    g_bikeAirFit.Add(g_markTime, here);
    Vec3 velocity, accel;
    if (!g_bikeAirFit.Estimate(kAirFitWindow, velocity, accel)) {
        g_bikeHave = false;
        g_bikeFallG = 0;
        return;
    }
    const double gravity = kGravity * g_unitsPerMetre;  // the game's gravity, game units/s^2
    g_bikeFallG = Clamp(Dot(accel, WorldUp()) / gravity, -kAirMaxG, kAirMaxG);
    g_bikeHave = true;
}

// Moves the window with you, re-presses from the cache what the clearing wiped where fresh
// ground came in, and presses this frame's grooves into the map. Returns whether the map is
// usable.
// Whether a wheel's tire is on the ground this frame, and where its rut may press -------------
// What every bike's paths did this frame in the marks (for the trace: WheelTraceText).
struct LobbyMarkStats {
    int    segments = 0, yours = 0, clear = 0, step = 0, outside = 0, pressed = 0;
    double nearest[2] = { -1, -1 };  // m from each of your wheels' contacts to the nearest path's end (-1: none)
};
LobbyMarkStats g_lobbyStats;

bool UpdateMarks(double dt)
{
    const Config& cfg = GetConfig();
    if (cfg.tireMarks <= 0.0 || !g_frame.haveView || !EnsureMarks())
        return false;
    const auto started = std::chrono::steady_clock::now();
    g_markTime += dt;  // the clock the fits' samples are stamped with
    Program& p = Use(kStamp);
    (void)p;
    g_gl.Enable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_BLEND);
    if (!g_res.marksCleared) {
        ClearMarks(g_res.marks, 0, 0, g_mark.size, g_mark.size);
        g_res.marksCleared = true;
        g_mark.set = false;
    }
    Vec3 u, v;
    PlaneAxes(u, v);
    const Vec3 eye = CameraPosition(g_frame.raw);
    const double texel = 0.04 * g_unitsPerMetre;  // where the map's texels are, game units
    if (g_unitsLocked && std::fabs(texel - g_mark.texel) > 0.1 * texel) {
        // The unit has settled somewhere other than where the map was laid out: start over at
        // 4 cm. Gated on the unit being locked: while it's still being measured it drifts a
        // little every frame, and re-laying on those blips wiped every mark mid-ride. The cache
        // goes with it: its widths are in the unit the map was laid out in.
        g_mark.texel = texel;
        ClearMarks(g_res.marks, 0, 0, g_mark.size, g_mark.size);
        g_mark.set = false;
        for (Wheel& w : g_wheels) {
            w.haveLast = false;
            w.fit.Reset();        // its positions are in game units as well
            w.heightVel = 0;
            w.heightAccel = 0;
            w.fallG = 0;
            w.airScore = 0;
            w.airPeak = 0;
            w.grounded = true;
        }
        ClearMarkCache();
        ClearGroundField();   // its heights are in game units as well
        ClearTerrainField();
        g_bikeAirFit.Reset();
        g_bikeHave = false;
        g_bikeFallG = 0;
    }
    MarkMap& m = g_mark;
    const double uCam = Dot(eye, u), vCam = Dot(eye, v);
    const long long cu = static_cast<long long>(std::floor(uCam / m.texel));
    const long long cv = static_cast<long long>(std::floor(vCam / m.texel));
    if (!m.set) {
        m.originU = cu - m.size / 2;
        m.originV = cv - m.size / 2;
        m.set = true;
        ClearMarks(g_res.marks, 0, 0, m.size, m.size);
        g_repress.clear();  // (the strips queued were of the window as it was)
    }
    // Stay centred on the camera, and move only as far as that asks, at most a sixteenth of the
    // map at a time (the old snap to the centre at every threshold cleared up to a quarter of
    // the map in one go, of ground only a quarter behind you - visible again straight after a
    // U-turn). Whatever the clearing wipes there, the cache re-presses right after, so a mark
    // can never be cut where ground is reused: a U-turn comes back to last lap's ruts whole.
    // Unless every rut ridden so far fits in the window with room to spare (g_ridden): then it holds
    // still around them - re-centred on them only when they've outgrown where it is - and nothing
    // the cache holds ever needs pressing back.
    ++g_markFrameNo;
    const long long slack = m.size / 16;
    long long shiftU = 0, shiftV = 0;
    const long long margin = static_cast<long long>(kMarkFitMargin * g_unitsPerMetre / m.texel);
    bool fit = false;
    if (g_ridden.have) {
        const auto Place = [&](double lo, double hi, long long cam, long long origin, long long& want) {
            if (std::ceil(hi) - std::floor(lo) + 2 * margin > m.size)
                return false;
            const auto Holds = [&](long long o) {
                return o <= std::floor(lo) - margin && o + m.size >= std::ceil(hi) + margin &&
                       cam >= o + margin && cam <= o + m.size - margin;
            };
            want = Holds(origin) ? origin : static_cast<long long>(std::floor(0.5 * (lo + hi))) - m.size / 2;
            return Holds(want);  // (the camera off past the ruts: followed instead)
        };
        long long wantU = 0, wantV = 0;
        fit = Place(g_ridden.loU, g_ridden.hiU, cu, m.originU, wantU) && Place(g_ridden.loV, g_ridden.hiV, cv, m.originV, wantV);
        if (fit) {
            shiftU = wantU - m.originU;
            shiftV = wantV - m.originV;
        }
    }
    if (!fit) {
        // Following the camera: stay centred on it, and move only as far as that asks, at most a
        // sixteenth of the map at a time (the old snap to the centre at every threshold cleared up
        // to a quarter of the map in one go, of ground only a quarter behind you - visible again
        // straight after a U-turn). Whatever the clearing wipes there, the cache presses back, so a
        // mark can never be cut where ground is reused: a U-turn comes back to last lap's ruts whole.
        const long long du = cu - m.originU, dv = cv - m.originV;
        if (du < m.size / 2 - slack || du > m.size / 2 + slack)
            shiftU = du > m.size / 2 + slack ? du - (m.size / 2 + slack) : du - (m.size / 2 - slack);
        if (dv < m.size / 2 - slack || dv > m.size / 2 + slack)
            shiftV = dv > m.size / 2 + slack ? dv - (m.size / 2 + slack) : dv - (m.size / 2 - slack);
    }
    if (fit != g_markFit) {
        g_markFit = fit;
        Log("postfx: tire marks: %s", fit ? "every rut ridden so far fits in the map: it holds still around them (nothing to press back)"
                                          : "the ruts ridden reach wider than the map: it follows the camera");
    }
    if (shiftU != 0) {
        const long long from = shiftU > 0 ? m.originU + m.size : m.originU + shiftU;
        const long long count = shiftU > 0 ? shiftU : -shiftU;
        ClearBand(0, g_res.marks, m, from, count);
        RepressCache(0, from, count);
        m.originU += shiftU;
    }
    if (shiftV != 0) {
        const long long from = shiftV > 0 ? m.originV + m.size : m.originV + shiftV;
        const long long count = shiftV > 0 ? shiftV : -shiftV;
        ClearBand(1, g_res.marks, m, from, count);
        RepressCache(1, from, count);
        m.originV += shiftV;
    }
    ProcessRepress();
    // The grooves: from each wheel's last contact with the ground to this one, pressed only where
    // the tire is down on it - and both answers must agree: the terrain's depth where the game drew
    // that wheel (exact, but it exists only there), and this wheel's own world-space answer from its
    // height (below), which is what answers in first person, where the wheels sit behind and under
    // the camera and are hardly drawn at all. As the tire rises so does the camera, so the one pixel
    // the screen does read of a tire in the air holds far-ahead ground at the tire's height and
    // calls it down; the world's answer has to hold as well, so it gates the whole groove.
    g_gl.Enable(GL_BLEND);
    g_gl.BlendEquationSeparate(kFuncMax, kFuncMax);
    g_gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    Program& stamp = g_res.programs[kStamp];
    const bool checkGround = g_frame.terrainSnapshot && g_res.terrainDepth != 0;
    Bind(stamp, "u_ground", 0, checkGround ? g_res.terrainDepth : 0);
    g_gl.Uniform1f(stamp["u_check"], checkGround ? 1.0f : 0.0f);
    g_gl.Uniform1f(stamp["u_groundTol"], static_cast<float>(0.15 * g_unitsPerMetre));
    Vec(stamp, "u_upView", Normalize(WorldToEye(g_frame.rendered, WorldUp())));
    const Vec3 up = WorldUp();
    const double gravity = kGravity * g_unitsPerMetre;  // the game's gravity, game units/s^2
    // Your bike's own fall, before the wheels' so both answers are this frame's.
    UpdateBikeFall();
    for (int i = 0; i < 2; ++i) {
        Wheel& w = g_wheels[i];
        if (!w.on) {
            // A moment off the tracker (a frame without a heading, a part that went away)
            // keeps the wheel's last contact: coming back, the rut joins up instead of leaving
            // a gap. Only a wheel off for over three quarters of a second forgets it.
            w.off += dt;
            if (w.off > 0.75) {
                w.haveLast = false;
                w.runArc = 0.0;  // the rut's forgotten: its run of ruts begins over with it
            }
            w.fit.Reset();     // its contact isn't trusted while the wheel is off the tracker
            w.fallG = 0;
            w.heightAccel = 0;
            continue;
        }
        w.off = 0.0;
        const Vec3 contact = w.contact;
        const double tu = Dot(contact, u) / m.texel, tv = Dot(contact, v) / m.texel;
        // Where its tire is on the ground, in the world (see the height field above, and your bike's
        // fall just above this): a state that holds rather than a number that flips, read only from
        // the tire's own path and your bike's own height - it asks nothing of the screen, which is
        // what makes it work in first person, where the wheels sit behind and under the camera and
        // are barely drawn at all, and the one pixel the screen does read of a rising tire holds
        // ground far ahead at the tire's height and would call it down. A tire rolling follows the
        // face it's on, a plane, so its height hardly accelerates; a tire in the air traces a
        // parabola at the game's gravity all through the flight. Both are fit, and the fall is only
        // believed when they both say so: alone, the wheel's own path said fall where there was
        // none (a rough stretch of ground, in 1st person where the wheels are stood in from your
        // bike and their height swings as it pitches) and none where there was one, so first
        // person took the tires off the ground late and put them back in the middle of a flight,
        // and the rut carried on being pressed under a wheel in the air while third person,
        // reading the drawn rider's wheels, got the same jump right.
        // Down to off the ground: that fall, held to kAirScoreAir - on its own, whatever the ground
        // near there says - or that far above the ground near here at once, which only a launch does.
        // Off to down: the fall's hold gone under kAirScoreDown, which a landing does by itself since
        // nothing is falling any more then (kAirLandedDrain), or the tire at the ground near here
        // having lost half of the hold its flight reached (kAirScoreAir only: that's the number the
        // flip asks for too, and the state flapped back through the lip within two frames of the
        // launch, which put the rut back on under a wheel in the air).
        {
            const bool wasGrounded = w.grounded;
            const double cell = kGroundCellMetres * g_unitsPerMetre;  // a field cell, game units
            const double height = Dot(contact, up);
            w.fit.Add(g_markTime, contact);
            Vec3 fitVel, fitAcc;
            if (w.fit.Estimate(kAirFitWindow, fitVel, fitAcc)) {
                w.heightVel = Dot(fitVel, up);
                w.heightAccel = Dot(fitAcc, up);
                w.fallG = Clamp(w.heightAccel / gravity, -kAirMaxG, kAirMaxG);
            } else {
                // Not enough samples yet (the wheel just came back, the fit just started over):
                // no fall is being read, so nothing can be decided from it.
                w.heightAccel = 0;
                w.fallG = 0;
            }
            // The deeper of the tire's own fall and your bike's: a tire rolling follows the face
            // it's on, a plane, so its height hardly accelerates, and a tire in the air traces a
            // parabola accelerating at the game's gravity through the whole flight, the launch's
            // rise through the drop. Your bike is the same parabola read off a rigid body rather
            // than off a wheel that hangs off it and swings with its pitch, which is the difference
            // that makes first person agree with third: read from the wheel alone, first person
            // saw a fall where there was none and none where there was one, so the rut carried on
            // under a wheel in the air.
            const double fallG = g_bikeHave ? std::fmin(w.fallG, g_bikeFallG) : w.fallG;
            // Both must agree that it is a free fall before one is believed: a rough stretch of
            // ground puts a whole gravity of fall into a wheel's own fitted path while your bike,
            // which is what actually left or didn't leave the ground, stays shallow, and the tire
            // was at the ground near there every frame of it.
            const bool bothFall = w.fallG < -kAirFreeFallG &&
                                  (!g_bikeHave || g_bikeFallG < -kAirFreeFallG);
            // Out of free fall at all, in both readings: the landing. Not the slam the contact makes
            // (see the note by kAirLandedDrain).
            const bool outOfFall = w.fallG > -kAirBackG && (!g_bikeHave || g_bikeFallG > -kAirBackG);
            const double fcu = Dot(contact, u) / cell, fcv = Dot(contact, v) / cell;
            double ridden = 0;
            const bool riddenKnown = GroundHeightAt(fcu, fcv, ridden);  // what your tires pressed here
            const double seenCell = kTerrainCellMetres * g_unitsPerMetre;
            double seen = 0;
            const bool seenKnown = TerrainHeightAt(Dot(contact, u) / seenCell, Dot(contact, v) / seenCell,
                                                   seen);  // the bare ground the game drew here, ahead of you
            // The seen and the ridden heights are never merged into one face, which is what one
            // would reach for to make the answer geometric either way: in the middle of a flight the
            // two disagree, and the seen one is then read as the ground the tire is on. The log of
            // the run this was read from shows what that did (a wheel back on the ground with its
            // own and your bike's fall still a whole gravity deep, its hold drained, the rut re-
            // pressed under it a third of the way through a jump), which is why the seen field is
            // asked only the question it can't get wrong in that direction: does the tire CLEAR the
            // bare ground there. A face standing over it (a ramp, a crate, the tire's own lip) makes
            // that answer no, never a false yes, so this half can only ever add air. Asking it
            // "is the tire on that ground" is the question that lies in flight - what the camera saw
            // at this column isn't the face under a wheel it barely draws - so going down stays with
            // the readings and with the ridden field, which recorded the face your tire pressed.
            const double seenGap = height - seen;      // game units above the ground the game drew here
            const double riddenGap = height - ridden;  // and above what your tires pressed here
            // Clear of the ground near here, in either field: only this far above the bare terrain
            // the game drew, or this far above what your tires last pressed, counts as clearing it.
            // It has to be a band rather than a line because the contact isn't a clean sample: the
            // game's suspension compresses a wheel, and a wheel stood in from your bike reads within
            // tens of centimetres of the face it rolls on whatever its pitch, so a rolling wheel has
            // to be allowed that much of slack or it would be read clear all along a rough stretch.
            // Nothing is said the other way here (see the note above about the seen field): going
            // down is the readings' answer, so there's no number for how close to a surface a tire
            // counts as being on it.
            const bool riddenClear = riddenKnown && riddenGap > kAirClearMetres * g_unitsPerMetre;
            // The seen field's clearance only where the ridden one doesn't speak: where your tires
            // have pressed near here, that answer is the nearer face and already says how far the
            // tire is from it, whereas the seen one would read a tire on a bridge or on high ground
            // as clear of the bare terrain the terrain pass drew under it, and hold it off the ground
            // after a few frames of rolling there. Past that face the seen field's the only one that
            // knows the surface at all, which is what answers for a wheel the game never renders.
            const bool seenClear = seenKnown && !riddenKnown && seenGap > kAirClearMetres * g_unitsPerMetre;
            const bool clear = riddenClear || seenClear;
            w.gap = riddenGap;
            w.gapKnown = riddenKnown;
            // The fall's hold, integrated rather than counted at one number: a tire known to clear
            // the ground gains a whole gravity of it whatever its fitted fall says (this is what cuts
            // the rut at the lip rather than a fit's window past it, and what answers in first person
            // where the readings are all the wheel's height has), and one that isn't known to gains
            // only what the fitted fall of the tire and your bike says, and only where both call a
            // free fall. Either way a whole second of it makes a hold of 1. The hold is lost by the
            // readings, not by either height field (see the note by kAirLandedDrain): a height
            // field's idea of where the tire is is never what ends a flight.
            if (clear || std::fabs(fallG) < kAirSpikG)
                w.airScore = Clamp(w.airScore + dt * ((clear ? 1.0
                                                            : (bothFall ? std::fmin(-fallG, 1.0)
                                                                      : 0.0)) * kAirScoreGain -
                                                      (outOfFall ? kAirLandedDrain : kAirScoreDrain)),
                                   0.0, 1.0);
            const bool falling = w.airScore > kAirScoreAir;
            if (w.grounded) {
                if (w.airScore < kAirScoreDown)
                    GroundWrite(fcu, fcv, height);  // only a pressing tire's height goes in
                w.airPeak = 0.0;                    // nothing of a flight to keep while it's down
                if (falling || (riddenKnown && riddenGap > kAirClimbMetres * g_unitsPerMetre))
                    w.grounded = false;  // its height has fallen freely for long enough
            } else if (w.airScore < kAirScoreDown ||
                       (riddenKnown && std::fabs(riddenGap) < kAirGap * g_unitsPerMetre &&
                        w.airScore <= 0.5 * w.airPeak)) {
                // landed: the fall's hold is gone, or the tire is back at the ground it last
                // pressed near here and has lost half of whatever its flight held (asked for the
                // hold to be under kAirScoreAir instead, the same number the flip asks for, the
                // state flapped back through the lip two frames after the launch and pressed the
                // rut on under a wheel in the air). Only the ridden field answers this: the seen
                // one knows the bare terrain ahead of you, which sits under a tire in the middle
                // of a flight, and calling it down there pressed the rut through whole jumps.
                w.grounded = true;
                GroundWrite(fcu, fcv, height);
            } else if (w.airScore > w.airPeak)
                w.airPeak = w.airScore;  // what this flight holds, to be lost by half to land
            if (g_airLogging && w.grounded != wasGrounded && g_airLogLines < kAirLogLinesMax) {
                ++g_airLogLines;
                Log("postfx: tire marks: wheel %d %s the ground: it was %+.2f m above the height of "
                    "the ground near here%s, the deeper of its own and your bike's fall was %+.2f g "
                    "with the fall held to %+.2f (the tire's %+.2f g, the bike's %+.2f g, %s)",
                    i, w.grounded ? "back on" : "off", (riddenKnown ? riddenGap : seenGap) / g_unitsPerMetre,
                    riddenKnown ? "" : (seenKnown ? " (of ground seen on screen, never ridden here)"
                                                  : " (of ground never seen nor ridden this close)"),
                    fallG, w.airScore, w.fallG,
                    g_bikeFallG, g_bikeHave ? "bike tracked" : "bike not tracked");
            }
        }
        if (w.haveLast) {
            const double step = std::hypot(tu - w.lastU, tv - w.lastV) * m.texel;
            if (step > 2.5 * g_unitsPerMetre) {  // a respawn: don't join it up
                w.haveLast = false;
                w.runArc = 0.0;  // ...and a run of ruts begins over with it
            } else if (step < 0.01 * g_unitsPerMetre) {
                continue;  // standing still: keep the last contact
            } else {
                // Rear: a 240 mm rut (the tire digs and throws dirt aside), front: 180 mm, shallower.
                // Stamped at the rut's true world width: the map's texels are 4 cm, so it's
                // several texels across and the fresh line comes out smooth by construction.
                const double halfWidth = (i == 0 ? 0.12 : 0.09) * g_unitsPerMetre;
                const double strength = i == 0 ? kMarkRearStrength : kMarkFrontStrength;
                // How far this wheel's run of ruts has gone, and how far this segment takes it to,
                // in the map's texels of trail counted from the run's own first contact. A run is
                // the stretch of ground the tire has been down on with no break, and its two ends
                // turn in and out over the first and last kMarkRunFade metres of that trail: the
                // turn is read as a distance along the trail rather than held as this segment's
                // own share at its ends, so every segment of a run answers against the same marks
                // on it and a max blend can't step a turn where they overlap on the ground - two
                // straight lines of their own disagree there, and the deeper of the two wins.
                const double arcA = w.runArc;
                const double arcB = arcA + std::hypot(tu - w.lastU, tv - w.lastV);
                // The last contact's own stamp reaches this far past the contact (the rut's half
                // width, plus where the stamp's cover falls to nothing) and holds full depth over
                // it, so the run's end turn is measured from the end of that reach: the run-out
                // then starts where the last press stops, at full depth, and is one straight line
                // down from there. That is why the stamps can't step it, and why a rut that ends
                // where the tire left the ground still reaches to the lip it left off at.
                const double plateau = halfWidth / m.texel + 0.5;
                const double fadeOut = arcA + plateau + m.fadeLen;
                if (w.grounded)
                    w.runArc = arcB;  // this segment is part of the run
                else
                    w.runArc = 0.0;   // in the air: the run is over at here, its turn runs out below
                g_gl.Uniform1f(stamp["u_fallback"], w.grounded ? 1.0f : 0.0f);
                Vec(stamp, "u_eyeA", TransformPoint(g_frame.rendered, w.lastContact));
                Vec(stamp, "u_eyeB", TransformPoint(g_frame.rendered, contact));
                Groove(g_res.marks, m, w.lastU, w.lastV, tu, tv, halfWidth / m.texel, strength, arcA,
                       w.grounded ? kMarkNoFade : fadeOut);
                // Into the cache too: when this ground comes back through the window's edge,
                // the same segment is pressed again, so marks stay wherever you've ridden this
                // session, however many times the window wrapped around since. Ruts the tires
                // didn't really press aren't cached - they'd be re-pressed wherever the ground
                // is reused. Oldest segments go only once the cap is reached.
                if (w.grounded)
                    PushMark(i, MarkSeg{ w.lastU, w.lastV, tu, tv, float(i == 0 ? 0.12 : 0.09),
                                         float(arcA), static_cast<float>(kMarkNoFade) });
                else if (arcA > 0.0) {
                    // The run's end: its last metres of turn lie beyond the last contact, so they
                    // are pressed on past it as a few short segments, each holding less of the
                    // press (and, through the stamp, sitting narrower: a tire leaves the ground
                    // as a wedge). They go on along the way the wheel was going, level with the
                    // last contact, and the stamp's own ground test keeps them to where that
                    // ground really is - over the gap a jump left there is none, so a rut still
                    // ends at the lip it left off at, only without a straight cut across it.
                    // They carry the same distance along the run and the same point its turn runs
                    // out at as the run's other segments, so they turn over as one straight line
                    // from the end of the last contact's reach; and they're cached like every other
                    // segment, so ground coming back through the window's edge returns with the
                    // same run-out.
                    const double tailTexels = kMarkTailStep * g_unitsPerMetre / m.texel;
                    const int tailSegs = static_cast<int>(
                        std::ceil((plateau + m.fadeLen) / tailTexels));
                    Vec3 along = contact - w.lastContact;
                    along = along - up * Dot(along, up);  // the way it was going, in the ground
                    if (Length(along) > 1e-6) {
                        const Vec3 dirW = Normalize(along);
                        double arc = arcA;  // how far along the run this stretch of trail lies
                        for (int k = 0; k < tailSegs; ++k) {
                            const Vec3 pa = w.lastContact + dirW * (kMarkTailStep * k);
                            const Vec3 pb = w.lastContact + dirW * (kMarkTailStep * (k + 1));
                            const double au = Dot(pa, u) / m.texel, av = Dot(pa, v) / m.texel;
                            const double bu = Dot(pb, u) / m.texel, bv = Dot(pb, v) / m.texel;
                            Vec(stamp, "u_eyeA", TransformPoint(g_frame.rendered, pa));
                            Vec(stamp, "u_eyeB", TransformPoint(g_frame.rendered, pb));
                            // Out of the ground's own answer rather than the tire's: the run has
                            // ended, so nothing gates these but the ground where they lie.
                            g_gl.Uniform1f(stamp["u_fallback"], 1.0f);
                            Groove(g_res.marks, m, au, av, bu, bv, halfWidth / m.texel, strength, arc,
                                   fadeOut);
                            PushMark(i, MarkSeg{ au, av, bu, bv, float(i == 0 ? 0.12 : 0.09),
                                                 float(arc), static_cast<float>(fadeOut) });
                            arc += std::hypot(bu - au, bv - av);
                        }
                    }
                }
            }
        }
        w.lastU = tu;
        w.lastV = tv;
        w.lastContact = contact;
        w.haveLast = true;
    }
    // Every other bike's wheels (Config::marksEveryBike, lobby_wheels.h): their paths since the last
    // frame, pressed only where the screen shows the tire on the ground (u_fallback 2) - nothing
    // here knows another bike's air the way your own wheels' fall is read above. Yours are left out
    // (pressed above already), as is anything outside the map's window.
    // Cached as well (g_markCache[2]), in strokes of kOthersStroke, so they come back wherever the
    // window reuses ground, as yours do. The screen's test that decides what's pressed is the GPU's,
    // so what's cached is what two answers here agree was on the ground: the wheel not falling (its
    // contact's own fall, lobby_wheels.cpp) and not clear of the ground seen there. (Cached whole,
    // the ruts under wheels in the air would be pressed back in wherever the cache re-presses.)
    {
        struct Stroke { double su = 0, sv = 0, lu = 0, lv = 0; double seen = 0; double arc = 0; bool have = false; };
        static std::unordered_map<unsigned, Stroke> strokes;  // each wheel's stroke being drawn out
        const auto Flush = [&](Stroke& k) {
            if (k.have && std::hypot(k.lu - k.su, k.lv - k.sv) * m.texel > 0.05 * g_unitsPerMetre)
                PushMark(2, MarkSeg{ k.su, k.sv, k.lu, k.lv, 0.105f,
                                     static_cast<float>(k.arc), static_cast<float>(kMarkNoFade) });
            k.have = false;
            k.arc = 0.0;  // the run broke at here: the next one turns in from its start
        };
        const double seenCell = kTerrainCellMetres * g_unitsPerMetre;
        // At the ground the game drew there: known, and the tire within kOthersAtGround of it. (Asked
        // only "not clear of it", ground never seen counted as down: other riders' jumps - their air
        // missed by their contact's fall, smoothed over the network - were cached, and pressed back as
        // fans of straight lines over every jump once the window moved.)
        const auto AtSeenGround = [&](Vec3 p) {
            double seen = 0;
            return TerrainHeightAt(Dot(p, u) / seenCell, Dot(p, v) / seenCell, seen) &&
                   std::fabs(Dot(p, up) - seen) < kOthersAtGround * g_unitsPerMetre;
        };
        static std::vector<lobby::Segment> others;
        lobby::TakeSegments(others);
        g_lobbyStats = LobbyMarkStats{};
        g_lobbyStats.segments = static_cast<int>(others.size());
        for (const lobby::Segment& s : others)
            for (int i = 0; i < 2; ++i)
                if (g_wheels[i].on) {
                    const double d = Length(Vec3{ s.b[0], s.b[1], s.b[2] } - g_wheels[i].contact) / g_unitsPerMetre;
                    if (g_lobbyStats.nearest[i] < 0 || d < g_lobbyStats.nearest[i])
                        g_lobbyStats.nearest[i] = d;
                }
        if (cfg.marksEveryBike && checkGround) {
            g_gl.Uniform1f(stamp["u_fallback"], 2.0f);
            const double margin = 64.0;
            const double halfWidth = 0.105 * g_unitsPerMetre, strength = 0.85;
            for (const lobby::Segment& s : others) {
                const Vec3 a{ s.a[0], s.a[1], s.a[2] }, b{ s.b[0], s.b[1], s.b[2] };
                const double step = Length(b - a);
                Stroke& stroke = strokes[s.id];
                stroke.seen = g_markTime;
                if (step < 0.01 * g_unitsPerMetre || step > 2.5 * g_unitsPerMetre) {
                    ++g_lobbyStats.step;
                    if (step >= 0.01 * g_unitsPerMetre)
                        Flush(stroke);  // (a respawn: the stroke ends where it was)
                    continue;  // (standing still, or a respawn)
                }
                // Your own wheel, while its own groove above is pressing (no second rut). Where yours
                // reads itself off the ground it's left to this path's tests: leant over mid-corner,
                // your rut stopped at the apex and came back on the way out, with nothing pressed
                // between (this path's tests then answered for the bikes around you correctly).
                bool yours = false;
                for (const Wheel& w : g_wheels)
                    yours = yours || (w.on && w.grounded && Length(b - w.contact) < 0.6 * g_unitsPerMetre);
                if (yours) {
                    ++g_lobbyStats.yours;
                    Flush(stroke);
                    continue;
                }
                // (In the air or not is the shader's test: the ground seen at the tire, right at it.)
                const double au = Dot(a, u) / m.texel, av = Dot(a, v) / m.texel;
                const double bu = Dot(b, u) / m.texel, bv = Dot(b, v) / m.texel;
                const auto Inside = [&](double tu, double tv) {
                    return tu > static_cast<double>(m.originU) + margin && tu < static_cast<double>(m.originU + m.size) - margin &&
                           tv > static_cast<double>(m.originV) + margin && tv < static_cast<double>(m.originV + m.size) - margin;
                };
                if (!Inside(au, av) || !Inside(bu, bv)) {
                    ++g_lobbyStats.outside;
                    Flush(stroke);
                    continue;
                }
                ++g_lobbyStats.pressed;  // (sent to be pressed: the screen's test may still say air)
                Vec(stamp, "u_eyeA", TransformPoint(g_frame.rendered, a));
                Vec(stamp, "u_eyeB", TransformPoint(g_frame.rendered, b));
                Groove(g_res.marks, m, au, av, bu, bv, halfWidth / m.texel, strength, stroke.arc, kMarkNoFade);
                // Into the cache, in strokes: only while both answers say on the ground.
                if (s.grounded && AtSeenGround(a) && AtSeenGround(b)) {
                    // (A stroke goes on only from where it left off: a wheel that jumped - another one
                    // taken for it - starts a stroke of its own rather than joining the two.)
                    if (stroke.have && std::hypot(au - stroke.lu, av - stroke.lv) * m.texel > 0.3 * g_unitsPerMetre)
                        Flush(stroke);
                    if (!stroke.have) {
                        stroke.su = au;
                        stroke.sv = av;
                        stroke.have = true;
                    }
                    stroke.lu = bu;
                    stroke.lv = bv;
                    if (std::hypot(bu - stroke.su, bv - stroke.sv) * m.texel >= kOthersStroke * g_unitsPerMetre) {
                        PushMark(2, MarkSeg{ stroke.su, stroke.sv, bu, bv, 0.105f,
                                             static_cast<float>(stroke.arc), static_cast<float>(kMarkNoFade) });
                        stroke.su = bu;
                        stroke.sv = bv;
                    }
                    stroke.arc += std::hypot(bu - au, bv - av);  // the run's trail grows with the press
                } else {
                    Flush(stroke);
                }
            }
        }
        // Wheels gone (out of sight, out of reach): their strokes end where they were last.
        for (auto it = strokes.begin(); it != strokes.end();) {
            if (g_markTime - it->second.seen > 0.5 || !cfg.marksEveryBike) {
                if (cfg.marksEveryBike)
                    Flush(it->second);
                it = strokes.erase(it);
            } else {
                ++it;
            }
        }
    }
    // What the marks' air test reads, whatever it makes up to decide: while rolling this should
    // read a shallow fall (well under kAirFreeFallG) and no hold at all, and in a jump about -1 g
    // with a hold that climbs past kAirScoreAir at the lip and stays up all the way through the
    // flight - the first two seconds of riding, and every tenth second while any hold is being read,
    // so the whole of a jump the user has just made is in the log beside the marks it made. The
    // bike's own reading is printed as well: where the two disagree is what the answer came from.
    // Only with g_airLogging on, the whole of this block being the watching of the test.
    g_airReadTimer += dt;
    if (g_airLogging && g_airReadLines < kAirReadLinesMax && g_airReadTimer >= kAirReadEvery &&
        (g_airReadLines < 20 || g_wheels[0].airScore > 0.0 || g_wheels[1].airScore > 0.0)) {
        g_airReadTimer = 0;
        ++g_airReadLines;
        Log("postfx: tire marks: air readout: rear %s%s: %+.2f g, fall %+.2f, gap %+.2f m%s; "
            "front %s%s: %+.2f g, fall %+.2f, gap %+.2f m%s; your bike %+.2f g%s",
            g_wheels[0].on ? "on the tracker" : "off it",
            g_wheels[0].grounded ? ", down" : ", IN THE AIR",
            g_wheels[0].fallG, g_wheels[0].airScore,
            g_wheels[0].gap / g_unitsPerMetre, g_wheels[0].gapKnown ? "" : ", ground not known here",
            g_wheels[1].on ? "on the tracker" : "off it",
            g_wheels[1].grounded ? ", down" : ", IN THE AIR",
            g_wheels[1].fallG, g_wheels[1].airScore,
            g_wheels[1].gap / g_unitsPerMetre, g_wheels[1].gapKnown ? "" : ", ground not known here",
            g_bikeFallG, g_bikeHave ? "" : ", your bike not tracked");
    }
    // Nothing else is forgotten: the cache keeps the whole session (the cap above trims the
    // oldest), so riding the far end of a large track and coming back re-presses whatever you
    // made there, however many times the window wrapped around in between.
    g_gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
    g_gl.Disable(GL_BLEND);
    g_gl.Disable(GL_SCISSOR_TEST);
    if (g_markDumpWanted) {
        g_markDumpWanted = false;
        DumpMarks(cu, cv);
    }
    g_markMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return true;
}

// World position of the front plate, the bike's forward and its up: always on your bike when
// it can be found (first person: the bike tracker; third person: the object under your rider),
// else on your rider, else on the game's camera.
// Third person: the beam where the front plate faces. The plate turns with the bars (how far: the
// front wheel's axle against the bike's own across, while the game draws the wheel), and the beam
// is kept level across: aimed down along the bike's own up, as it was, a bike leaned into a turn
// threw its pool of light out to the far side of the turn (lean left, light to the right).
double g_steer = 0, g_steerSeenAt = -1e9, g_steerTime = -1e9;
void LearnPlateFromHips(const Vec3& plate, double dt);  // (below, with every other bike's headlight)

void AimLikeThePlate(Vec3& forward, Vec3& up, double now)
{
    const double dt = Clamp(now - g_steerTime, 0.0, 0.25);
    g_steerTime = now;
    const Wheel& front = g_wheels[1];
    if (front.on && !front.fromBike && front.axleAxis >= 0 && front.axleAxis < 3) {
        const Vec3 across = Normalize(Cross(forward, up));
        Vec3 axle = front.axes[front.axleAxis];
        axle = axle - up * Dot(axle, up);
        if (Length(axle) > 1e-3) {
            axle = Normalize(axle);
            if (Dot(axle, across) < 0.0)
                axle = axle * -1.0;
            const double steer = std::atan2(Dot(Cross(across, axle), up), Dot(across, axle));
            if (std::fabs(steer) < 50.0 * kDegToRad) {
                g_steer = steer;
                g_steerSeenAt = now;
            }
        }
    }
    if (now - g_steerSeenAt > 0.3)
        g_steer *= std::exp(-dt / 0.2);  // (the wheel not drawn: back to straight, gently)
    forward = Normalize(Rotate(AxisAngle(up, g_steer), forward));
    const Vec3 worldUp = WorldUp();
    const Vec3 level = worldUp - forward * Dot(worldUp, forward);
    if (Length(level) > 1e-3)
        up = Normalize(level);
}

bool HeadlightPose(Vec3& position, Vec3& forward, Vec3& up)
{
    const Config& cfg = GetConfig();
    double invRaw[16];
    if (!AffineInverse(g_frame.raw, invRaw))
        return false;
    const Vec3 worldUp = WorldUp();
    // Third person: the bike last found, against your rider (where and which way they're going),
    // to keep the light on it when it's lost for a moment (rather than jump to the rough place
    // below, and back).
    static double held[16];
    static Vec3   heldRider, heldHeading;
    static double heldAt = -1e9;
    const double now = Seconds();
    if (g_bikeNow.found) {
        PoseFromBike(g_bikeNow.model, position, forward, up);
        if (g_thirdPerson) {
            AimLikeThePlate(forward, up, now);
            if (g_bikeNow.how == 1)
                LearnPlateFromHips(position, g_lastDt);  // (other riders' lights, placed from their hips)
        }
        NoteHeadlightMode(g_bikeNow.how, position);
        if (g_bikeNow.how == 1 && g_rider.seen && g_rider.haveHeading) {
            std::copy(g_bikeNow.model, g_bikeNow.model + 16, held);
            heldRider = g_rider.position;
            heldHeading = g_rider.heading;
            heldAt = now;
        } else {
            heldAt = -1e9;
        }
        return true;
    }
    if (g_thirdPerson && g_rider.seen && g_rider.haveHeading && now - heldAt < 1.5 &&
        Length(g_rider.position - heldRider) < 10.0 * g_unitsPerMetre) {
        // Carried with your rider: moved as far as they did, turned as far about the up as they did.
        const double turn = std::atan2(Dot(Cross(heldHeading, g_rider.heading), worldUp), Dot(heldHeading, g_rider.heading));
        const Quat q = AxisAngle(worldUp, turn);
        double model[16];
        std::copy(held, held + 16, model);
        for (int c = 0; c < 3; ++c) {
            const Vec3 axis = Rotate(q, Vec3{ held[c * 4], held[c * 4 + 1], held[c * 4 + 2] });
            model[c * 4] = axis.x;
            model[c * 4 + 1] = axis.y;
            model[c * 4 + 2] = axis.z;
        }
        const Vec3 origin = g_rider.position + Rotate(q, Vec3{ held[12], held[13], held[14] } - heldRider);
        model[12] = origin.x;
        model[13] = origin.y;
        model[14] = origin.z;
        PoseFromBike(model, position, forward, up);
        AimLikeThePlate(forward, up, now);
        NoteHeadlightMode(4, position);
        return true;
    }
    if (g_rider.seen) {
        // Ahead of the middle of the rider and bike, at about the height of the plate (m).
        Vec3 flat = g_rider.haveHeading ? g_rider.heading : EyeToWorld(g_frame.raw, Vec3{ 0, 0, -1 });
        flat = flat - worldUp * Dot(flat, worldUp);
        if (Length(flat) < 1e-3)
            return false;
        forward = Normalize(flat);
        up = worldUp;
        position = g_rider.position + (forward * 0.75 + worldUp * 0.05) * (g_thirdPerson ? g_unitsPerMetre : 1.0);
        NoteHeadlightMode(2, position);
        return true;
    }
    // Level with the ground, under the camera: the way the bike faces, near enough.
    Vec3 look = EyeToWorld(g_frame.raw, Vec3{ 0, 0, -1 });
    look = look - worldUp * Dot(look, worldUp);
    if (Length(look) < 1e-3)
        return false;
    forward = Normalize(look);
    up = worldUp;
    position = CameraPosition(g_frame.raw) + forward * cfg.headlightForward + up * cfg.headlightHeight;
    NoteHeadlightMode(3, position);
    return true;
}

// Every other bike's headlight (Config::headlightEveryBike), found two ways:
//   By its wheels (lobby_wheels.h, any model): its two tires, a wheelbase apart on parallel axles,
//   give where it is, which way it points (from the rear one to the front one), its lean and how far
//   its bars are turned (the front axle against the rear). Which tire is the front: the way the bike
//   moves, while it moves; standing still, its wheels' parts as moving bikes taught them (yours too:
//   front or rear, per mesh), else its rims (a motocross bike's front rim is a 21", its rear a 19").
//   The light sits on the front plate, up the fork from the front axle (as yours in third person).
//   By its rider, when its wheels aren't known (a model whose wheels haven't been seen turning yet:
//   everyone waiting at the gate): ahead of the rider's hips, the way their torso faces, as far as
//   the plate is from yours (learnt from your bike in third person).
// Aimed where the plate faces and kept level across. Each light is followed from frame to frame: it
// fades in and out (no popping as bikes are found and lost), is held a moment where it was going
// when its bike isn't found for a frame or two, and is smoothed a little (a wheel's centre wobbles).
constexpr int    kMaxHeadlights   = 12;    // the shader's arrays (u_headPos[12]...): yours and the nearest others'
constexpr double kTireMinMetres   = 0.2;   // a tire's or a rim's radius (a brake disc's or a sprocket's is smaller)
constexpr double kTireMaxMetres   = 0.5;
constexpr double kRimMaxMetres    = 0.3;   // a rim: 21" front 0.267 m, 19" rear 0.241 m (a tire's ~0.34)
constexpr double kWheelbaseMin    = 1.15;  // m, axle to axle
constexpr double kWheelbaseMax    = 1.8;
constexpr double kWheelbaseUsual  = 1.48;
constexpr double kOtherLightReach = 150.0; // m from the camera: further bikes' lights are left out
constexpr double kLightFade       = 0.3;   // s to fade a light in or out
constexpr double kLightHold       = 0.5;   // s a light is kept going when its bike isn't found

struct OtherBike {
    Vec3   mid, front;          // the middle of its axles and its front one, the last frame it was seen
    bool   decided = false;     // which tire is the front is known
    int    against = 0;         // frames on end moving towards what's taken for its rear
    double seen = 0;
};
std::vector<OtherBike> g_otherBikes;
std::unordered_map<unsigned long long, double> g_frontVotes;  // per wheel mesh: + seen at a moving bike's front, - its rear
double g_otherLogAt = -1e9;
int    g_otherLogged[4] = { -1, -1, -1, -1 };

// The plate from a rider's hips (m): ahead, the way they face, and up. Learnt from yours.
double g_plateAhead = 0.8, g_plateUp = -0.05;

void LearnPlateFromHips(const Vec3& plate, double dt)
{
    if (!g_rider.seen || !g_rider.haveHeading)
        return;
    const Vec3 d = (plate - g_rider.hips) / g_unitsPerMetre;
    const double ahead = Dot(d, g_rider.heading), up = Dot(d, WorldUp());
    if (ahead < 0.2 || ahead > 1.6 || std::fabs(up) > 0.8)
        return;
    const double a = 1.0 - std::exp(-dt / 2.0);
    g_plateAhead += (ahead - g_plateAhead) * a;
    g_plateUp += (up - g_plateUp) * a;
}

struct ShownLight {
    Vec3   position, forward, up, velocity;
    double power = 0;     // 0..1: fading in and out
    double seen = 0;      // the last time its bike was found
    bool   updated = false;
};
std::vector<ShownLight> g_shownLights;

int OtherHeadlights(Vec3* positions, Vec3* forwards, Vec3* ups, double* powers, int max, double now, double dt)
{
    static std::vector<lobby::WheelNow> wheels;
    lobby::CurrentWheels(g_frame.raw, wheels);
    const double u = g_unitsPerMetre;
    const Vec3 worldUp = WorldUp();
    const Vec3 camera = CameraPosition(g_frame.raw);
    // The tires, one per axle (a tire's meshes and its rim can each be a "wheel"): where the biggest
    // part is (the tire: a smaller part's centre is further off its axle, and comes and goes).
    struct Tire {
        Vec3   centre, axle;
        double biggest = 0;
        double rim = 0;                    // its rim's radius (m), 0 if none was found
        unsigned long long meshes[4] = {};  // the parts on this axle
        int    meshCount = 0;
        bool   used = false;
    };
    static std::vector<Tire> tires;
    tires.clear();
    for (const lobby::WheelNow& w : wheels) {
        const double r = w.radius / u;
        if (r < kTireMinMetres || r > kTireMaxMetres)
            continue;
        const Vec3 c{ w.centre[0], w.centre[1], w.centre[2] };
        Tire* on = nullptr;
        for (Tire& t : tires)
            if (Length(t.centre - c) < 0.25 * u) {
                on = &t;
                break;
            }
        if (!on) {
            if (tires.size() >= 256)
                continue;
            tires.push_back(Tire{});
            on = &tires.back();
        }
        if (r > on->biggest) {
            on->biggest = r;
            on->centre = c;
            on->axle = Vec3{ w.axle[0], w.axle[1], w.axle[2] };
        }
        if (r < kRimMaxMetres)
            on->rim = std::max(on->rim, r);
        if (on->meshCount < 4)
            on->meshes[on->meshCount++] = w.mesh;
    }
    const auto Votes = [](const Tire& t) {
        double sum = 0;
        for (int k = 0; k < t.meshCount; ++k) {
            const auto found = g_frontVotes.find(t.meshes[k]);
            if (found != g_frontVotes.end())
                sum += found->second;
        }
        return sum;
    };
    // Pairs: a wheelbase apart, axles parallel, one behind the other (not side by side).
    struct Pair { int a, b; double score; };
    static std::vector<Pair> pairs;
    pairs.clear();
    for (size_t i = 0; i < tires.size(); ++i)
        for (size_t j = i + 1; j < tires.size(); ++j) {
            const Vec3 d = tires[j].centre - tires[i].centre;
            const double len = Length(d) / u;
            if (len < kWheelbaseMin || len > kWheelbaseMax || std::fabs(Dot(tires[i].axle, tires[j].axle)) < 0.8 ||
                std::fabs(Dot(d / (len * u), tires[i].axle)) > 0.35)
                continue;
            pairs.push_back(Pair{ static_cast<int>(i), static_cast<int>(j), std::fabs(len - kWheelbaseUsual) });
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.score < y.score; });

    // This frame's lights, before they're matched to the ones shown.
    struct Found { Vec3 position, forward, up; };
    static std::vector<Found> found;
    found.clear();
    static std::vector<OtherBike> next;
    next.clear();
    std::vector<bool> taken(g_otherBikes.size(), false);
    int bikesSeen = 0, bikesDecided = 0;
    for (const Pair& pr : pairs) {
        Tire& ta = tires[pr.a];
        Tire& tb = tires[pr.b];
        if (ta.used || tb.used)
            continue;
        ta.used = tb.used = true;
        const Vec3 mid = (ta.centre + tb.centre) * 0.5;
        // The same bike as last frame: the nearest middle, near enough.
        int match = -1;
        double best = 1.5 * u;
        for (size_t k = 0; k < g_otherBikes.size(); ++k) {
            const double d = Length(g_otherBikes[k].mid - mid);
            if (!taken[k] && d < best) {
                best = d;
                match = static_cast<int>(k);
            }
        }
        OtherBike bike;
        const Tire* front = &ta;
        const Tire* rear = &tb;
        bool moving = false;
        if (match >= 0) {
            const OtherBike& was = g_otherBikes[match];
            taken[match] = true;
            bike.decided = was.decided;
            bike.against = was.against;
            if (Length(tb.centre - was.front) < Length(ta.centre - was.front))
                std::swap(front, rear);
            // Which way it's moving says which tire leads.
            const Vec3 velocity = (mid - was.mid) / std::max(dt, 1e-3);
            if (Length(velocity) / u > 1.5) {
                moving = true;
                if (Dot(velocity, front->centre - rear->centre) < 0.0) {
                    if (!bike.decided || ++bike.against >= 5) {
                        std::swap(front, rear);
                        bike.decided = true;
                        bike.against = 0;
                    }
                } else {
                    bike.decided = true;
                    bike.against = 0;
                }
            }
        }
        // Standing still and not yet known: its parts as moving bikes taught them, else its rims.
        if (!bike.decided) {
            const double votes = Votes(*front) - Votes(*rear);
            if (std::fabs(votes) >= 30.0) {
                if (votes < 0.0)
                    std::swap(front, rear);
                bike.decided = true;
            } else if (front->rim > 0.0 && rear->rim > 0.0 && std::fabs(front->rim - rear->rim) > 0.008) {
                if (rear->rim > front->rim)
                    std::swap(front, rear);
                bike.decided = true;
            }
        } else if (moving && bike.against == 0) {
            // Moving, the right way round: what its parts are (front or rear) for bikes standing still.
            for (int side = 0; side < 2; ++side) {
                const Tire& t = side ? *rear : *front;
                for (int k = 0; k < t.meshCount; ++k) {
                    double& v = g_frontVotes[t.meshes[k]];
                    v = Clamp(v + (side ? -1.0 : 1.0), -600.0, 600.0);
                }
            }
        }
        bike.mid = mid;
        bike.front = front->centre;
        bike.seen = now;
        next.push_back(bike);
        ++bikesSeen;
        if (!bike.decided)
            continue;
        ++bikesDecided;
        // Yours has its own light.
        if (g_rider.seen && Length(mid - g_rider.position) < 1.5 * u)
            continue;
        const Vec3 forward = Normalize(front->centre - rear->centre);
        Vec3 up = Normalize(Cross(rear->axle, forward));
        if (Dot(up, worldUp) < 0.0)
            up = up * -1.0;
        if (Dot(up, worldUp) < 0.3)
            continue;  // (down on its side)
        Found f;
        f.position = front->centre + up * (kPlateAboveAxle * u) - forward * (kPlateBehindAxle * u);
        if (Length(f.position - camera) / u > kOtherLightReach)
            continue;
        // Where the plate faces: turned with the bars, then level across.
        const Vec3 across = Normalize(Cross(forward, up));
        Vec3 axle = front->axle - up * Dot(front->axle, up);
        double steer = 0;
        if (Length(axle) > 1e-3) {
            axle = Normalize(axle);
            if (Dot(axle, across) < 0.0)
                axle = axle * -1.0;
            steer = std::atan2(Dot(Cross(across, axle), up), Dot(across, axle));
            if (std::fabs(steer) > 50.0 * kDegToRad)
                steer = 0;
        }
        f.forward = Normalize(Rotate(AxisAngle(up, steer), forward));
        const Vec3 level = worldUp - f.forward * Dot(worldUp, f.forward);
        f.up = Length(level) > 1e-3 ? Normalize(level) : up;
        found.push_back(f);
    }
    // Bikes not seen this frame are remembered a moment (a wheel out of view, a frame it wasn't drawn).
    for (size_t k = 0; k < g_otherBikes.size(); ++k)
        if (!taken[k] && now - g_otherBikes[k].seen < 1.0)
            next.push_back(g_otherBikes[k]);
    g_otherBikes.swap(next);

    // Riders whose bikes weren't found by their wheels: from the rider.
    const size_t byWheels = found.size();
    int byRiders = 0;
    double invRaw[16];
    if (AffineInverse(g_frame.raw, invRaw)) {
        for (const RiderPoint& r : g_riderPoints) {
            double vm[16], world[16];
            for (int i = 0; i < 16; ++i)
                vm[i] = r.viewModel[i];
            MatMul(invRaw, vm, world);
            const Vec3 hips = TransformPoint(world, Vec3{ r.hips[0], r.hips[1], r.hips[2] });
            if (g_rider.seen && (Length(hips - g_rider.hips) < 1.0 * u || Length(hips - g_rider.position) < 1.2 * u))
                continue;  // you
            const float* t = r.torso;
            const Vec3 z = r.transposed ? Vec3{ t[2], t[5], t[8] } : Vec3{ t[6], t[7], t[8] };
            Vec3 forward = TransformDirection(world, z) * -1.0;
            forward = forward - worldUp * Dot(forward, worldUp);
            if (Length(forward) < 1e-3)
                continue;
            forward = Normalize(forward);
            Found f;
            f.position = hips + forward * (g_plateAhead * u) + worldUp * (g_plateUp * u);
            if (Length(f.position - camera) / u > kOtherLightReach)
                continue;
            bool lit = false;  // (found by its wheels already)
            for (size_t i = 0; i < byWheels && !lit; ++i)
                lit = Length(found[i].position - f.position) < 1.6 * u;
            for (const OtherBike& b : g_otherBikes)
                if (!lit && b.decided && now - b.seen < 1.0)
                    lit = Length(b.mid - hips) < 1.5 * u;
            if (lit)
                continue;
            f.forward = forward;
            f.up = worldUp;
            found.push_back(f);
            ++byRiders;
        }
    }

    // Matched to the lights shown: each to the nearest where it was going.
    for (ShownLight& s : g_shownLights)
        s.updated = false;
    const double follow = 1.0 - std::exp(-dt * 25.0), turn = 1.0 - std::exp(-dt * 15.0);
    for (const Found& f : found) {
        ShownLight* best = nullptr;
        double bestDistance = 2.0 * u;
        for (ShownLight& s : g_shownLights) {
            const double d = Length(s.position + s.velocity * dt - f.position);
            if (!s.updated && d < bestDistance) {
                bestDistance = d;
                best = &s;
            }
        }
        if (!best) {
            ShownLight s;
            s.position = f.position;
            s.forward = f.forward;
            s.up = f.up;
            s.seen = now;
            s.updated = true;
            g_shownLights.push_back(s);
            continue;
        }
        const Vec3 predicted = best->position + best->velocity * dt;
        const Vec3 position = predicted + (f.position - predicted) * follow;
        if (dt > 1e-4)
            best->velocity = best->velocity + ((position - best->position) / dt - best->velocity) * follow;
        best->position = position;
        best->forward = Normalize(best->forward + (f.forward - best->forward) * turn);
        best->up = Normalize(best->up + (f.up - best->up) * turn);
        best->seen = now;
        best->updated = true;
    }
    for (ShownLight& s : g_shownLights) {
        if (s.updated) {
            s.power = std::min(1.0, s.power + dt / kLightFade);
        } else {
            s.position = s.position + s.velocity * dt;
            if (now - s.seen > kLightHold)
                s.power -= dt / kLightFade;
        }
    }
    g_shownLights.erase(std::remove_if(g_shownLights.begin(), g_shownLights.end(),
                                       [](const ShownLight& s) { return s.power <= 0.0 && !s.updated; }),
                        g_shownLights.end());
    if (g_shownLights.size() > 64)
        g_shownLights.resize(64);
    std::sort(g_shownLights.begin(), g_shownLights.end(), [&camera](const ShownLight& x, const ShownLight& y) {
        return Length(x.position - camera) < Length(y.position - camera);
    });
    int n = 0;
    for (const ShownLight& s : g_shownLights) {
        if (n >= max)
            break;
        if (s.power <= 0.0)
            continue;
        positions[n] = s.position;
        forwards[n] = s.forward;
        ups[n] = s.up;
        powers[n] = s.power;
        ++n;
    }
    // For the log: what was found (when it changes, at most every few seconds).
    const int counts[4] = { bikesSeen, bikesDecided, byRiders, n };
    if (now - g_otherLogAt > 3.0 && std::memcmp(counts, g_otherLogged, sizeof counts) != 0) {
        g_otherLogAt = now;
        std::memcpy(g_otherLogged, counts, sizeof counts);
        Log("postfx: headlights on other bikes: %d wheel part(s) on %d axle(s): %d bike(s) found by their wheels (%d with "
            "their front known); %d rider(s) seen, %d lit from the rider; %d light(s) shown",
            static_cast<int>(wheels.size()), static_cast<int>(tires.size()), bikesSeen, bikesDecided,
            static_cast<int>(g_riderPoints.size()), byRiders, n);
    }
    return n;
}

// ===========================================================================
// The passes
// ===========================================================================
// What's in this frame's scene: your rider, your bike, its wheels, whether you're in the air. Before
// the held-back shadow casters are drawn: your wheels the game didn't draw are cast with them.
double g_applyNow = 0;

void UnderstandScene()
{
    const double now = Seconds();
    const double dt = g_lastApplyTime > 0 ? Clamp(now - g_lastApplyTime, 1.0 / 1000.0, 0.25) : 1.0 / 60.0;
    g_lastApplyTime = now;
    g_lastDt = dt;
    g_applyNow = now;
    FindOwnRider(dt);
    UpdateBike(dt);
    UpdateWheels(now, dt);
    UpdateAir(dt);
}

// ---- WEATHER: the snowfall (Config::snowfall; see shaders::kSnowVertex) ----------------------
constexpr double kSnowBoxMetres   = 30.0;   // the box of flakes that wraps round the camera
constexpr int    kSnowFlakesMax   = 60000;  // at 100%: about two in every cubic metre around you (a heavy fall)
constexpr double kSnowFallMetres  = 1.1;    // m/s, a snowflake's pace (each its own, 0.7 to 1.3 of it)
constexpr double kSnowWindMetres  = 7.0;    // m/s at 100% wind
constexpr double kSnowFlakeMetres = 0.013;  // a flake's radius (each 0.6 to 1.4 of it)
double g_snowDrift[3] = {};                 // how far the wind has carried the snow (wrapped to the box)
WetLook g_wetLook;                          // the bike shader's wet shine this frame (GetWetLook)

bool MakeSnowProgram()
{
    if (g_res.snowTried)
        return g_res.snow.id != 0;
    g_res.snowTried = true;
    std::string error;
    for (const char* version : { "#version 150\n", "#version 130\n" }) {
        const GLuint vs = CompileShader(kVertexShader, version, shaders::kSnowVertex, nullptr, error);
        if (!vs)
            continue;
        const GLuint fs = CompileShader(kFragmentShader, version, shaders::kSnowFragment, nullptr, error);
        if (!fs) {
            g_gl.DeleteShader(vs);
            continue;
        }
        const GLuint program = g_gl.CreateProgram();
        g_gl.AttachShader(program, vs);
        g_gl.AttachShader(program, fs);
        g_gl.LinkProgram(program);
        g_gl.DeleteShader(vs);
        g_gl.DeleteShader(fs);
        GLint linked = 0;
        g_gl.GetProgramiv(program, kLinkStatus, &linked);
        if (!linked) {
            g_gl.DeleteProgram(program);
            continue;
        }
        g_res.snow.id = program;
        g_gl.GenVertexArrays(1, &g_res.snowVao);  // no attributes: everything comes from gl_VertexID
        Log("postfx: snowfall ready (%s)", version[9] == '5' ? "GLSL 150" : "GLSL 130");
        if (version[9] != '5' && !error.empty())
            LogInfo("the snowfall as GLSL 150 (130 is used)", error);
        return true;
    }
    LogInfo("the snowfall couldn't be built (snow on the ground still works)", error);
    return false;
}

// Into the HDR image the composite just made (before bloom, depth of field and motion blur, which
// then take the flakes in like the rest of the scene). colour: a flake's, lit as the scene is.
void DrawSnowfall(const double V[16], const double P[16], Vec3 eye, Vec3 up, Vec3 colour, double now, double dt, int h)
{
    const Config& cfg = GetConfig();
    const int flakes = static_cast<int>(kSnowFlakesMax * cfg.snowfall);
    if (flakes < 1 || !MakeSnowProgram())
        return;
    const double u = g_unitsPerMetre, box = kSnowBoxMetres * u;
    const auto Wrap = [box](double v) { const double m = std::fmod(v, box); return m < 0 ? m + box : m; };
    // The wind: across the fall, its direction swinging slowly; carried frame by frame, so changing it
    // never moves the snow that's already falling.
    Vec3 east = Cross(up, Vec3{ 0, 0, 1 });
    if (Length(east) < 0.1)
        east = Cross(up, Vec3{ 1, 0, 0 });
    east = Normalize(east);
    const Vec3 north = Normalize(Cross(east, up));
    const double swing = 0.35 * std::sin(now * 0.05), gust = 0.8 + 0.2 * std::sin(now * 0.6) * std::sin(now * 0.23);
    const Vec3 wind = (east * std::cos(swing) + north * std::sin(swing)) * (cfg.snowWind * kSnowWindMetres * u * gust);
    const double step = Clamp(dt, 0.0, 0.25);
    g_snowDrift[0] = Wrap(g_snowDrift[0] + wind.x * step);
    g_snowDrift[1] = Wrap(g_snowDrift[1] + wind.y * step);
    g_snowDrift[2] = Wrap(g_snowDrift[2] + wind.z * step);

    Program& p = g_res.snow;
    g_gl.UseProgram(p.id);
    g_gl.BindVertexArray(g_res.snowVao);
    float rotation[9];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            rotation[c * 3 + r] = static_cast<float>(V[c * 4 + r]);
    g_gl.UniformMatrix4fv(p["u_proj"], 1, GL_FALSE, g_m.proj);
    g_gl.UniformMatrix4fv(p["u_invProj"], 1, GL_FALSE, g_m.invProj);
    g_gl.UniformMatrix3fv(p["u_viewRot"], 1, GL_FALSE, rotation);
    Vec(p, "u_camBox", Vec3{ Wrap(eye.x), Wrap(eye.y), Wrap(eye.z) });
    g_gl.Uniform1f(p["u_box"], static_cast<float>(box));
    g_gl.Uniform1f(p["u_time"], static_cast<float>(std::fmod(now, 3600.0)));
    Vec(p, "u_fall", up * (-kSnowFallMetres * u));
    Vec(p, "u_drift", Vec3{ g_snowDrift[0], g_snowDrift[1], g_snowDrift[2] });
    Vec(p, "u_sway1", east);
    Vec(p, "u_sway2", north);
    g_gl.Uniform1f(p["u_size"], static_cast<float>(kSnowFlakeMetres * u));
    g_gl.Uniform1f(p["u_pixel"], static_cast<float>(2.0 / (std::fmax(std::fabs(P[5]), 1e-3) * h)));
    g_gl.Uniform1f(p["u_metre"], static_cast<float>(u));
    g_gl.Uniform2f(p["u_texel"], g_m.texel[0], g_m.texel[1]);
    Vec(p, "u_colour", colour);
    Bind(p, "u_depth", 0, g_res.depth);

    g_gl.BindFramebuffer(kDrawFramebuffer, g_res.hdr.fbo);
    g_gl.Viewport(0, 0, g_res.hdr.w, g_res.hdr.h);
    g_gl.Disable(GL_CULL_FACE);
    g_gl.Enable(GL_BLEND);
    g_gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);  // (premultiplied; the image's alpha kept)
    g_gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
    g_gl.DrawArrays(GL_TRIANGLES, 0, flakes * 6);
    g_gl.Disable(GL_BLEND);
    g_gl.BindVertexArray(g_res.vao);  // (the fullscreen triangle, for the passes after)
}

// ---- WEATHER: the rainfall (Config::rainfall; see shaders::kRainVertex) ----------------------
constexpr double kRainBoxMetres   = 22.0;    // the box of drops that wraps round the camera
constexpr int    kRainDropsMax    = 45000;   // at 100%: about four in every cubic metre around you (a downpour)
constexpr double kRainFallMetres  = 8.5;     // m/s, a raindrop's pace
constexpr double kRainWindMetres  = 6.0;     // m/s at 100% wind
constexpr double kRainDropMetres  = 0.0015;  // a drop's half width
constexpr double kRainShutter     = 0.032;   // s: how long a streak the eye sees each drop as (its fall in that time)
double g_rainDrift[3] = {};                  // how far the rain has fallen, the wind's part too (wrapped to the box)
// Your headlight this frame, view space (the composite's own, for the drops in its beam).
struct RainHeadlight {
    double strength = 0.0;                   // 0 = off
    Vec3   pos{ 0, 0, 0 }, dir{ 0, 0, -1 }, right{ 1, 0, 0 }, up{ 0, 1, 0 }, beam{ 0.4, 0.5, 0.5 }, colour{ 1, 1, 1 };
    double reach = 1.0;
};
RainHeadlight g_rainHead;
Vec3   g_rainCamVelocity{ 0, 0, 0 };         // the camera's own motion (game units / s), eased
Vec3   g_rainLastEye{ 0, 0, 0 };
bool   g_rainHaveEye = false;

bool MakeRainProgram()
{
    if (g_res.rainTried)
        return g_res.rain.id != 0;
    g_res.rainTried = true;
    std::string error;
    for (const char* version : { "#version 150\n", "#version 130\n" }) {
        const GLuint vs = CompileShader(kVertexShader, version, shaders::kRainVertex, nullptr, error);
        if (!vs)
            continue;
        const GLuint fs = CompileShader(kFragmentShader, version, shaders::kRainFragment, nullptr, error);
        if (!fs) {
            g_gl.DeleteShader(vs);
            continue;
        }
        const GLuint program = g_gl.CreateProgram();
        g_gl.AttachShader(program, vs);
        g_gl.AttachShader(program, fs);
        g_gl.LinkProgram(program);
        g_gl.DeleteShader(vs);
        g_gl.DeleteShader(fs);
        GLint linked = 0;
        g_gl.GetProgramiv(program, kLinkStatus, &linked);
        if (!linked) {
            g_gl.DeleteProgram(program);
            continue;
        }
        g_res.rain.id = program;
        g_gl.GenVertexArrays(1, &g_res.rainVao);  // no attributes: everything comes from gl_VertexID
        Log("postfx: rainfall ready (%s)", version[9] == '5' ? "GLSL 150" : "GLSL 130");
        if (version[9] != '5' && !error.empty())
            LogInfo("the rainfall as GLSL 150 (130 is used)", error);
        return true;
    }
    LogInfo("the rainfall couldn't be built (the wet ground and the puddles still work)", error);
    return false;
}

// Into the HDR image the composite just made, as the snowfall is. colour: a drop's, lit as the scene is.
void DrawRainfall(const double V[16], const double P[16], Vec3 eye, Vec3 up, Vec3 colour, double now, double dt, int h)
{
    const Config& cfg = GetConfig();
    const int drops = static_cast<int>(kRainDropsMax * cfg.rainfall);
    if (drops < 1 || !MakeRainProgram())
        return;
    const double u = g_unitsPerMetre, box = kRainBoxMetres * u;
    const auto Wrap = [box](double v) { const double m = std::fmod(v, box); return m < 0 ? m + box : m; };
    // The wind, as the snow's: across the fall, its direction swinging slowly, carried frame by frame.
    Vec3 east = Cross(up, Vec3{ 0, 0, 1 });
    if (Length(east) < 0.1)
        east = Cross(up, Vec3{ 1, 0, 0 });
    east = Normalize(east);
    const Vec3 north = Normalize(Cross(east, up));
    const double swing = 0.35 * std::sin(now * 0.05 + 1.3), gust = 0.8 + 0.2 * std::sin(now * 0.7) * std::sin(now * 0.29);
    const Vec3 wind = (east * std::cos(swing) + north * std::sin(swing)) * (cfg.rainWind * kRainWindMetres * u * gust);
    const Vec3 fall = up * (-kRainFallMetres * u) + wind;
    const double step = Clamp(dt, 0.0, 0.25);
    g_rainDrift[0] = Wrap(g_rainDrift[0] + fall.x * step);
    g_rainDrift[1] = Wrap(g_rainDrift[1] + fall.y * step);
    g_rainDrift[2] = Wrap(g_rainDrift[2] + fall.z * step);
    // The camera's own motion: a streak is the drop's way seen from it. (A jump - a reset, a new
    // camera - isn't motion: kept as it was.)
    if (g_rainHaveEye && step > 1e-4) {
        const Vec3 moved = (eye - g_rainLastEye) * (1.0 / step);
        if (Length(moved) < 80.0 * u) {
            const double ease = Clamp(step * 12.0, 0.0, 1.0);
            g_rainCamVelocity = g_rainCamVelocity + (moved - g_rainCamVelocity) * ease;
        }
    }
    g_rainLastEye = eye;
    g_rainHaveEye = true;
    Vec3 streak = (fall - g_rainCamVelocity) * kRainShutter;
    const double longest = 1.2 * u;
    if (Length(streak) > longest)
        streak = Normalize(streak) * longest;

    Program& p = g_res.rain;
    g_gl.UseProgram(p.id);
    g_gl.BindVertexArray(g_res.rainVao);
    float rotation[9];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            rotation[c * 3 + r] = static_cast<float>(V[c * 4 + r]);
    g_gl.UniformMatrix4fv(p["u_proj"], 1, GL_FALSE, g_m.proj);
    g_gl.UniformMatrix4fv(p["u_invProj"], 1, GL_FALSE, g_m.invProj);
    g_gl.UniformMatrix3fv(p["u_viewRot"], 1, GL_FALSE, rotation);
    Vec(p, "u_camBox", Vec3{ Wrap(eye.x), Wrap(eye.y), Wrap(eye.z) });
    g_gl.Uniform1f(p["u_box"], static_cast<float>(box));
    Vec(p, "u_drift", Vec3{ g_rainDrift[0], g_rainDrift[1], g_rainDrift[2] });
    Vec(p, "u_streak", streak);
    g_gl.Uniform1f(p["u_width"], static_cast<float>(kRainDropMetres * u));
    g_gl.Uniform1f(p["u_pixel"], static_cast<float>(2.0 / (std::fmax(std::fabs(P[5]), 1e-3) * h)));
    g_gl.Uniform1f(p["u_metre"], static_cast<float>(u));
    g_gl.Uniform2f(p["u_texel"], g_m.texel[0], g_m.texel[1]);
    Vec(p, "u_colour", colour);
    g_gl.Uniform1f(p["u_headOn"], static_cast<float>(g_rainHead.strength));
    if (g_rainHead.strength > 0.0) {
        Vec(p, "u_headPos", g_rainHead.pos);
        Vec(p, "u_headDir", g_rainHead.dir);
        Vec(p, "u_headRight", g_rainHead.right);
        Vec(p, "u_headUp", g_rainHead.up);
        Vec(p, "u_headBeam", g_rainHead.beam);
        g_gl.Uniform1f(p["u_headReach"], static_cast<float>(g_rainHead.reach));
        Vec(p, "u_headColor", g_rainHead.colour);
    }
    Bind(p, "u_depth", 0, g_res.depth);

    g_gl.BindFramebuffer(kDrawFramebuffer, g_res.hdr.fbo);
    g_gl.Viewport(0, 0, g_res.hdr.w, g_res.hdr.h);
    g_gl.Disable(GL_CULL_FACE);
    g_gl.Enable(GL_BLEND);
    g_gl.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);  // (premultiplied; the image's alpha kept)
    g_gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
    g_gl.DrawArrays(GL_TRIANGLES, 0, drops * 6);
    g_gl.Disable(GL_BLEND);
    g_gl.BindVertexArray(g_res.vao);  // (the fullscreen triangle, for the passes after)
}

void Render(int w, int h)
{
    const Config& cfg = GetConfig();
    const double now = g_applyNow;
    const double dt = g_lastDt;

    g_gl.BindVertexArray(g_res.vao);  // the fullscreen triangle (the game's arrays are restored after)
    if (!CaptureColor())
        return;

    // ---- What's known about the scene ----
    const float* proj = g_haveProjCamera ? g_projCamera : (g_haveProjOther ? g_projOther : nullptr);
    double P[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, invP[16];
    if (proj) {
        for (int i = 0; i < 16; ++i)
            P[i] = proj[i];
    } else if (!g_loggedNoProjection) {
        g_loggedNoProjection = true;
        Log("postfx: no perspective projection seen yet; depth-based effects wait for one");
    }
    const bool haveInverse = Invert(P, invP);
    const bool depthOk = proj && haveInverse && CaptureDepth(g_res.captureFbo, g_res.depth);
    g_lastDepthOk = depthOk;
    ToFloat(P, g_m.proj, 16);
    ToFloat(haveInverse ? invP : P, g_m.invProj, 16);
    g_m.texel[0] = 1.0f / w;
    g_m.texel[1] = 1.0f / h;
    const bool marksOn = UpdateMarks(dt);  // after the matrices: the grooves check the ground on screen

    const double* V = g_frame.rendered;
    if (g_frame.haveLight) {
        Vec3 sun;
        if (SunFromEye(g_frame.raw, g_frame.lightEye, sun)) {
            g_gameSun = sun;
            g_haveGameSun = true;
        }
    } else if (!g_haveGameSun && !g_loggedNoLight) {
        g_loggedNoLight = true;
        Log("postfx: no '%s' upload seen yet; sun shadows and sun rays wait for it", cfg.lightUniform);
    }
    const timeofday::Sky sky = CurrentSky();
    const bool tod = cfg.timeOfDay >= 0;
    // What lights the scene: the game's sun, or ours when the time of day turns it.
    const Vec3 lightWorld = SceneLight(sky);
    const Vec3 visibleSun = tod ? sky.sun : g_gameSun;
    const Vec3 lightView = Normalize(WorldToEye(V, lightWorld));
    const Vec3 up = WorldUp();

    float viewToWorld[9];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            viewToWorld[c * 3 + r] = static_cast<float>(V[r * 4 + c]);
    const Vec3 eye = CameraPosition(V);
    const auto Wrap = [](double v) { const double m = std::fmod(v, 64.0); return m < 0 ? m + 64.0 : m; };
    const Vec3 camWrapped{ Wrap(eye.x), Wrap(eye.y), Wrap(eye.z) };
    // WEATHER: the light the snow is seen in (the time of day's, dim at night; a grey day's without it).
    const bool snowOn = cfg.snow;
    const double snowLight = tod ? std::fmax(sky.day, 0.03) * sky.exposure : 0.8;
    const Vec3 snowTint = tod ? sky.tint : Vec3{ 1.0, 1.0, 1.0 };
    const Vec3 snowFog{ 0.60 * snowTint.x * snowLight, 0.64 * snowTint.y * snowLight, 0.70 * snowTint.z * snowLight };
    const Vec3 flakeColour{ 0.86 * snowTint.x * snowLight, 0.89 * snowTint.y * snowLight, 0.94 * snowTint.z * snowLight };
    // Rain: a darker grey, and the drops lit by it.
    const bool rainOn = cfg.rain;
    const Vec3 rainFog{ 0.36 * snowTint.x * snowLight, 0.38 * snowTint.y * snowLight, 0.41 * snowTint.z * snowLight };
    const Vec3 dropColour{ 0.62 * snowTint.x * snowLight, 0.66 * snowTint.y * snowLight, 0.72 * snowTint.z * snowLight };
    // (The bike shader's wet shine, proc_hooks PaintWet: in the game's own colours, before the
    // composite's grade - as bright as the weather's light over the exposure the grade adds.)
    {
        const double light = tod ? std::fmax(sky.day, 0.03) : 0.8;
        g_wetLook.valid = rainOn && cfg.wetness > 0.0;
        g_wetLook.wet = static_cast<float>(cfg.wetness);
        g_wetLook.shine = static_cast<float>(0.25 + 0.75 * cfg.rainReflections);
        const double skyGamma = std::pow(0.3 * light, 1.0 / 2.2), groundGamma = std::pow(0.05 * light, 1.0 / 2.2);
        const double tint[3] = { snowTint.x, snowTint.y, snowTint.z };
        for (int k = 0; k < 3; ++k) {
            g_wetLook.sky[k] = static_cast<float>(skyGamma * std::pow(tint[k], 1.0 / 2.2) * (k == 0 ? 0.95 : k == 1 ? 0.98 : 1.04));
            g_wetLook.ground[k] = static_cast<float>(groundGamma * (k == 0 ? 1.05 : k == 1 ? 0.98 : 0.9));
        }
        const Vec3 upEye = Normalize(WorldToEye(V, up));
        g_wetLook.up[0] = static_cast<float>(upEye.x);
        g_wetLook.up[1] = static_cast<float>(upEye.y);
        g_wetLook.up[2] = static_cast<float>(upEye.z);
    }

    // ---- Lighting: AO and sun shadows (half resolution) ----
    const double sunStrength = tod ? std::fmax(sky.day, 0.4 * sky.night) : 1.0;
    const double shadowAmount = (g_haveGameSun || tod) ? cfg.shadows * sunStrength : 0.0;
    // With the shadow map on, it has every object's shadow, sharp and the same from any view: the
    // screen-space shadows are left out then. Seen only from what's on screen, they had the ground
    // shade itself (a dark smudge on berms in corners) and shadows that moved with your bike and
    // went away as you drove over them.
    const bool shadowMapOn = depthOk && g_shadow.usable && g_shadow.drawn && g_res.shadowMap && shadowAmount > 0.0;
    const bool screenShadows = shadowAmount > 0.0 && !shadowMapOn;
    const bool lightOn = depthOk && (cfg.ambientOcclusion > 0.0 || screenShadows);
    if (lightOn) {
        Program& p = Use(kLighting);
        Bind(p, "u_depth", 0, g_res.depth);
        Vec(p, "u_sunView", lightView);
        g_gl.Uniform1f(p["u_aoOn"], cfg.ambientOcclusion > 0.0 ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_aoRadius"], static_cast<float>(cfg.aoRadius));
        g_gl.Uniform1f(p["u_shadowOn"], screenShadows ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_shadowLength"], static_cast<float>(cfg.shadowLength));
        Draw(g_res.light);
    }

    // ---- Composite -> linear HDR ----
    {
        Program& p = Use(kComposite);
        Bind(p, "u_color", 0, g_res.color);
        Bind(p, "u_depth", 1, g_res.depth);
        Bind(p, "u_terrainDepth", 2, g_res.terrainDepth);
        Bind(p, "u_light", 3, g_res.light.tex);
        Bind(p, "u_detail", 4, g_res.detail);
        // Always on its own unit: a shadow sampler may not share one with an ordinary sampler.
        Bind(p, "u_shadowMap", 5, g_res.shadowMap);
        g_gl.Uniform1f(p["u_shadowMapOn"], shadowMapOn ? 1.0f : 0.0f);
        if (shadowMapOn) {
            float matrices[kCascades][16], bias[kCascades], texelWorld[kCascades];
            for (int c = 0; c < kCascades; ++c) {
                double projection[16], viewToShadow[16];
                for (int i = 0; i < 16; ++i)
                    projection[i] = g_shadow.cascadeProjection[c][i];
                MatMul(projection, g_shadow.toLight, viewToShadow);
                ToFloat(viewToShadow, matrices[c], 16);
                // About 4 cm along the light, plus a little more for a wider cascade's bigger texels.
                bias[c] = static_cast<float>((0.04 + g_shadow.cascadeHalf[c] * 0.002) / (2.0 * g_shadow.depth));
                // One texel of this cascade (a quarter tile of the map), for the shader's push off the surface.
                texelWorld[c] = static_cast<float>(4.0 * g_shadow.cascadeHalf[c] / std::max(g_res.shadowSize, 1));
            }
            g_gl.UniformMatrix4fv(p["u_viewToShadow[0]"], kCascades, GL_FALSE, &matrices[0][0]);
            g_gl.Uniform1fv(p["u_shadowBias[0]"], kCascades, bias);
            g_gl.Uniform1fv(p["u_shadowTexelWorld[0]"], kCascades, texelWorld);
            g_gl.Uniform1f(p["u_shadowSoft"], static_cast<float>(cfg.shadowSoftness));
            g_gl.Uniform1f(p["u_shadowTexel"], 1.0f / g_res.shadowSize);
        }
        Vec(p, "u_lightView", lightView);
        // The headlight: only when the time of day makes it dark enough to see.
        const double headlight = tod && depthOk ? cfg.headlight * Clamp(1.1 - sky.day, 0.0, 1.0) : 0.0;
        // Yours first, then the nearest other bikes' (Config::headlightEveryBike).
        Vec3 headPositions[kMaxHeadlights], headForwards[kMaxHeadlights], headUps[kMaxHeadlights];
        double headPowers[kMaxHeadlights];
        int headCount = 0;
        if (headlight > 0.0 && HeadlightPose(headPositions[0], headForwards[0], headUps[0])) {
            headPowers[0] = 1.0;
            headCount = 1;
        }
        if (headlight > 0.0 && cfg.headlightEveryBike) {
            headCount += OtherHeadlights(headPositions + headCount, headForwards + headCount, headUps + headCount,
                                         headPowers + headCount, kMaxHeadlights - headCount, now, dt);
        } else {
            g_otherBikes.clear();
            g_shownLights.clear();
        }
        const bool headlightOn = headCount > 0;
        g_rainHead.strength = 0.0;  // (the rain's: set below with the rest)
        g_gl.Uniform1f(p["u_headlight"], headlightOn ? static_cast<float>(headlight) : 0.0f);
        g_gl.Uniform1i(p["u_headCount"], headCount);
        if (headlightOn) {
            static const char* const kPos[kMaxHeadlights] = { "u_headPos[0]", "u_headPos[1]", "u_headPos[2]", "u_headPos[3]",
                "u_headPos[4]", "u_headPos[5]", "u_headPos[6]", "u_headPos[7]", "u_headPos[8]", "u_headPos[9]",
                "u_headPos[10]", "u_headPos[11]" };
            static const char* const kDir[kMaxHeadlights] = { "u_headDir[0]", "u_headDir[1]", "u_headDir[2]", "u_headDir[3]",
                "u_headDir[4]", "u_headDir[5]", "u_headDir[6]", "u_headDir[7]", "u_headDir[8]", "u_headDir[9]",
                "u_headDir[10]", "u_headDir[11]" };
            static const char* const kRight[kMaxHeadlights] = { "u_headRight[0]", "u_headRight[1]", "u_headRight[2]",
                "u_headRight[3]", "u_headRight[4]", "u_headRight[5]", "u_headRight[6]", "u_headRight[7]", "u_headRight[8]",
                "u_headRight[9]", "u_headRight[10]", "u_headRight[11]" };
            static const char* const kUp[kMaxHeadlights] = { "u_headUp[0]", "u_headUp[1]", "u_headUp[2]", "u_headUp[3]",
                "u_headUp[4]", "u_headUp[5]", "u_headUp[6]", "u_headUp[7]", "u_headUp[8]", "u_headUp[9]",
                "u_headUp[10]", "u_headUp[11]" };
            static const char* const kPower[kMaxHeadlights] = { "u_headPower[0]", "u_headPower[1]", "u_headPower[2]",
                "u_headPower[3]", "u_headPower[4]", "u_headPower[5]", "u_headPower[6]", "u_headPower[7]", "u_headPower[8]",
                "u_headPower[9]", "u_headPower[10]", "u_headPower[11]" };
            for (int i = 0; i < headCount; ++i) {
                g_gl.Uniform1f(p[kPower[i]], static_cast<float>(headPowers[i]));
                const Vec3 forwardEye = Normalize(WorldToEye(V, headForwards[i]));
                const Vec3 upEye = Normalize(WorldToEye(V, headUps[i]));
                Vec(p, kPos[i], TransformPoint(V, headPositions[i]));
                Vec(p, kDir[i], forwardEye);
                Vec(p, kRight[i], Normalize(Cross(forwardEye, upEye)));
                Vec(p, kUp[i], upEye);
            }
            // The beam: a round cone seen from the plate, like a real headlight's, from its bottom
            // edge on the ground just in front of the front wheel (65 degrees down) up to its top
            // edge, HeadlightAimDeg above level (0 by default: out to the distance). Beam width
            // spreads it sideways only. Aimed up it also reaches further ahead (twice as far at
            // +15 degrees), aimed down not as far.
            constexpr double kBottomDeg = 65.0;
            const double topDeg = Clamp(-cfg.headlightAimDeg, -15.0, 40.0);
            g_gl.Uniform3f(p["u_headBeam"], static_cast<float>(cfg.headlightBeamDeg * kDegToRad),
                           static_cast<float>((kBottomDeg + topDeg) * 0.5 * kDegToRad),
                           static_cast<float>((kBottomDeg - topDeg) * 0.5 * kDegToRad));
            g_gl.Uniform1f(p["u_headReach"], static_cast<float>(Clamp(1.0 + cfg.headlightAimDeg / 15.0, 0.6, 2.0)));
            // (The rain's drops in your beam: the same light.)
            if (headPowers[0] > 0.0) {
                const Vec3 forwardEye = Normalize(WorldToEye(V, headForwards[0]));
                const Vec3 upEye = Normalize(WorldToEye(V, headUps[0]));
                g_rainHead.strength = headlight * headPowers[0];
                g_rainHead.pos = TransformPoint(V, headPositions[0]);
                g_rainHead.dir = forwardEye;
                g_rainHead.right = Normalize(Cross(forwardEye, upEye));
                g_rainHead.up = upEye;
                g_rainHead.beam = Vec3{ cfg.headlightBeamDeg * kDegToRad, (kBottomDeg + topDeg) * 0.5 * kDegToRad,
                                        (kBottomDeg - topDeg) * 0.5 * kDegToRad };
                g_rainHead.reach = Clamp(1.0 + cfg.headlightAimDeg / 15.0, 0.6, 2.0);
                g_rainHead.colour = cfg.headlightColor == 1 ? Vec3{ 1.0, 0.63, 0.16 }
                                  : cfg.headlightColor == 2 ? RainbowColor(Seconds()) : Vec3{ 1.0, 0.97, 0.92 };
            }
            // White a touch warm (a halogen-like LED); amber, the deep orange-yellow of an amber
            // off-road lamp (its red full, its blue nearly gone); or rainbow, every hue in turn.
            if (cfg.headlightColor == 1) {
                g_gl.Uniform3f(p["u_headColor"], 1.0f, 0.63f, 0.16f);
            } else if (cfg.headlightColor == 2) {
                const Vec3 c = RainbowColor(Seconds());
                g_gl.Uniform3f(p["u_headColor"], static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z));
            } else {
                g_gl.Uniform3f(p["u_headColor"], 1.0f, 0.97f, 0.92f);
            }
        }
        // Tire marks: where the map's window is, from the camera, in its own texels.
        Bind(p, "u_markMap", 6, marksOn ? g_res.marks.tex : 0);
        Bind(p, "u_wheelMask", 7, g_wheelMask.drawn ? g_res.wheelDepth : 0);
        g_gl.Uniform1f(p["u_wheelMaskOn"], g_wheelMask.drawn ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_marks"], marksOn ? static_cast<float>(cfg.tireMarks) : 0.0f);
        if (marksOn) {
            Vec3 mu, mv;
            PlaneAxes(mu, mv);
            const Vec3 camera = CameraPosition(V);
            const double camU = Dot(camera, mu), camV = Dot(camera, mv);
            g_gl.Uniform2f(p["u_markCam"], static_cast<float>(camU / g_mark.texel - static_cast<double>(g_mark.originU)),
                           static_cast<float>(camV / g_mark.texel - static_cast<double>(g_mark.originV)));
            g_gl.Uniform2f(p["u_markPhys"], static_cast<float>(MarkWrap(g_mark.originU, g_mark.size)),
                           static_cast<float>(MarkWrap(g_mark.originV, g_mark.size)));
            Vec(p, "u_markU", WorldToEye(V, mu));
            Vec(p, "u_markV", WorldToEye(V, mv));
            Vec(p, "u_markUW", mu);
            Vec(p, "u_markVW", mv);
            g_gl.Uniform1f(p["u_markTexel"], static_cast<float>(g_mark.texel));
            g_gl.Uniform1f(p["u_markSize"], static_cast<float>(g_mark.size));
            // (Holding still around every rut ridden there are none near its edge to fade: a few m.)
            g_gl.Uniform1f(p["u_markBlend"], static_cast<float>(g_markFit ? 3.0 : kMarkBlend));
            // How deep the map's groove values measure down, in m: the relief the floor is
            // sampled at (the shader keeps it under eight texels either way).
            g_gl.Uniform1f(p["u_markDepth"], static_cast<float>(cfg.markDepth * 0.3));
        }
        // Tire dirt: each wheel's own space (its axle along x, turning with it) from the camera's.
        g_gl.Uniform1f(p["u_tireDirt"], static_cast<float>(cfg.tireDirt));
        g_gl.Uniform1f(p["u_metre"], static_cast<float>(g_unitsPerMetre));
        for (int i = 0; i < 2; ++i) {
            const Wheel& wheel = g_wheels[i];
            const bool on = depthOk && cfg.tireDirt > 0.0 && wheel.on;
            char name[32];
            snprintf(name, sizeof name, "u_wheelOn[%d]", i);
            g_gl.Uniform1f(p[name], on ? 1.0f : 0.0f);
            if (!on)
                continue;
            const Vec3 x = Normalize(WorldToEye(V, wheel.axes[wheel.axleAxis]));
            Vec3 y = WorldToEye(V, wheel.axes[(wheel.axleAxis + 1) % 3]);
            y = Normalize(y - x * Dot(y, x));
            const Vec3 z = Normalize(Cross(x, y)), o = TransformPoint(V, wheel.axle);
            // eye -> wheel: the transpose of the axes, and the origin moved there.
            const float local[16] = { static_cast<float>(x.x), static_cast<float>(y.x), static_cast<float>(z.x), 0.0f,
                                      static_cast<float>(x.y), static_cast<float>(y.y), static_cast<float>(z.y), 0.0f,
                                      static_cast<float>(x.z), static_cast<float>(y.z), static_cast<float>(z.z), 0.0f,
                                      static_cast<float>(-Dot(x, o)), static_cast<float>(-Dot(y, o)),
                                      static_cast<float>(-Dot(z, o)), 1.0f };
            snprintf(name, sizeof name, "u_wheelLocal[%d]", i);
            g_gl.UniformMatrix4fv(p[name], 1, GL_FALSE, local);
            snprintf(name, sizeof name, "u_wheelRadius[%d]", i);
            g_gl.Uniform1f(p[name], static_cast<float>(wheel.radius));
            snprintf(name, sizeof name, "u_wheelStretch[%d]", i);
            g_gl.Uniform1f(p[name], static_cast<float>(wheel.stretch));
        }
        g_gl.Uniform2f(p["u_halfTexel"], 1.0f / g_res.light.w, 1.0f / g_res.light.h);
        g_gl.Uniform1f(p["u_depthOk"], depthOk ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_lightOn"], lightOn ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_ao"], static_cast<float>(cfg.ambientOcclusion));
        g_gl.Uniform1f(p["u_shadow"], static_cast<float>(shadowAmount));
        g_gl.Uniform1f(p["u_dirt"], static_cast<float>(cfg.dirtDetail));
        g_gl.Uniform1f(p["u_dirtScale"], static_cast<float>(cfg.dirtScale));
        g_gl.Uniform1f(p["u_ultra"], cfg.ultraGraphics ? 1.0f : 0.0f);
        // The ground's patterns' frequencies (per game unit), in the shader's kPattern* order, and
        // each one's phase at the camera, in double: its whole part and its fraction (see
        // postfx_shaders.h, u_freq). Taken from the wrapped camera, the patterns jumped every 64 units.
        {
            const double ds = cfg.dirtScale > 0.0 ? cfg.dirtScale : 1.0, m = g_unitsPerMetre;
            // (The last three: the rain's puddles, coarse and fine, and the grid of its rings in them.)
            const double freqs[] = { 0.6 / ds, 2.6 / ds, 0.16 / ds, 0.012 / ds, 0.0035 / ds, 5.2 / ds,
                                     30.0 / m, 40.0 / m, 0.3 / m, 1.6 / m, 5.0 / m, 0.25 / m, 0.9 / m, 4.0 / m };
            constexpr int kPatterns = 14;
            static_assert(sizeof freqs / sizeof freqs[0] == kPatterns, "postfx_shaders.h: u_freq[14]");
            static std::string kFreq[kPatterns], kWhole[kPatterns], kFraction[kPatterns];
            if (kFreq[0].empty()) {
                for (int k = 0; k < kPatterns; ++k) {
                    kFreq[k] = "u_freq[" + std::to_string(k) + "]";
                    kWhole[k] = "u_phaseI[" + std::to_string(k) + "]";
                    kFraction[k] = "u_phaseF[" + std::to_string(k) + "]";
                }
            }
            const double at[3] = { eye.x, eye.y, eye.z };
            for (int k = 0; k < kPatterns; ++k) {
                double whole[3], fraction[3];
                for (int a = 0; a < 3; ++a) {
                    const double phase = at[a] * freqs[k];
                    whole[a] = std::floor(phase);
                    fraction[a] = phase - whole[a];
                }
                g_gl.Uniform1f(p[kFreq[k].c_str()], static_cast<float>(freqs[k]));
                g_gl.Uniform3f(p[kWhole[k].c_str()], static_cast<float>(whole[0]), static_cast<float>(whole[1]),
                               static_cast<float>(whole[2]));
                g_gl.Uniform3f(p[kFraction[k].c_str()], static_cast<float>(fraction[0]), static_cast<float>(fraction[1]),
                               static_cast<float>(fraction[2]));
            }
        }
        g_gl.Uniform1f(p["u_haveTerrain"], g_frame.terrainSnapshot ? 1.0f : 0.0f);
        g_gl.UniformMatrix3fv(p["u_viewToWorld"], 1, GL_FALSE, viewToWorld);
        Vec(p, "u_camWrapped", camWrapped);
        Vec(p, "u_up", up);
        Vec(p, "u_lightWorld", lightWorld);
        Vec(p, "u_sunWorld", visibleSun);
        Vec(p, "u_moonWorld", sky.moon);
        g_gl.Uniform1f(p["u_tod"], tod ? 1.0f : 0.0f);
        Vec(p, "u_tint", sky.tint);
        g_gl.Uniform1f(p["u_exposure"], static_cast<float>(sky.exposure));
        g_gl.Uniform1f(p["u_desaturate"], static_cast<float>(sky.desaturate));
        g_gl.Uniform1f(p["u_day"], static_cast<float>(sky.day));
        g_gl.Uniform1f(p["u_golden"], static_cast<float>(sky.golden));
        g_gl.Uniform1f(p["u_night"], static_cast<float>(sky.night));
        g_gl.Uniform1f(p["u_haze"], static_cast<float>(sky.haze));
        g_gl.Uniform1f(p["u_nightLevel"], static_cast<float>(sky.nightLevel));
        g_gl.Uniform1f(p["u_time"], static_cast<float>(std::fmod(now, 1000.0)));
        const double fovY = 2.0 * std::atan(1.0 / std::fmax(std::fabs(P[5]), 1e-3));
        g_gl.Uniform1f(p["u_pixelAngle"], static_cast<float>(fovY / h));
        g_gl.Uniform1f(p["u_snow"], snowOn ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_overcast"], snowOn || rainOn ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_weatherHaze"], static_cast<float>(std::fmax(snowOn ? cfg.snowfall : 0.0, rainOn ? 0.85 * cfg.rainfall : 0.0)));
        Vec(p, "u_weatherFog", rainOn ? rainFog : snowFog);
        g_gl.Uniform1f(p["u_rain"], rainOn ? 1.0f : 0.0f);
        g_gl.Uniform1f(p["u_rainfall"], rainOn ? static_cast<float>(cfg.rainfall) : 0.0f);
        g_gl.Uniform1f(p["u_wet"], rainOn ? static_cast<float>(cfg.wetness) : 0.0f);
        g_gl.Uniform1f(p["u_puddles"], rainOn ? static_cast<float>(cfg.puddles) : 0.0f);
        g_gl.Uniform1f(p["u_reflect"], rainOn ? static_cast<float>(cfg.rainReflections) : 0.0f);
        Draw(g_res.hdr);
    }
    if (snowOn && cfg.snowfall > 0.0 && depthOk)
        DrawSnowfall(V, P, eye, up, flakeColour, now, dt, h);
    if (rainOn && cfg.rainfall > 0.0 && depthOk)
        DrawRainfall(V, P, eye, up, dropColour, now, dt, h);
    g_lastTerrainMask = g_frame.terrainSnapshot;
    const Target* current = &g_res.hdr;
    const auto Spare = [&current]() -> Target& { return current == &g_res.a ? g_res.b : g_res.a; };

    // ---- Bloom ----
    const bool bloomOn = cfg.bloom > 0.0;
    if (bloomOn) {
        for (int i = 0; i < kBloomLevels; ++i) {
            const Target& src = i == 0 ? g_res.hdr : g_res.bloom[i - 1];
            Program& p = Use(kBloomDown);
            Bind(p, "u_src", 0, src.tex);
            g_gl.Uniform2f(p["u_srcTexel"], 1.0f / src.w, 1.0f / src.h);
            g_gl.Uniform1f(p["u_prefilter"], i == 0 ? 1.0f : 0.0f);
            g_gl.Uniform1f(p["u_threshold"], static_cast<float>(cfg.bloomThreshold));
            Draw(g_res.bloom[i]);
        }
        g_gl.Enable(GL_BLEND);
        g_gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        g_gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
        for (int i = kBloomLevels - 1; i > 0; --i) {
            Program& p = Use(kBloomUp);
            Bind(p, "u_src", 0, g_res.bloom[i].tex);
            g_gl.Uniform2f(p["u_srcTexel"], 1.0f / g_res.bloom[i].w, 1.0f / g_res.bloom[i].h);
            Draw(g_res.bloom[i - 1]);
        }
        g_gl.Disable(GL_BLEND);
    }

    // ---- Sun rays ----
    double raysAmount = 0;
    {
        const Vec3 sunView = WorldToEye(V, visibleSun);
        const double cx = P[0] * sunView.x + P[4] * sunView.y + P[8] * sunView.z;
        const double cy = P[1] * sunView.x + P[5] * sunView.y + P[9] * sunView.z;
        const double cw = P[3] * sunView.x + P[7] * sunView.y + P[11] * sunView.z;
        if (cfg.sunRays > 0.0 && depthOk && cw > 1e-4 && (g_haveGameSun || tod)) {
            const double u = cx / cw * 0.5 + 0.5, v = cy / cw * 0.5 + 0.5;
            const double off = std::fmax(std::fmax(-u, u - 1.0), std::fmax(-v, v - 1.0));  // > 0: off screen
            const double visible = 1.0 - Clamp(off / 0.4, 0.0, 1.0);
            const double daylight = tod ? Clamp((sky.sunElevationDeg + 2.0) / 6.0, 0.0, 1.0) : 1.0;
            raysAmount = cfg.sunRays * visible * daylight;
            if (raysAmount > 0.0) {
                Program& p = Use(kRays);
                Bind(p, "u_src", 0, g_res.hdr.tex);
                Bind(p, "u_depth", 1, g_res.depth);
                g_gl.Uniform2f(p["u_sunUV"], static_cast<float>(u), static_cast<float>(v));
                g_gl.Uniform1f(p["u_aspect"], static_cast<float>(w) / h);
                Draw(g_res.rays);
            }
        }
    }

    // ---- Depth of field ----
    if (cfg.depthOfField > 0.0 && depthOk) {
        const float maxBlur = static_cast<float>(12.0 * h / 1080.0);
        {
            Target& next = g_res.focus[1 - g_res.focusIndex];
            Program& p = Use(kFocus);
            Bind(p, "u_depth", 0, g_res.depth);
            Bind(p, "u_prev", 1, g_res.focus[g_res.focusIndex].tex);
            g_gl.Uniform1f(p["u_rate"], g_res.focusValid ? static_cast<float>(1.0 - std::exp(-dt / 0.3)) : 1.0f);
            g_gl.Uniform1f(p["u_manual"], static_cast<float>(cfg.dofFocus));
            Draw(next);
            g_res.focusIndex = 1 - g_res.focusIndex;
            g_res.focusValid = true;
        }
        const GLuint focus = g_res.focus[g_res.focusIndex].tex;
        {
            Program& p = Use(kDof);
            Bind(p, "u_src", 0, current->tex);
            Bind(p, "u_depth", 1, g_res.depth);
            Bind(p, "u_focus", 2, focus);
            g_gl.Uniform1f(p["u_strength"], static_cast<float>(cfg.depthOfField));
            g_gl.Uniform1f(p["u_maxBlur"], maxBlur);
            g_gl.Uniform1f(p["u_radScale"], maxBlur * maxBlur / 150.0f);
            g_gl.Uniform2f(p["u_halfTexel"], 1.0f / g_res.dof.w, 1.0f / g_res.dof.h);
            Draw(g_res.dof);
        }
        {
            Target& out = Spare();
            Program& p = Use(kDofMerge);
            Bind(p, "u_src", 0, current->tex);
            Bind(p, "u_blur", 1, g_res.dof.tex);
            Bind(p, "u_depth", 2, g_res.depth);
            Bind(p, "u_focus", 3, focus);
            g_gl.Uniform1f(p["u_strength"], static_cast<float>(cfg.depthOfField));
            g_gl.Uniform1f(p["u_maxBlur"], maxBlur);
            Draw(out);
            current = &out;
        }
    } else {
        g_res.focusValid = false;
    }

    // ---- Motion blur ----
    double viewProj[16];
    MatMul(P, V, viewProj);
    const Vec3 forward = Normalize(EyeToWorld(V, Vec3{ 0, 0, -1 }));
    if (cfg.motionBlur > 0.0 && depthOk && g_motion.valid) {
        const double moved = Length(eye - g_motion.eye);
        const double turned = std::acos(Clamp(Dot(forward, g_motion.forward), -1.0, 1.0)) * kRadToDeg;
        double invViewProj[16], reproj[16];
        if (moved < 4.0 && turned < 30.0 && Invert(viewProj, invViewProj)) {
            MatMul(g_motion.viewProj, invViewProj, reproj);
            float reprojF[16];
            ToFloat(reproj, reprojF, 16);
            Target& out = Spare();
            Program& p = Use(kMotion);
            Bind(p, "u_src", 0, current->tex);
            Bind(p, "u_depth", 1, g_res.depth);
            g_gl.UniformMatrix4fv(p["u_reproj"], 1, GL_FALSE, reprojF);
            // As if the shutter were open for this fraction of a 60 fps frame, at any frame rate.
            g_gl.Uniform1f(p["u_scale"], static_cast<float>(cfg.motionBlur * Clamp((1.0 / 60.0) / dt, 0.25, 4.0)));
            g_gl.Uniform1f(p["u_maxPx"], static_cast<float>(48.0 * h / 1080.0));
            Draw(out);
            current = &out;
        }
    }
    for (int i = 0; i < 16; ++i)
        g_motion.viewProj[i] = viewProj[i];
    g_motion.eye = eye;
    g_motion.forward = forward;
    g_motion.valid = true;

    // ---- Grade -> display colours ----
    {
        Program& p = Use(kGrade);
        Bind(p, "u_src", 0, current->tex);
        Bind(p, "u_bloom", 1, bloomOn ? g_res.bloom[0].tex : 0);
        Bind(p, "u_rays", 2, raysAmount > 0.0 ? g_res.rays.tex : 0);
        g_gl.Uniform1f(p["u_bloomAmount"], bloomOn ? static_cast<float>(cfg.bloom * 0.12) : 0.0f);
        g_gl.Uniform1f(p["u_raysAmount"], static_cast<float>(raysAmount));
        const Vec3 raysTint = tod ? (Vec3{ 1.0, 0.95, 0.85 } + (Vec3{ 1.0, 0.62, 0.35 } - Vec3{ 1.0, 0.95, 0.85 }) * sky.golden)
                                  : Vec3{ 1.0, 0.95, 0.85 };
        Vec(p, "u_raysTint", raysTint);
        g_gl.Uniform1f(p["u_exposure"], static_cast<float>(std::pow(2.0, cfg.exposure)));
        g_gl.Uniform1f(p["u_contrast"], static_cast<float>(cfg.contrast));
        g_gl.Uniform1f(p["u_saturation"], static_cast<float>(cfg.saturation));
        g_gl.Uniform1f(p["u_vignette"], static_cast<float>(cfg.vignette));
        g_gl.Uniform1f(p["u_aspect"], static_cast<float>(w) / h);
        Draw(g_res.ldr);
    }

    // ---- FXAA + sharpening -> the window ----
    {
        static const float kFastSteps[12]    = { 1.0f, 1.5f, 2.0f, 4.0f, 12.0f };
        static const float kQualitySteps[12] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.5f, 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 8.0f };
        const bool quality = cfg.antiAliasing >= 2;
        Program& p = Use(kFinal);
        Bind(p, "u_src", 0, g_res.ldr.tex);
        g_gl.Uniform1f(p["u_fxaa"], cfg.antiAliasing > 0 ? 1.0f : 0.0f);
        g_gl.Uniform1fv(p["u_steps"], 12, quality ? kQualitySteps : kFastSteps);
        g_gl.Uniform1i(p["u_stepCount"], quality ? 12 : 5);
        g_gl.Uniform1f(p["u_edgeThreshold"], quality ? 0.125f : 0.166f);
        g_gl.Uniform1f(p["u_edgeThresholdMin"], quality ? 0.0625f : 0.0833f);
        g_gl.Uniform1f(p["u_subpix"], 0.75f);
        g_gl.Uniform1f(p["u_sharpen"], static_cast<float>(cfg.sharpen));
        Draw(nullptr, w, h);
    }
}

bool WindowSize(int& w, int& h)
{
    const HDC dc = g_gl.GetCurrentDC();
    const HWND window = dc ? WindowFromDC(dc) : nullptr;
    RECT client;
    if (!window || !GetClientRect(window, &client))
        return false;
    w = client.right;
    h = client.bottom;
    return w > 0 && h > 0;
}

// ---- What the game leaves out of your shadow in first person ----------------------------------
// In first person the game draws neither your helmet (your head), nor part of your body (the
// torso), nor your rear wheel. Shapes of our own stand in for them in the shadow map, from the first
// frame: a helmet-sized ball where your head is, a rounded torso on your torso bone (it leans and
// turns with you), and a tire with its hub on the axle where your rear wheel sits on your bike.
// Whatever of the game's own the mod could keep (the helmet and body pieces while the game drew
// them in third person, the rear wheel's draw call) is cast instead (proc_hooks).
bool  g_riderPartCast = false;     // this frame: your helmet's own model was cast
bool  g_riderPieceCast = false;    // this frame: a body piece of your rider's own was cast
int   g_riderPiecesDrawn = 0;      // this frame: your rider's body pieces the game drew (NoteRiderPiecesDrawn)
int   g_standInsCast = 0;          // this frame, for the trace (F11): the shapes cast (bit i: LogStandIn's which)
int   g_lateSceneDraws = 0;        // this frame, for the trace: 3D draws after the effects were applied
int   g_riderDrawsDropped = 0;     // this frame, for the trace: rider pieces not collected (the list full)
int   g_nearCasts[6] = {};         // this frame, for the trace: the draws around your rider (NoteNearCast)
int   g_allCasts[8] = {};          // this frame, for the trace: every draw the shadow map was offered (NoteCast)
int   g_castBands[4] = {};         // and the 3D ones by how far from the camera (NoteCastDistance)
int   g_replays[2][4] = {};        // this frame, for the trace: repeats into the shadow map (NoteReplay)
int   g_realBodyDrawn = 0;         // this frame: pieces of your rider drawn along with one the game drew (NoteRealBodyDrawn)
int   g_realBodyTraced = 0;        // (the last frame's, for the trace)
unsigned g_sceneEndProgram = 0;    // what this frame's scene end was taken from
char  g_sceneEndWhy[96] = "";
bool  g_loggedWholeRider = false, g_wholeRiderWas = false;
bool  g_wheelDrawnByGame[2] = { false, false };  // this frame: the game drew that wheel's call near you
bool  g_haveHeadRest = false;
constexpr double kAtHeadMetres = 0.35;  // the game's camera this close to where your rider's eyes are: first person
constexpr double kLeftHeadMetres = 0.48; // ...and this far off before it's taken to have left them
bool  g_cameraAtHead = false;       // last frame's: the game's camera at your rider's eyes (CameraAtYourHead)
Vec3  g_headRest;                  // the game's eye in your torso bone's own frame (first person)
int   g_headRestSamples = 0;
double g_headRestUnit = 0;         // the game unit g_headRest was laid out in
bool  g_loggedStandIn[7] = {};     // head, torso (on its bone), rear wheel, swingarm, torso (on its joints), front wheel,
                                   // the bike's body

float g_shapeLight[kCascades][16];  // this frame's eye space -> each cascade's clip space (DrawStandIns)

bool MakeShapeProgram()
{
    if (g_res.headTried)
        return g_res.headProgram != 0;
    g_res.headTried = true;
    // Made from the vertex number alone (no buffers), 6 vertices per quad. u_shape 0: a unit sphere
    // (12 x 8 quads); 1: a ring about the x axis, radius 1, its tube u_minor thick (24 x 8 quads).
    static const char* const kVertexSource =
        "uniform mat4 u_light; uniform mat4 u_model; uniform int u_shape; uniform float u_minor;\n"
        "void main() {\n"
        "    int quad = gl_VertexID / 6, corner = gl_VertexID - quad * 6;\n"
        "    int du = (corner == 1 || corner == 2 || corner == 4) ? 1 : 0;\n"
        "    int dv = (corner == 2 || corner == 4 || corner == 5) ? 1 : 0;\n"
        "    vec3 p;\n"
        "    if (u_shape == 0) {\n"
        "        int ring = quad / 12, slice = quad - ring * 12;\n"
        "        float theta = float(slice + du) * (6.2831853 / 12.0), phi = float(ring + dv) * (3.1415927 / 8.0);\n"
        "        p = vec3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));\n"
        "    } else {\n"
        "        int tube = quad / 24, around = quad - tube * 24;\n"
        "        float a = float(around + du) * (6.2831853 / 24.0), b = float(tube + dv) * (6.2831853 / 8.0);\n"
        "        float r = 1.0 + u_minor * cos(b);\n"
        "        p = vec3(u_minor * sin(b), r * cos(a), r * sin(a));\n"
        "    }\n"
        "    gl_Position = u_light * (u_model * vec4(p, 1.0));\n"
        "}\n";
    static const char* const kFragmentSource = "out vec4 o_color;\nvoid main() { o_color = vec4(1.0); }\n";
    std::string error;
    for (const char* version : { "#version 150\n", "#version 130\n" }) {
        const GLuint vs = CompileShader(kVertexShader, version, kVertexSource, nullptr, error);
        if (!vs)
            continue;
        const GLuint fs = CompileShader(kFragmentShader, version, kFragmentSource, nullptr, error);
        if (!fs) {
            g_gl.DeleteShader(vs);
            continue;
        }
        const GLuint program = g_gl.CreateProgram();
        g_gl.AttachShader(program, vs);
        g_gl.AttachShader(program, fs);
        g_gl.LinkProgram(program);
        g_gl.DeleteShader(vs);
        g_gl.DeleteShader(fs);
        GLint linked = 0;
        g_gl.GetProgramiv(program, kLinkStatus, &linked);
        if (!linked) {
            g_gl.DeleteProgram(program);
            continue;
        }
        g_res.headProgram = program;
        g_res.shapeLight = g_gl.GetUniformLocation(program, "u_light");
        g_res.shapeModel = g_gl.GetUniformLocation(program, "u_model");
        g_res.shapeKind = g_gl.GetUniformLocation(program, "u_shape");
        g_res.shapeMinor = g_gl.GetUniformLocation(program, "u_minor");
        g_gl.GenVertexArrays(1, &g_res.headVao);  // no attributes: everything comes from gl_VertexID
        return true;
    }
    LogInfo("the shadow stand-ins for what the game leaves out couldn't be built", error);
    return false;
}

// One shape: model is its world transform (scale included).
void DrawShape(int kind, const double model[16], float minor)
{
    double eye[16];
    MatMul(g_frame.rendered, model, eye);  // the eye space the frame is drawn from
    float m[16];
    ToFloat(eye, m, 16);
    g_gl.UniformMatrix4fv(g_res.shapeModel, 1, GL_FALSE, m);
    g_gl.Uniform1i(g_res.shapeKind, kind);
    g_gl.Uniform1f(g_res.shapeMinor, minor);
    const int tile = g_res.shadowSize / 2;
    for (int c = 0; c < kCascades; ++c) {  // into every cascade (they're small: no point sorting them out)
        g_gl.Viewport((c % 2) * tile, (c / 2) * tile, tile, tile);
        g_gl.UniformMatrix4fv(g_res.shapeLight, 1, GL_FALSE, g_shapeLight[c]);
        g_gl.DrawArrays(GL_TRIANGLES, 0, kind == 0 ? 12 * 8 * 6 : 24 * 8 * 6);
    }
}

// A world transform: the given axes (scaled) at a point.
void ShapeModel(Vec3 x, Vec3 y, Vec3 z, Vec3 at, double out[16])
{
    const double m[16] = { x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, at.x, at.y, at.z, 1 };
    std::copy(m, m + 16, out);
}

void LogStandIn(int which, const char* text)
{
    g_standInsCast |= 1 << which;  // (called as each is cast)
    if (!g_loggedStandIn[which]) {
        g_loggedStandIn[which] = true;
        Log("postfx: shadow map: %s", text);
    }
}

void DrawStandIns()
{
    const Config& cfg = GetConfig();
    if (!cfg.shaders || !cfg.shadowMap || cfg.shadows <= 0.0 || !g_frame.haveView || !g_rider.seen)
        return;
    const double u = g_unitsPerMetre;
    bool head = false, torso = false;
    // The game drawing your whole rider (third person, or the camera close behind or beside you) draws
    // your whole bike too: no shape stands in for anything then. In a lobby the wheel tracker can let
    // go of your wheel for another bike's: your tire, swingarm and body shapes were then cast on top
    // of your real bike's shadow, and it came out a mess.
    const bool whole = g_riderPiecesDrawn >= 5;
    if (whole != g_wholeRiderWas && !g_loggedWholeRider) {
        g_loggedWholeRider = whole;
        Log("postfx: shadow map: the game draws your whole rider here (%d pieces): no shape stands in for any of it",
            g_riderPiecesDrawn);
    }
    g_wholeRiderWas = whole;
    bool atHead = false;  // the game's camera where your rider's eyes are: first person
    g_cameraAtHead = false;
    if (g_rider.haveTorso && !g_thirdPerson) {
        double invTorso[16];
        if (AffineInverse(g_rider.torsoWorld, invTorso)) {
            // Where the game's first person eye sits on your torso bone, learnt slowly while it's
            // somewhere a head can be: centred, 50 to 85 cm above the bone, within 25 cm front to
            // back. (A camera behind the rider, in a replay or another view, once dragged your
            // head's shadow back behind you.) Until then: 65 cm straight above it (your rider's).
            const Vec3 rest = TransformPoint(invTorso, CameraPosition(g_frame.raw));
            // (Laid out again if the unit changes: it's in game units. Measured before the unit settled,
            // the eyes stood in the wrong place for the rest of the session.)
            if (!g_haveHeadRest || std::fabs(g_headRestUnit - u) > 0.1 * u) {
                g_headRest = Vec3{ 0.0, 0.65 * u, 0.0 };
                g_haveHeadRest = true;
                g_headRestUnit = u;
                g_headRestSamples = 0;
            }
            if (std::fabs(rest.x) < 0.10 * u && rest.y > 0.50 * u && rest.y < 0.85 * u && std::fabs(rest.z) < 0.25 * u &&
                replay::Current() == replay::Mode::Off && g_headRestSamples < 300) {
                g_headRest = g_headRest + (rest - g_headRest) * 0.02;
                ++g_headRestSamples;  // (then kept as it is: nothing can drag it away over a session)
            }
            // Only with the game's camera right at your rider's eyes does the game leave your helmet and
            // torso out. Anywhere else (the camera close behind or beside you) it draws them, whatever
            // its pieces count: the count alone (whole, below) took some riders' gear, drawn in fewer
            // pieces, for first person, and the shapes sat inside the real helmet and hoodie, shading
            // them in jagged, pixelated patches.
            // (Only once the game's unit is measured, from your bike's wheelbase: the eyes are placed in
            // it. Until then, as before: by the pieces count alone.)
            const double off = Length(rest - g_headRest) / u;
            // (Held once there until the camera is well clear: turning your head in first person moves
            // the eyes some 35 cm about the torso bone, and the shapes went with it; the camera close
            // behind you sits half a metre off.)
            static bool wasAtHead = true;
            atHead = off < (wasAtHead ? kLeftHeadMetres : kAtHeadMetres) || !g_unitsMeasured;
            wasAtHead = atHead;
            g_cameraAtHead = atHead;
            static bool loggedOff = false;
            if (!atHead && !whole && !loggedOff) {
                loggedOff = true;
                Log("postfx: shadow map: the game's camera is %.2f m from your rider's eyes: your helmet and torso are "
                    "drawn by the game, no shapes stand in for them", off);
            }
        }
        // (Whenever the game leaves your torso and helmet out: a part kept on your torso could be
        // anything small, a body piece of the game's own any small part. Not when it draws your whole
        // rider - the camera close behind or beside you, not far enough off to count as third person:
        // the shapes then sat inside the real helmet and body, and where they poked through, they
        // shaded them in jagged patches.)
        // And once your real helmet and body pieces are known (the game drew them once: third person,
        // a replay) they're cast instead (proc_hooks CastRiderPieces, UpdateRiddenParts): the shapes
        // step aside, so your shadow is your real rider's, not the two on top of each other.
        // (The torso's shape steps aside only for your real body drawn along with your rider this frame
        // - repeated later instead, as kept, it came out invisible in the game, and your shadow had no
        // torso at all.)
        head = g_haveHeadRest && atHead && !whole && !g_riderPartCast;
        torso = g_haveHeadRest && atHead && !whole && g_realBodyDrawn == 0;
    }
    // Your wheels the game isn't drawing (placed from your bike, see OwnWheelCast): a tire for one it
    // has never drawn this session; the swingarm whenever the rear one isn't drawn (the game leaves
    // the whole back of the bike out in first person).
    bool placed[2] = { false, false }, wheel[2] = { false, false };
    for (int i = 0; i < 2; ++i) {
        const Wheel& w = g_wheels[i];
        // Whenever the game isn't drawing it (see Wheel::standIn): the tire and hub, always ours (a
        // wheel's kept draw call can be its brake disc or sprocket, followed for the wheel: the
        // shadow then went small). Placed where it sits on your bike.
        // (Never beside the real one: the game drawing its call near you, see NoteWheelDrawnByGame.)
        placed[i] = !whole && w.standIn && !g_wheelDrawnByGame[i] && w.onBikeWeight > 0 && g_bikeNow.found && WheelPlausibleOnBike(i, w.onBike) &&
                    Length(TransformPoint(g_bikeNow.model, w.onBike) - g_rider.position) < 2.0 * u;
        wheel[i] = placed[i];
    }
    const bool swingarm = placed[0];
    if (!head && !torso && !wheel[0] && !wheel[1] && !swingarm)
        return;
    if (!g_shadow.setup || !g_shadow.usable || !MakeShapeProgram() || !BeginShadowDraw())
        return;
    for (int c = 0; c < kCascades; ++c) {  // the eye space -> each cascade's clip space
        double light[16], projection[16];
        for (int i = 0; i < 16; ++i)
            projection[i] = g_shadow.cascadeProjection[c][i];
        MatMul(projection, g_shadow.toLight, light);
        ToFloat(light, g_shapeLight[c], 16);
    }
    g_gl.Disable(GL_CULL_FACE);
    g_gl.UseProgram(g_res.headProgram);
    g_gl.BindVertexArray(g_res.headVao);

    const double* T = g_rider.torsoWorld;
    const Vec3 tx = Normalize(Vec3{ T[0], T[1], T[2] }), ty = Normalize(Vec3{ T[4], T[5], T[6] }),
               tz = Normalize(Vec3{ T[8], T[9], T[10] });  // the torso bone's own axes: up +y, facing -z
    double model[16];
    // A rounded piece on the torso bone: its middle and half-sizes (across, up, front to back) in
    // metres, from the game's eye there (x across, y up, z backwards: the torso faces -z).
    const auto Piece = [&](double x, double y, double z, double across, double up, double depth) {
        const Vec3 centre = TransformPoint(T, g_headRest + Vec3{ x * u, y * u, z * u });
        ShapeModel(tx * (across * u), ty * (up * u), tz * (depth * u), centre, model);
        DrawShape(0, model, 0.0f);
    };
    if (head) {
        // A motocross helmet: the shell, longer front to back than across, its middle a little
        // above and behind the eyes; the chin bar out in front under them; the peak on top at the
        // front; and the neck down into the shoulders, so the head never floats.
        Piece(0.0, 0.03, 0.06, 0.13, 0.14, 0.16);     // shell
        Piece(0.0, -0.09, -0.10, 0.08, 0.06, 0.07);   // chin bar
        Piece(0.0, 0.11, -0.12, 0.11, 0.025, 0.10);   // peak
        Piece(0.0, -0.20, 0.06, 0.07, 0.11, 0.07);    // neck
        LogStandIn(0, "your helmet isn't drawn by the game in first person: a helmet shape stands in for it");
    }
    if (torso && g_rider.haveJoints) {
        // From your rider's own joints, so the arms and legs the game draws join it: a bar across
        // the shoulders (where the arms start), the body from the hips up to them as wide as the
        // shoulders, a bar across the hips (where the legs start), and the neck from between the
        // shoulders up into the helmet.
        const Vec3* sh = g_rider.shoulder;
        const Vec3* hp = g_rider.hip;
        const Vec3 shoulderMid = (sh[0] + sh[1]) * 0.5, hipMid = (hp[0] + hp[1]) * 0.5;
        const Vec3 spine = shoulderMid - hipMid;
        const Vec3 up = Normalize(spine);
        Vec3 side = sh[1] - sh[0];
        side = side - up * Dot(side, up);
        const double halfWide = std::fmax(Length(side) * 0.5, 0.15 * u);
        side = Normalize(side);
        const Vec3 depth = Cross(side, up);
        const auto Bar = [&](Vec3 a, Vec3 c, double thick) {  // from a to c, thick (half, metres) around
            const Vec3 along = (c - a) * 0.5;
            Vec3 other = Cross(along, depth);
            if (Length(other) < 1e-6)
                other = side;
            ShapeModel(along, Normalize(other) * (thick * u), Normalize(Cross(along, other)) * (thick * u), (a + c) * 0.5, model);
            DrawShape(0, model, 0.0f);
        };
        Bar(sh[0] - side * (0.04 * u), sh[1] + side * (0.04 * u), 0.07);    // shoulders
        Bar(hp[0] - side * (0.03 * u), hp[1] + side * (0.03 * u), 0.08);    // hips
        ShapeModel(side * halfWide, spine * 0.5 + up * (0.04 * u), depth * (0.12 * u), (shoulderMid + hipMid) * 0.5, model);
        DrawShape(0, model, 0.0f);                                           // the body
        const Vec3 helmet = TransformPoint(T, g_headRest + Vec3{ 0.0, 0.03 * u, 0.06 * u });
        Bar(shoulderMid, helmet, 0.065);                                     // neck
        LogStandIn(4, "your torso isn't drawn by the game in first person: a torso shape on your rider's shoulders and hips stands in for it");
    } else if (torso) {
        // (No joints found:) from the hips to the shoulders (20 cm under the eyes), 40 cm across,
        // 26 cm front to back, and the shoulders across its top (an oval alone would taper to a point there).
        // (From the eye, in metres: the shoulders 20 cm under it, the hips 15 cm under the torso bone.)
        const double top = -0.20, bottom = -0.15 - g_headRest.y / u;
        const double half = std::fmax((top - bottom) * 0.5, 0.15);
        Piece(0.0, (top + bottom) * 0.5, 0.04, 0.20, half, 0.13);
        Piece(0.0, -0.30, 0.05, 0.22, 0.12, 0.12);   // shoulders, up to the neck
        LogStandIn(1, "your torso isn't drawn by the game in first person: a torso shape on your torso bone stands in for it "
                      "(until your rider's shoulders and hips are found)");
    }
    for (int i = 0; i < 2; ++i) {
        if (!wheel[i])
            continue;
        const Wheel& w = g_wheels[i];
        // On the bike's centreline, its axle across the bike.
        const BikeModel& b = g_bikeModel;
        const Vec3 across = Normalize(Cross(b.forward, b.up));
        const Vec3 centre = TransformPoint(g_bikeNow.model, w.onBike - across * Dot(w.onBike, across));
        const Vec3 axle = Normalize(TransformDirection(g_bikeNow.model, across));
        Vec3 up = TransformDirection(g_bikeNow.model, b.up);
        up = Normalize(up - axle * Dot(up, axle));
        const Vec3 ahead = Cross(axle, up);
        // The tire: a ring to the outside of the tire (w.radius), 12 cm thick; then the hub.
        const double tube = 0.06 * u, ring = w.radius - tube;
        ShapeModel(axle * ring, up * ring, ahead * ring, centre, model);
        DrawShape(1, model, static_cast<float>(tube / ring));
        const double hub = 0.07 * u;
        ShapeModel(axle * hub, up * hub, ahead * hub, centre, model);
        DrawShape(0, model, 0.0f);
        LogStandIn(i == 0 ? 2 : 5, i == 0 ? "your rear wheel isn't drawn by the game here: a tire and hub stand in for it"
                             : "your front wheel isn't drawn by the game here: a tire and hub stand in for it");
    }
    if (swingarm) {
        // The back of your bike, big enough for the shadow map to show (a pixel of it covers ~6 cm):
        // the swingarm's two arms either side of the rear wheel, from its axle forward and a little up
        // to the pivot behind the engine (about 58 cm on a motocross bike) with a brace across in
        // front of the tire; the shock from there up under the seat; the rear fender over the wheel.
        const Wheel& w = g_wheels[0];
        const BikeModel& b = g_bikeModel;
        const Vec3 across = Normalize(Cross(b.forward, b.up));
        const Vec3 axleAt = TransformPoint(g_bikeNow.model, w.onBike - across * Dot(w.onBike, across));
        const Vec3 side = Normalize(TransformDirection(g_bikeNow.model, across));
        const Vec3 forward = Normalize(TransformDirection(g_bikeNow.model, b.forward));
        Vec3 up = TransformDirection(g_bikeNow.model, b.up);
        up = Normalize(up - side * Dot(up, side));
        const Vec3 pivot = axleAt + forward * (0.58 * u) + up * (0.12 * u);
        // A piece from a to b (its length), `tall` up and `wide` across (half-sizes, metres).
        const auto Bar = [&](Vec3 a, Vec3 c, double tall, double wide) {
            const Vec3 along = (c - a) * 0.5;
            const Vec3 thick = Normalize(Cross(side, along)) * (tall * u);
            ShapeModel(along, thick, side * (wide * u), (a + c) * 0.5, model);
            DrawShape(0, model, 0.0f);
        };
        for (int k = -1; k <= 1; k += 2)
            Bar(axleAt + side * (k * 0.10 * u), pivot + side * (k * 0.10 * u), 0.05, 0.04);
        Bar(pivot - forward * (0.10 * u) - side * (0.13 * u), pivot - forward * (0.10 * u) + side * (0.13 * u), 0.05, 0.05);
        Bar(axleAt + forward * (0.38 * u) + up * (0.08 * u), axleAt + forward * (0.50 * u) + up * (0.48 * u), 0.045, 0.045);
        Bar(axleAt - forward * (0.25 * u) + up * (0.42 * u), axleAt + forward * (0.45 * u) + up * (0.50 * u), 0.04, 0.11);
        LogStandIn(3, "your rear wheel isn't drawn by the game here: a swingarm shape stands in for the back of your bike "
                      "(with the shock and the rear fender)");
        // The bike's body, which the game leaves out with the back of the bike in first person: without
        // it your shadow was a rider over two tire rings and a hole where the bike is (the front end -
        // forks, bars, fender - the game does draw, and it casts). Between your two axles as learnt on
        // your bike, its length scaled to your wheelbase: the seat, the tank and its shrouds, the side
        // panels under the seat, the engine and the radiators (metres along from the rear axle, up from it).
        const Wheel& f = g_wheels[1];
        if (f.onBikeWeight > 0 && WheelPlausibleOnBike(1, f.onBike)) {
            const Vec3 frontAt = TransformPoint(g_bikeNow.model, f.onBike - across * Dot(f.onBike, across));
            Vec3 ahead = frontAt - axleAt;
            const double wheelbase = Length(ahead);
            if (wheelbase > 0.8 * u && wheelbase < 2.2 * u) {
                ahead = ahead / wheelbase;
                const Vec3 rise = Normalize(up - ahead * Dot(up, ahead));
                const double stretch = wheelbase / (1.48 * u);  // (a motocross bike's 1.48 m)
                const auto At = [&](double along, double high) { return axleAt + ahead * (along * stretch * u) + rise * (high * u); };
                const auto BodyPiece = [&](Vec3 a, Vec3 c, double tall, double wide) {  // half sizes, metres
                    const Vec3 along = (c - a) * 0.5;
                    ShapeModel(along, Normalize(Cross(side, along)) * (tall * u), side * (wide * u), (a + c) * 0.5, model);
                    DrawShape(0, model, 0.0f);
                };
                BodyPiece(At(0.05, 0.62), At(0.85, 0.63), 0.06, 0.12);   // the seat
                BodyPiece(At(0.78, 0.60), At(1.12, 0.70), 0.13, 0.21);   // the tank and its shrouds
                BodyPiece(At(0.10, 0.40), At(0.80, 0.46), 0.14, 0.13);   // the side panels, under the seat
                BodyPiece(At(0.52, 0.12), At(1.05, 0.24), 0.19, 0.15);   // the engine
                BodyPiece(At(0.95, 0.30), At(1.15, 0.58), 0.12, 0.19);   // the radiators
                LogStandIn(6, "your bike's body isn't drawn by the game here: a body shape stands in for it "
                              "(the seat, the tank and shrouds, the side panels, the engine)");
            }
        }
    }
    EndShadowDraw();
}

// ---- Timings for the trace (F11) --------------------------------------------------------------
// How long the mod's own work takes each frame: on the CPU, and on the GPU (timer queries, read a few
// frames later so nothing waits for them). 0: the held-back shadow casters drawn at once, 1: the rest
// of the effects (stand-ins, the composite and the passes after it).
constexpr int kTimerSlots = 2, kTimerRing = 4;
constexpr GLenum kTimeElapsed = 0x88BF, kQueryResult = 0x8866, kQueryResultAvailable = 0x8867;
GLuint g_timerQ[kTimerRing][kTimerSlots] = {};
bool   g_timerPending[kTimerRing][kTimerSlots] = {};
bool   g_timersMade = false;
unsigned g_timerFrame = 0;
double g_gpuMs[kTimerSlots] = {}, g_cpuMs[kTimerSlots] = {};
std::chrono::steady_clock::time_point g_timerStart[kTimerSlots];
double g_castNowMs = 0, g_castNowMsTraced = 0, g_lobbyMs = 0;
// And how many depth samples the held-back casters wrote into the shadow map (GL_SAMPLES_PASSED,
// read the same few frames later): none at all while things should be casting = drawn but thrown away.
constexpr GLenum kSamplesPassed = 0x8914;
GLuint g_sampleQ[kTimerRing] = {};
bool   g_samplePending[kTimerRing] = {};
long long g_flushSamples = -1;

bool TimersOk()
{
    return g_gl.GenQueries && g_gl.BeginQuery && g_gl.EndQuery && g_gl.GetQueryObjectiv && g_gl.GetQueryObjectui64v;
}

void TimerBegin(int slot)
{
    g_timerStart[slot] = std::chrono::steady_clock::now();
    if (!TimersOk())
        return;
    if (!g_timersMade) {
        g_gl.GenQueries(kTimerRing * kTimerSlots, &g_timerQ[0][0]);
        g_gl.GenQueries(kTimerRing, g_sampleQ);
        g_timersMade = true;
    }
    const int r = static_cast<int>(g_timerFrame % kTimerRing);
    const GLuint q = g_timerQ[r][slot];
    if (g_timerPending[r][slot]) {  // this slot's result from kTimerRing frames ago, if it's in
        GLint available = 0;
        g_gl.GetQueryObjectiv(q, kQueryResultAvailable, &available);
        if (available) {
            unsigned long long ns = 0;
            g_gl.GetQueryObjectui64v(q, kQueryResult, &ns);
            g_gpuMs[slot] = static_cast<double>(ns) / 1e6;
        }
        g_timerPending[r][slot] = false;
    }
    g_gl.BeginQuery(kTimeElapsed, q);
    if (slot == 0) {
        const GLuint s = g_sampleQ[r];
        if (g_samplePending[r]) {
            GLint available = 0;
            g_gl.GetQueryObjectiv(s, kQueryResultAvailable, &available);
            if (available) {
                unsigned long long samples = 0;
                g_gl.GetQueryObjectui64v(s, kQueryResult, &samples);
                g_flushSamples = static_cast<long long>(samples);
            }
            g_samplePending[r] = false;
        }
        g_gl.BeginQuery(kSamplesPassed, s);
    }
}

void TimerEnd(int slot)
{
    g_cpuMs[slot] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_timerStart[slot]).count();
    if (!TimersOk() || !g_timersMade)
        return;
    g_gl.EndQuery(kTimeElapsed);
    g_timerPending[g_timerFrame % kTimerRing][slot] = true;
    if (slot == 0) {
        g_gl.EndQuery(kSamplesPassed);
        g_samplePending[g_timerFrame % kTimerRing] = true;
    }
}

bool g_effectsTimerOpen = false;
void ApplyInner();
void Apply()
{
    ApplyInner();
    if (g_effectsTimerOpen) {
        g_effectsTimerOpen = false;
        TimerEnd(1);
    }
}

void ApplyInner()
{
    if (!LoadGL())
        return;
    int w = 0, h = 0;
    g_riderPartCast = false;
    g_riderPieceCast = false;
    g_riderPiecesDrawn = 0;
    g_standInsCast = 0;
    g_wheelDrawnByGame[0] = g_wheelDrawnByGame[1] = false;
    g_wheelMask.cleared = g_wheelMask.drawn = false;
    if (!WindowSize(w, h)) {
        FlushShadowCasters();
        return;
    }
    UnderstandScene();
    TimerBegin(0);
    FlushShadowCasters();  // the held-back shadow casters, all at once, before the shadows are used
    TimerEnd(0);
    TimerBegin(1);
    g_effectsTimerOpen = true;
    SavedState saved;
    Save(saved);
    DrawStandIns();       // (its program and vertex array are put back with the rest)
    PrepareState();
    if (EnsureResources(w, h)) {
        if (!g_loggedFirstApply) {
            g_loggedFirstApply = true;
            Log("postfx: running at %dx%d", w, h);
        }
        Render(w, h);
    }
    Restore(saved);
}

// The context's resources exist (made with the game's state saved around it, if not).
bool ReadyResources()
{
    int w = 0, h = 0;
    if (!WindowSize(w, h))
        return false;
    if (g_res.context == g_gl.GetCurrentContext() && g_res.programs[0].id && w == g_res.width && h == g_res.height)
        return !g_res.failed;
    SavedState saved;
    Save(saved);
    PrepareState();
    const bool ok = EnsureResources(w, h);
    Restore(saved);
    return ok;
}

// The shadow map's depth texture and framebuffer, at the configured size.
bool EnsureShadowMap()
{
    const int size = ShadowMapSize(GetConfig());  // (a change in the menu takes effect next frame)
    if (g_res.shadowMap && g_res.shadowSize == size)
        return true;
    GLint active = 0, bound = 0;
    g_gl.GetIntegerv(kActiveTexture, &active);
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    GLint unpackBuffer = 0;
    g_gl.GetIntegerv(kPixelUnpackBufferBinding, &unpackBuffer);
    g_gl.BindBuffer(kPixelUnpackBuffer, 0);
    if (g_res.shadowMap)
        g_gl.DeleteTextures(1, &g_res.shadowMap);
    g_res.shadowMap = NewTexture(kDepthComponent24, size, size, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, kTextureCompareMode, kCompareRefToTexture);  // hardware PCF
    g_gl.TexParameteri(GL_TEXTURE_2D, kTextureCompareFunc, GL_LEQUAL);
    g_gl.BindTexture(GL_TEXTURE_2D, bound);
    g_gl.ActiveTexture(active);
    g_gl.BindBuffer(kPixelUnpackBuffer, unpackBuffer);
    if (!g_res.shadowFbo)
        g_gl.GenFramebuffers(1, &g_res.shadowFbo);
    g_gl.BindFramebuffer(kDrawFramebuffer, g_res.shadowFbo);
    g_gl.FramebufferTexture2D(kDrawFramebuffer, kDepthAttachment, GL_TEXTURE_2D, g_res.shadowMap, 0);
    g_gl.DrawBuffer(GL_NONE);
    const bool ok = g_gl.CheckFramebufferStatus(kDrawFramebuffer) == kFramebufferComplete;
    g_res.shadowSize = size;
    Log("postfx: shadow map %dx%d %s", size, size, ok ? "ready" : "could not be made");
    if (!ok) {
        g_gl.DeleteTextures(1, &g_res.shadowMap);
        g_res.shadowMap = 0;
    }
    return ok;
}

// The game's state a shadow draw changes.
struct ShadowSaved {
    GLint     drawFb = 0, viewport[4] = {}, depthFunc = GL_LESS;
    GLfloat   depthRange[2] = { 0, 1 };
    GLboolean depthMask = GL_TRUE;
    bool      depthTest = false, scissorTest = false, stencilTest = false;
};
ShadowSaved g_shadowSaved;
bool g_loggedShadowCasters = false;

}  // namespace

// ===========================================================================
// Interface
// ===========================================================================
bool WantsShadowCasters()
{
    const Config& cfg = GetConfig();
    return cfg.shaders && cfg.shadowMap && cfg.shadows > 0.0 && g_frame.haveView && !g_frame.applied && !g_res.failed &&
           g_glState >= 0 && (!g_shadow.setup || g_shadow.usable);
}

bool ShadowMatrices(const float uploaded[16], const float* anchorEye, float lightModelview[16], unsigned* cascades)
{
    *cascades = 0;
    if (!g_shadow.setup)
        SetupShadowFrame();
    if (!g_shadow.usable)
        return false;
    double modelview[16], toLight[16];
    for (int i = 0; i < 16; ++i)
        modelview[i] = uploaded[i];
    MatMul(g_shadow.toLight, modelview, toLight);

    // Only what's near enough to throw a shadow inside the map. A mesh placed in world space (its
    // model matrix is the identity) can't be judged by its origin: it is always drawn.
    Vec3 at;
    if (anchorEye) {
        at = TransformPoint(g_shadow.toLight, Vec3{ anchorEye[0], anchorEye[1], anchorEye[2] });
    } else {
        double invView[16], model[16];
        bool worldSpace = false;
        if (AffineInverse(g_frame.rendered, invView)) {
            MatMul(invView, modelview, model);
            worldSpace = std::fabs(model[12]) + std::fabs(model[13]) + std::fabs(model[14]) < 1e-2 &&
                         std::fabs(model[0] - 1) + std::fabs(model[5] - 1) + std::fabs(model[10] - 1) < 1e-3;
        }
        if (worldSpace)
            *cascades = (1u << kCascades) - 1;  // (all of them)
        at = worldSpace ? Vec3{} : Vec3{ toLight[12], toLight[13], toLight[14] };
    }
    if (std::fabs(at.z) > g_shadow.depth)
        return false;
    // Into each cascade it can reach: half as far again past its edge, and never less than 40 m past
    // it (a big object's origin can sit well off what it covers: left out of the sharp ones, the
    // stands' and banners' shadows came and went near you).
    for (int c = 0; c < kCascades; ++c) {
        const double reach = g_shadow.cascadeHalf[c] + std::fmax(g_shadow.cascadeHalf[c] * 0.5, 40.0 * UnitsPerMetre());
        if (std::fabs(at.x - g_shadow.cascadeX[c]) <= reach && std::fabs(at.y - g_shadow.cascadeY[c]) <= reach)
            *cascades |= 1u << c;
    }
    if (!*cascades)
        return false;
    ToFloat(toLight, lightModelview, 16);
    return true;
}

bool FrameRenderedView(double rendered[16], double up[3])
{
    if (!g_frame.haveView)
        return false;
    std::copy(g_frame.rendered, g_frame.rendered + 16, rendered);
    const Vec3 u = WorldUp();
    up[0] = u.x;
    up[1] = u.y;
    up[2] = u.z;
    return true;
}

bool GetWetLook(WetLook& out)
{
    const Config& cfg = GetConfig();
    if (!cfg.shaders || !cfg.rain || !g_wetLook.valid || g_res.failed)
        return false;
    out = g_wetLook;
    return true;
}

bool FrameRawView(double raw[16])
{
    if (!g_frame.haveView)
        return false;
    std::copy(g_frame.raw, g_frame.raw + 16, raw);
    return true;
}

bool WantsWheelMask()
{
    const Config& cfg = GetConfig();
    return cfg.shaders && cfg.tireDirt > 0.0 && g_frame.haveView && !g_res.failed && g_glState >= 0;
}

int WheelMaskDraws(int i, float viewModels[][16], double axleLocals[][3], int max)
{
    if (i < 0 || i > 1 || !WantsWheelMask() || !g_wheels[i].on)
        return 0;
    const Wheel& w = g_wheels[i];
    const int n = std::min(w.maskParts, max);
    for (int k = 0; k < n; ++k) {
        std::copy(w.maskViewModel[k], w.maskViewModel[k] + 16, viewModels[k]);
        axleLocals[k][0] = w.maskAxleLocal[k].x;
        axleLocals[k][1] = w.maskAxleLocal[k].y;
        axleLocals[k][2] = w.maskAxleLocal[k].z;
    }
    return n;
}

bool BeginWheelMask()
{
    if (!LoadGL())
        return false;
    WheelMaskState& s = g_wheelMask;
    g_gl.GetIntegerv(kDrawFramebufferBinding, &s.drawFb);
    if (s.drawFb != 0)
        return false;  // not drawing the scene
    g_gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    const int w = s.viewport[0] + s.viewport[2], h = s.viewport[1] + s.viewport[3];
    if (w <= 0 || h <= 0)
        return false;
    if (!g_res.wheelFbo || g_res.wheelWidth != w || g_res.wheelHeight != h) {
        GLint active = 0, bound = 0, unpack = 0;
        g_gl.GetIntegerv(kActiveTexture, &active);
        g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        g_gl.GetIntegerv(kPixelUnpackBufferBinding, &unpack);
        g_gl.BindBuffer(kPixelUnpackBuffer, 0);
        if (g_res.wheelDepth)
            g_gl.DeleteTextures(1, &g_res.wheelDepth);
        g_res.wheelDepth = NewTexture(kDepthComponent24, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST);
        g_gl.TexParameteri(GL_TEXTURE_2D, kTextureCompareMode, GL_NONE);
        g_gl.BindTexture(GL_TEXTURE_2D, bound);
        g_gl.ActiveTexture(active);
        g_gl.BindBuffer(kPixelUnpackBuffer, unpack);
        if (!g_res.wheelFbo)
            g_gl.GenFramebuffers(1, &g_res.wheelFbo);
        g_gl.BindFramebuffer(kDrawFramebuffer, g_res.wheelFbo);
        g_gl.FramebufferTexture2D(kDrawFramebuffer, kDepthAttachment, GL_TEXTURE_2D, g_res.wheelDepth, 0);
        g_gl.DrawBuffer(GL_NONE);
        const bool ok = g_gl.CheckFramebufferStatus(kDrawFramebuffer) == kFramebufferComplete;
        g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFb);
        g_res.wheelWidth = w;
        g_res.wheelHeight = h;
        if (!ok) {
            Log("postfx: tire dirt: your wheels' mask could not be made; tire dirt stays off");
            g_gl.DeleteTextures(1, &g_res.wheelDepth);
            g_res.wheelDepth = 0;
        }
    }
    if (!g_res.wheelDepth)
        return false;
    g_gl.GetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    g_gl.GetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    s.depthTest = g_gl.IsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    s.scissor = g_gl.IsEnabled(GL_SCISSOR_TEST) != GL_FALSE;
    s.stencil = g_gl.IsEnabled(GL_STENCIL_TEST) != GL_FALSE;
    s.cull = g_gl.IsEnabled(GL_CULL_FACE) != GL_FALSE;
    g_gl.BindFramebuffer(kDrawFramebuffer, g_res.wheelFbo);
    g_gl.Enable(GL_DEPTH_TEST);
    g_gl.DepthFunc(GL_LESS);
    g_gl.DepthMask(GL_TRUE);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_STENCIL_TEST);
    if (!s.cleared) {
        GLfloat clearDepth = 1.0f;
        g_gl.GetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);
        g_gl.ClearDepth(1.0);
        g_gl.Clear(GL_DEPTH_BUFFER_BIT);
        g_gl.ClearDepth(clearDepth);
        s.cleared = true;
    }
    s.drawn = true;
    return true;
}

void EndWheelMask()
{
    const WheelMaskState& s = g_wheelMask;
    g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFb);
    g_gl.DepthFunc(s.depthFunc);
    g_gl.DepthMask(s.depthMask);
    SetCapability(GL_DEPTH_TEST, s.depthTest);
    SetCapability(GL_SCISSOR_TEST, s.scissor);
    SetCapability(GL_STENCIL_TEST, s.stencil);
    SetCapability(GL_CULL_FACE, s.cull);
}

int ShadowCascades()
{
    return kCascades;
}

void UseShadowCascade(int i, float projection[16])
{
    const int tile = g_res.shadowSize / 2;
    g_gl.Viewport((i % 2) * tile, (i / 2) * tile, tile, tile);
    std::copy(g_shadow.cascadeProjection[i], g_shadow.cascadeProjection[i] + 16, projection);
}

bool BeginShadowDraw()
{
    if (!g_shadow.usable || !LoadGL())
        return false;
    GLint drawFb = 0;
    g_gl.GetIntegerv(kDrawFramebufferBinding, &drawFb);
    if (drawFb != 0 || !ReadyResources())
        return false;  // the game is drawing into a texture of its own, not the scene

    ShadowSaved& s = g_shadowSaved;
    s.drawFb = drawFb;
    g_gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    g_gl.GetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    g_gl.GetFloatv(GL_DEPTH_RANGE, s.depthRange);
    g_gl.GetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    s.depthTest   = g_gl.IsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    s.scissorTest = g_gl.IsEnabled(GL_SCISSOR_TEST) != GL_FALSE;
    s.stencilTest = g_gl.IsEnabled(GL_STENCIL_TEST) != GL_FALSE;

    if (!EnsureShadowMap()) {
        g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFb);
        g_shadow.usable = false;
        return false;
    }
    g_gl.BindFramebuffer(kDrawFramebuffer, g_res.shadowFbo);
    g_gl.Viewport(0, 0, g_res.shadowSize, g_res.shadowSize);
    g_gl.Enable(GL_DEPTH_TEST);
    g_gl.DepthFunc(GL_LESS);
    g_gl.DepthMask(GL_TRUE);
    g_gl.DepthRange(0.0, 1.0);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_STENCIL_TEST);
    if (!g_shadow.cleared) {
        GLfloat clearDepth = 1.0f;
        g_gl.GetFloatv(GL_DEPTH_CLEAR_VALUE, &clearDepth);
        g_gl.ClearDepth(1.0);
        g_gl.Clear(GL_DEPTH_BUFFER_BIT);
        g_gl.ClearDepth(clearDepth);
        g_shadow.cleared = true;
        if (!g_loggedShadowCasters) {
            g_loggedShadowCasters = true;
            Log("postfx: objects cast shadow-map shadows (%.0f m around the camera, %dx%d)", g_shadow.half / g_unitsPerMetre,
                g_res.shadowSize, g_res.shadowSize);
        }
    }
    g_shadow.drawn = true;
    return true;
}

void EndShadowDraw()
{
    const ShadowSaved& s = g_shadowSaved;
    g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFb);
    g_gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    g_gl.DepthFunc(s.depthFunc);
    g_gl.DepthMask(s.depthMask);
    g_gl.DepthRange(s.depthRange[0], s.depthRange[1]);
    SetCapability(GL_DEPTH_TEST, s.depthTest);
    SetCapability(GL_SCISSOR_TEST, s.scissorTest);
    SetCapability(GL_STENCIL_TEST, s.stencilTest);
}
void NoteView(const double raw[16], const double rendered[16])
{
    if (g_frame.haveView)
        return;
    g_frame.haveView = true;
    for (int i = 0; i < 16; ++i) {
        g_frame.raw[i] = raw[i];
        g_frame.rendered[i] = rendered[i];
    }
}

void NoteProjection(const float m[16], bool fromCamera)
{
    float* slot = fromCamera ? g_projCamera : g_projOther;
    for (int i = 0; i < 16; ++i)
        slot[i] = m[i];
    (fromCamera ? g_haveProjCamera : g_haveProjOther) = true;
}

void NoteGameLight(const float* value, int components)
{
    if (!value || (components == 4 && value[3] != 0.0f))
        return;  // a point light, not the sun
    g_frame.haveLight = true;
    for (int i = 0; i < 3; ++i)
        g_frame.lightEye[i] = value[i];
    // The game lit this frame with this frame's view: only that gives the sun exactly (it is
    // worked out again from this upload once the frame's camera is known).
    Vec3 sun;
    if (const double* raw = g_frame.haveView || !g_haveGameSun ? BestRawView() : nullptr) {
        if (SunFromEye(raw, value, sun)) {
            g_gameSun = sun;
            g_haveGameSun = true;
        }
    }
}

bool OverrideLight(const float* value, int count, int components, float* out)
{
    const Config& cfg = GetConfig();
    const double* raw = BestRawView();
    if (!cfg.shaders || !cfg.moveSun || cfg.timeOfDay < 0 || !raw || !value || !g_haveGameSun)
        return false;
    const timeofday::Sky sky = timeofday::Compute(cfg.timeOfDay, g_gameSun, WorldUp(), cfg.nightBrightness);
    const Vec3 eye = Normalize(WorldToEye(raw, sky.light * (cfg.invertLight ? -1.0 : 1.0)));
    for (int i = 0; i < count; ++i) {
        const float* v = value + i * components;
        float* o = out + i * components;
        for (int k = 0; k < components; ++k)
            o[k] = v[k];
        if (components == 4 && v[3] != 0.0f)
            continue;  // a point light
        const double length = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
        o[0] = static_cast<float>(eye.x * length);
        o[1] = static_cast<float>(eye.y * length);
        o[2] = static_cast<float>(eye.z * length);
    }
    return true;
}

void TerrainPassDone()
{
    const Config& cfg = GetConfig();
    if (!cfg.shaders || (cfg.dirtDetail <= 0.0 && cfg.tireMarks <= 0.0 && cfg.tireDirt <= 0.0 && !cfg.snow && !cfg.rain) || !g_frame.haveView || g_frame.terrainSnapshot || g_frame.applied ||
        g_res.failed || !LoadGL())
        return;
    GLint drawFb = 0;
    g_gl.GetIntegerv(kDrawFramebufferBinding, &drawFb);
    if (drawFb != 0) {
        if (!g_loggedTerrainFbo) {
            g_loggedTerrainFbo = true;
            Log("postfx: the terrain is drawn into framebuffer %d, not the window: no ground mask for the dirt detail", drawFb);
        }
        return;
    }
    int w = 0, h = 0;
    if (!WindowSize(w, h))
        return;
    SavedState saved;
    Save(saved);
    PrepareState();
    if (EnsureResources(w, h) && CaptureDepth(g_res.terrainFbo, g_res.terrainDepth)) {
        g_frame.terrainSnapshot = true;
        if (cfg.tireMarks > 0.0)
            FillTerrainFromDepth();  // the ground ahead of you, for the marks' air test
    }
    Restore(saved);
}

// A rider's skeleton (any distance): its hips and torso, once per rider (a rider takes several draws).
void NoteRiderPoint(const float* centres, const float* positions, const float* rotations, bool transposed, int bones,
                    const float modelview[16])
{
    if (bones < 8 || bones > 12 || g_riderPoints.size() >= 128)
        return;
    double halfSpan = 0;
    for (int b = 0; b < bones; ++b)
        halfSpan = std::fmax(halfSpan, std::fabs(double(centres[b * 3])));
    int torso = -1, hips = -1, midline = 0;
    for (int b = 0; b < bones; ++b) {
        if (std::fabs(centres[b * 3]) > 0.05 * halfSpan)
            continue;
        ++midline;
        if (torso < 0 || centres[b * 3 + 1] > centres[torso * 3 + 1])
            torso = b;
        if (hips < 0 || centres[b * 3 + 1] < centres[hips * 3 + 1])
            hips = b;
    }
    if (halfSpan <= 0.05 || torso < 0 || hips < 0 || bones - midline < 6)
        return;
    // In view space (to know the same rider's other draws).
    const float* m = modelview;
    const float* h = positions + hips * 3;
    const float hx = m[0] * h[0] + m[4] * h[1] + m[8] * h[2] + m[12], hy = m[1] * h[0] + m[5] * h[1] + m[9] * h[2] + m[13],
                hz = m[2] * h[0] + m[6] * h[1] + m[10] * h[2] + m[14];
    for (const RiderPoint& r : g_riderPoints) {
        const float* n = r.viewModel;
        const float rx = n[0] * r.hips[0] + n[4] * r.hips[1] + n[8] * r.hips[2] + n[12],
                    ry = n[1] * r.hips[0] + n[5] * r.hips[1] + n[9] * r.hips[2] + n[13],
                    rz = n[2] * r.hips[0] + n[6] * r.hips[1] + n[10] * r.hips[2] + n[14];
        if ((rx - hx) * (rx - hx) + (ry - hy) * (ry - hy) + (rz - hz) * (rz - hz) < 1.0f)
            return;
    }
    RiderPoint r;
    std::copy(modelview, modelview + 16, r.viewModel);
    std::copy(h, h + 3, r.hips);
    std::copy(rotations + torso * 9, rotations + torso * 9 + 9, r.torso);
    r.transposed = transposed;
    g_riderPoints.push_back(r);
}

void NoteSkeleton(unsigned program, const float* centres, const float* positions, const float* rotations, bool transposed,
                  int bones, const float modelview[16])
{
    if (!centres || !positions || !rotations || !modelview || bones <= 0 || bones > kSkeletonBones ||
        g_skeletons.size() >= 400)
        return;
    // Only what's near the camera matters (your rider, your bike): skip the rest of the track.
    {
        const float* m = modelview;
        const float x = positions[0], y = positions[1], z = positions[2];
        const float ex = m[0] * x + m[4] * y + m[8] * z + m[12], ey = m[1] * x + m[5] * y + m[9] * z + m[13],
                    ez = m[2] * x + m[6] * y + m[10] * z + m[14];
        const float distance2 = ex * ex + ey * ey + ez * ez;
        if (distance2 < kRiderPointRange * kRiderPointRange)
            NoteRiderPoint(centres, positions, rotations, transposed, bones, modelview);
        if (distance2 > 60.0f * 60.0f)
            return;
    }
    // The same draw again (a rider takes several): skip it.
    for (const SkeletonRecord& rec : g_skeletons)
        if (rec.program == program && rec.bones == bones && std::memcmp(rec.viewModel, modelview, sizeof rec.viewModel) == 0 &&
            std::memcmp(rec.position, positions, sizeof(float) * 3 * bones) == 0)
            return;
    SkeletonRecord rec;
    rec.program = program;
    rec.bones = bones;
    rec.transposed = transposed;
    std::copy(modelview, modelview + 16, rec.viewModel);
    std::copy(positions, positions + bones * 3, rec.position);
    std::copy(rotations, rotations + bones * 9, rec.rotation);
    std::copy(centres, centres + bones * 3, rec.centre);
    // A rider: bones off the body's midline (arms, legs) and the highest one on it, the torso.
    double halfSpan = 0;
    for (int b = 0; b < bones; ++b)
        halfSpan = std::fmax(halfSpan, std::fabs(double(centres[b * 3])));
    int torso = -1, midline = 0;
    for (int b = 0; b < bones; ++b) {
        if (std::fabs(centres[b * 3]) > 0.05 * halfSpan)
            continue;
        ++midline;
        if (torso < 0 || centres[b * 3 + 1] > centres[torso * 3 + 1])
            torso = b;
    }
    rec.rider = halfSpan > 0.05 && torso >= 0 && bones >= 8 && bones <= 12 && bones - midline >= 6;
    rec.torso = rec.rider ? torso : -1;
    if (rec.rider) {  // the hips: the lowest bone on the midline (it sits on the seat)
        for (int b = 0; b < bones; ++b)
            if (std::fabs(centres[b * 3]) <= 0.05 * halfSpan && (rec.hips < 0 || centres[b * 3 + 1] < centres[rec.hips * 3 + 1]))
                rec.hips = b;
    }
    g_skeletons.push_back(rec);
}

void NoteObject(unsigned program, const float viewModel[16])
{
    if (!viewModel || g_objects.size() >= 1024)
        return;
    const double distance = std::sqrt(double(viewModel[12]) * viewModel[12] + double(viewModel[13]) * viewModel[13] +
                                      double(viewModel[14]) * viewModel[14]);
    if (distance > 40.0)
        return;  // your bike is never this far from the camera (the far chase camera sits ~12 m back)
    SceneObject o;
    o.program = program;
    std::copy(viewModel, viewModel + 16, o.viewModel);
    g_objects.push_back(o);
}


WheelCast OwnWheelCast(int i, unsigned* program, float viewModel[16])
{
    if (i < 0 || i > 1 || !g_frame.haveView)
        return WheelCast::None;
    const Wheel& w = g_wheels[i];
    if (w.drawnNow) {
        *program = w.partProgram;
        std::copy(w.drawnViewModel, w.drawnViewModel + 16, viewModel);
        return WheelCast::Drawn;
    }
    // Not drawn: the tire and hub of ours stand in for it (DrawStandIns). Its kept draw call isn't
    // cast for it: that can be the brake disc or sprocket, followed for the wheel (the shadow then
    // went small).
    return WheelCast::None;
}

const char* WheelTraceText()
{
    static char text[512];
    double inv[16];
    const bool bike = g_bikeNow.found && AffineInverse(g_bikeNow.model, inv);
    int n = 0;
    for (int i = 0; i < 2; ++i) {
        const Wheel& w = g_wheels[i];
        const Vec3 local = bike ? TransformPoint(inv, w.axle) : Vec3{};
        n += snprintf(text + n, sizeof text - n, "%d,%d,%d,%.3f,%.3f,%.3f,", w.on ? 1 : 0, w.on && !w.fromBike ? 1 : 0,
                      w.farOff ? 1 : 0, local.x, local.y, local.z);
    }
    n += snprintf(text + n, sizeof text - n, "%.3f,%d", g_wheels[1].stretch, bike ? 1 : 0);
    // The marks: your wheels' own answer (pressing), and what every bike's paths did this frame.
    const LobbyMarkStats& l = g_lobbyStats;
    n += snprintf(text + n, sizeof text - n, ",%d,%d,%d,%d,%d,%d,%d,%d,%.2f,%.2f", g_wheels[0].on && g_wheels[0].grounded ? 1 : 0,
                  g_wheels[1].on && g_wheels[1].grounded ? 1 : 0, l.segments, l.yours, l.clear, l.step, l.outside, l.pressed,
                  l.nearest[0], l.nearest[1]);
    // The headlight (third person): the mode it was placed in, your bike's object, the rider's
    // distance from its place on it (m), and the light from your rider: ahead, up, across (m).
    {
        const BikeTrace& b = g_bikeTrace;
        Vec3 rel;
        if (b.lit && g_rider.seen && g_rider.haveHeading) {
            const Vec3 d = (b.headlight - g_rider.position) / g_unitsPerMetre, upW = WorldUp();
            rel = Vec3{ Dot(d, g_rider.heading), Dot(d, upW), Dot(d, Normalize(Cross(g_rider.heading, upW))) };
        }
        n += snprintf(text + n, sizeof text - n, ",%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f", b.lit ? b.headlightMode : -1,
                      g_thirdPerson ? 1 : 0, b.pass, b.object, b.objects, b.carry, rel.x, rel.y, rel.z);
        g_bikeTrace.lit = false;
    }
    // The dab indicator (dab.cpp): the dab key held, sitting, your speed (m/s).
    n += snprintf(text + n, sizeof text - n, ",%d,%d,%.2f", dab::KeyDown() ? 1 : 0, dab::Sitting() ? 1 : 0,
                  g_rider.seen ? Length(g_rider.velocity) / g_unitsPerMetre : -1.0);
    // Your shadow: your rider's pieces the game drew, your real helmet / body pieces cast in place of
    // what it left out, and the shapes standing in (as letters: h helmet, t torso on its bone, T torso
    // on its joints, r rear tire, f front tire, s swingarm, b the bike's body; - none).
    char shapes[8] = "", *at = shapes;
    static const char kLetters[] = "htrsTfb";
    for (int i = 0; i < 7; ++i)
        if (g_standInsCast & (1 << i))
            *at++ = kLetters[i];
    *at = '\0';
    snprintf(text + n, sizeof text - n, ",%d,%d,%d,%s,%d,%d,%d/%d/%d/%d/%d/%d", g_riderPiecesDrawn, g_riderPieceCast ? 1 : 0,
             g_riderPartCast ? 1 : 0, shapes[0] ? shapes : "-", g_lateSceneDraws, g_riderDrawsDropped, g_nearCasts[0], g_nearCasts[1],
             g_nearCasts[2], g_nearCasts[3], g_nearCasts[4], g_nearCasts[5]);
    n = static_cast<int>(strlen(text));
    snprintf(text + n, sizeof text - n, ",%d/%d/%d/%d/%d/%d/%d/%d", g_allCasts[0], g_allCasts[1], g_allCasts[2], g_allCasts[3],
             g_allCasts[4], g_allCasts[5], g_allCasts[6], g_allCasts[7]);
    n = static_cast<int>(strlen(text));
    snprintf(text + n, sizeof text - n, ",%d/%d/%d/%d,%lld", g_castBands[0], g_castBands[1], g_castBands[2], g_castBands[3],
             g_flushSamples);
    n = static_cast<int>(strlen(text));
    snprintf(text + n, sizeof text - n, ",%d/%d/%d/%d,%d/%d/%d/%d,%d", g_replays[1][0], g_replays[1][1], g_replays[1][2],
             g_replays[1][3], g_replays[0][0], g_replays[0][1], g_replays[0][2], g_replays[0][3], g_realBodyTraced);
    n = static_cast<int>(strlen(text));
    snprintf(text + n, sizeof text - n, ",%d,%d,%d,%.2f,%d", g_markRepressed, static_cast<int>(g_repress.size()),
             static_cast<int>(g_markCacheSize[2]), g_markMs, g_markFit ? 1 : 0);
    n = static_cast<int>(strlen(text));
    snprintf(text + n, sizeof text - n, ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", g_gpuMs[0], g_gpuMs[1], g_cpuMs[0], g_cpuMs[1],
             g_castNowMsTraced, g_lobbyMs);
    for (auto& kind : g_replays)
        for (int& c : kind)
            c = 0;
    g_lateSceneDraws = g_riderDrawsDropped = 0;  // (read once a frame, at its end)
    for (int& c : g_nearCasts)
        c = 0;
    for (int& c : g_allCasts)
        c = 0;
    for (int& c : g_castBands)
        c = 0;
    return text;
}

void NoteWheelDrawnByGame(int i)
{
    if (i >= 0 && i < 2)
        g_wheelDrawnByGame[i] = true;
}

void NoteRiderPiecesDrawn(int pieces)
{
    g_riderPiecesDrawn = pieces;
}

void NoteRiderPartCast(bool piece)
{
    (piece ? g_riderPieceCast : g_riderPartCast) = true;
}

bool OwnRiderForLook(double inEye[3])
{
    // (Seen in the last frame drawn: after a pause or a menu in between it isn't trusted.)
    if (!g_thirdPerson || !g_rider.seen || Seconds() - g_riderTime > 0.25)
        return false;
    inEye[0] = g_rider.inEye.x;
    inEye[1] = g_rider.inEye.y;
    inEye[2] = g_rider.inEye.z;
    return true;
}

bool GetOwnAnchors(OwnAnchors& out)
{
    out = OwnAnchors{};
    if (!g_frame.haveView || !g_rider.seen)
        return false;
    out.bike = g_bikeNow.found;
    if (out.bike)
        std::copy(g_bikeNow.model, g_bikeNow.model + 16, out.bikeWorld);
    {
        const Vec3 across = Normalize(Cross(g_bikeModel.forward, g_bikeModel.up));
        out.bikeAcross[0] = across.x;
        out.bikeAcross[1] = across.y;
        out.bikeAcross[2] = across.z;
    }
    out.torso = g_rider.haveTorso;
    if (out.torso)
        std::copy(g_rider.torsoWorld, g_rider.torsoWorld + 16, out.torsoWorld);
    out.riderPosition[0] = g_rider.position.x;
    out.riderPosition[1] = g_rider.position.y;
    out.riderPosition[2] = g_rider.position.z;
    out.hips[0] = g_rider.hips.x;
    out.hips[1] = g_rider.hips.y;
    out.hips[2] = g_rider.hips.z;
    std::copy(g_frame.raw, g_frame.raw + 16, out.raw);
    std::copy(g_frame.rendered, g_frame.rendered + 16, out.rendered);
    out.unitsPerMetre = g_unitsPerMetre;
    out.thirdPerson = g_thirdPerson;
    return out.bike || out.torso;
}

// A new track was loaded (proc_hooks.cpp: the game sent its heightmap a new u_spacing / u_heights_dim_inv
// when one loads, the same moment terrain.cpp forgets its map of the ground). Everything the marks
// know belongs to the track that was just left: the map of the ground around you, the cache of your
// ruts - which is what re-presses them wherever ground is reused, so kept, they'd be pressed back
// where they were never made - the heights your tires recorded, and the terrain read off the screen.
// Setting the map unset is enough to have it cleared and laid out fresh at the next frame's camera.
void MarksNewTrack()
{
    if (!g_mark.set && g_markCache[0].empty() && g_markCache[1].empty() && g_markCache[2].empty())
        return;  // nothing was learnt about a ground: the marks are as fresh as the new track's
    g_mark.set = false;
    for (Wheel& w : g_wheels) {
        w.haveLast = false;
        w.fit.Reset();    // its positions are in the old track's world as well
        w.heightVel = 0;
        w.heightAccel = 0;
        w.fallG = 0;
        w.airScore = 0;
        w.airPeak = 0;
        w.grounded = true;
        w.gap = 0;
        w.gapKnown = false;
    }
    ClearMarkCache();
    ClearGroundField();
    ClearTerrainField();
    g_bikeAirFit.Reset();
    g_bikeHave = false;
    g_bikeFallG = 0;
    Log("postfx: tire marks: a new track: its ground is empty and the ruts of the one before "
        "forgotten, as are the heights your tires recorded and the terrain's own");
}

const char* TireStatusText()
{
    const Config& cfg = GetConfig();
    if (!cfg.shaders || (cfg.tireMarks <= 0.0 && cfg.tireDirt <= 0.0))
        return "";
    return g_tireStatus;
}

bool HidesGameFlare()
{
    const Config& cfg = GetConfig();
    return cfg.shaders && cfg.hideGameFlare && cfg.timeOfDay >= 0 && (g_frame.haveView || g_lastFrame3D);
}

bool SceneApplied() { return g_frame.haveView && g_frame.applied; }

void NoteDrawAfterScene(unsigned program)
{
    ++g_lateSceneDraws;
    static int logged = 0;
    if (logged < 5 && g_lateSceneDraws == 1) {
        ++logged;
        Log("postfx: a 3D object (program %u) drawn after the effects were applied this frame - no shadow for it: the "
            "scene was taken as ended at program %u (%s)", program, g_sceneEndProgram, g_sceneEndWhy);
    }
}

void NoteRiderDrawDropped() { ++g_riderDrawsDropped; }

bool OwnRiderEye(float out[3])
{
    if (!g_frame.haveView || !g_rider.seen)
        return false;
    const Vec3 e = TransformPoint(g_frame.rendered, g_rider.position);
    out[0] = static_cast<float>(e.x);
    out[1] = static_cast<float>(e.y);
    out[2] = static_cast<float>(e.z);
    return true;
}

bool NearAnyRiderEye(const float eye[3], double metres)
{
    double inv[16];
    if (!g_frame.haveView || g_lastRiderHips.empty() || !AffineInverse(g_frame.rendered, inv))
        return false;
    const Vec3 at = TransformPoint(inv, Vec3{ eye[0], eye[1], eye[2] });
    const double reach = metres * g_unitsPerMetre;
    for (const Vec3& h : g_lastRiderHips)
        if (Length(at - h) < reach)
            return true;
    return false;
}

bool CameraAtYourHead() { return g_cameraAtHead; }
void NoteCastNowMs(double ms) { g_castNowMs += ms; }
void NoteLobbyMs(double ms) { g_lobbyMs = ms; }
void RequestMarksDump() { g_markDumpWanted = true; }

bool OwnHipsEye(float out[3])
{
    if (!g_frame.haveView || !g_rider.seen)
        return false;
    const Vec3 e = TransformPoint(g_frame.rendered, g_rider.hips);
    out[0] = static_cast<float>(e.x);
    out[1] = static_cast<float>(e.y);
    out[2] = static_cast<float>(e.z);
    return true;
}

void NoteRealBodyDrawn(int pieces) { g_realBodyDrawn += pieces; }

void NoteReplay(bool skinned, int outcome)
{
    if (outcome >= 0 && outcome < 4)
        ++g_replays[skinned ? 1 : 0][outcome];
}

void NoteNearCast(int outcome)
{
    if (outcome >= 0 && outcome < 6)
        ++g_nearCasts[outcome];
}

void NoteCast(int outcome)
{
    if (outcome >= 0 && outcome < 8)
        ++g_allCasts[outcome];
}

void NoteCastDistance(double metres)
{
    ++g_castBands[metres < 50 ? 0 : metres < 100 ? 1 : metres < 200 ? 2 : 3];
}

bool WantsSceneEnd()
{
    const Config& cfg = GetConfig();
    return cfg.shaders && cfg.applyBeforeHud && g_frame.haveView && !g_frame.applied && !g_res.failed && g_glState >= 0;
}

void SceneEnd(unsigned program, const char* why)
{
    if (!WantsSceneEnd() || !LoadGL())
        return;
    GLint drawFb = 0;
    g_gl.GetIntegerv(kDrawFramebufferBinding, &drawFb);
    if (drawFb != 0)
        return;  // drawing into a texture of the game's, not over the scene
    g_frame.applied = true;
    g_sceneEndProgram = program;
    strncpy_s(g_sceneEndWhy, why ? why : "", _TRUNCATE);
    if (!g_loggedBeforeHud) {
        g_loggedBeforeHud = true;
        Log("postfx: applied before the HUD (first HUD draw: program %u, %s)", program, why);
    }
    DumpCall("^ POSTFX applied here: program %u is the start of the HUD (%s)", program, why);
    Apply();
}

void FrameEnd(bool apply)
{
    const Config& cfg = GetConfig();
    if (apply && cfg.shaders && g_frame.haveView && !g_frame.applied) {
        if (!g_loggedEndOfFrame) {
            g_loggedEndOfFrame = true;
            Log("postfx: applied at the end of the frame (%s)", cfg.applyBeforeHud ? "no HUD draw was recognised" : "ApplyBeforeHud=0");
        }
        g_frame.applied = true;
        Apply();
    }
    ++g_timerFrame;
    g_castNowMsTraced = g_castNowMs;
    g_castNowMs = 0;
    // The dab indicator's rider, when the effects didn't look for it this frame (shaders off).
    if ((cfg.dabIndicator || cfg.brakeSqueal > 0.0 || cfg.controllerLook) && g_frame.haveView && !g_riderThisFrame &&
        LoadGL()) {
        const double now = Seconds();
        const double dt = g_riderTime > 0 ? Clamp(now - g_riderTime, 1.0 / 1000.0, 0.25) : 1.0 / 60.0;
        FindOwnRider(dt);
    }
    g_riderThisFrame = false;
    if (g_frame.haveView) {
        for (int i = 0; i < 16; ++i)
            g_lastRaw[i] = g_frame.raw[i];
        g_haveLastRaw = true;
    }
    g_lastFrame3D = g_frame.haveView;
    g_realBodyTraced = g_realBodyDrawn;
    g_realBodyDrawn = 0;
    // (Every rider's hips this frame, for the next: NearAnyRiderEye.)
    g_lastRiderHips.clear();
    double invRawHips[16];
    if (g_frame.haveView && AffineInverse(g_frame.raw, invRawHips)) {
        for (const RiderPoint& r : g_riderPoints) {
            double vm[16], world[16];
            for (int i = 0; i < 16; ++i)
                vm[i] = r.viewModel[i];
            MatMul(invRawHips, vm, world);
            g_lastRiderHips.push_back(TransformPoint(world, Vec3{ r.hips[0], r.hips[1], r.hips[2] }));
        }
    }
    g_frame = Frame{};
    g_skeletons.clear();
    g_riderPoints.clear();
    g_objects.clear();
    g_shadow = ShadowFrame{};
}

bool RiderSpeed(double& speed)
{
    if (!g_rider.seen || g_riderTime < 0 || Seconds() - g_riderTime > 0.5)
        return false;
    speed = Length(g_rider.velocity) / g_unitsPerMetre;
    return true;
}

const char* StatusText()
{
    const Config& cfg = GetConfig();
    char keyName[32] = "";
    const UINT scanCode = MapVirtualKeyA(static_cast<UINT>(cfg.shaderToggleKey), MAPVK_VK_TO_VSC);
    if (!scanCode || GetKeyNameTextA(static_cast<LONG>(scanCode << 16), keyName, sizeof keyName) <= 0)
        snprintf(keyName, sizeof keyName, "KEY %02X", cfg.shaderToggleKey);
    for (char* c = keyName; *c; ++c)
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));

    if (!cfg.shaders)
        snprintf(g_status, sizeof g_status, "SHADERS OFF   \xB7   %s TOGGLES", keyName);
    else if (g_glState < 0 || g_res.failed)
        snprintf(g_status, sizeof g_status, "SHADERS UNAVAILABLE - SEE MXS_REMASTERED.LOG");
    else if (g_lastApplyTime < 0 || Seconds() - g_lastApplyTime > 1.0)
        snprintf(g_status, sizeof g_status, "WAITING FOR THE 3D VIEW");
    else
        snprintf(g_status, sizeof g_status, "SHADERS ON%s%s%s   \xB7   %.0f FPS",
                 g_lastDepthOk ? "" : " - NO DEPTH",
                 g_lastDepthOk && cfg.dirtDetail > 0 && !g_lastTerrainMask ? " - NO GROUND" : "",
                 g_haveGameSun ? "" : " - NO SUN", 1.0 / g_lastDt);
    return g_status;
}

}  // namespace postfx
}  // namespace mxcam
