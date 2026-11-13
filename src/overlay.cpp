// overlay.cpp
//   In-game settings panel ("MXS Remastered"). Config::menuKey (E by default) opens and closes it.
//   Black and red: the tabs down the left (with a search box over them that finds a setting on
//   any tab), the logo and the presets along the top, the current tab's settings in one scrolling
//   list in a box, and the buttons along the bottom.
//
//   Rendering: the panel is drawn into the game's back buffer right before each swap, as one
//   batch of textured triangles through a tiny shader of our own (GLSL 1.50, 1.30 or 1.20,
//   whichever the context accepts). The texture is an atlas built once with GDI: Segoe UI and
//   Arial glyphs, an anti-aliased disc (slider knobs, rounded corners), a solid white block
//   (plain rectangles) and the logo (logo_png.h, decoded with WIC). The settings list is clipped
//   to its box with the scissor test. Every piece of GL state touched is saved and restored, and
//   all GL calls go straight to the driver, never through our own hooks.
//
//   Input: keys and the mouse are polled each frame. While the panel is open the game window is
//   subclassed so clicks and key presses stop reaching the game (releases still pass, so nothing
//   gets stuck), and the system cursor is replaced by a drawn one.

#include <windows.h>
#ifdef _WIN32
#include <wincodec.h>
#else
#include "linux/platform_linux.h"
#include "linux/third_party/stb_image.h"
#include "linux/third_party/stb_truetype.h"
#endif
#include <GL/gl.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bike.h"
#include "camera.h"
#include "config.h"
#include "dab.h"
#include "gamepad.h"
#include "hooks.h"
#include "log.h"
#include "logo_png.h"
#include "mathlib.h"
#include "overlay.h"
#include "postfx.h"
#include "replay.h"
#include "sound_packs.h"
#include "texfilter.h"

#ifdef _WIN32
#pragma comment(lib, "windowscodecs.lib")  // (the logo's PNG: WIC)
#pragma comment(lib, "ole32.lib")
#endif

namespace mxcam {
namespace {

// ===========================================================================
// OpenGL entry points (the driver's own) and constants past OpenGL 1.1
// ===========================================================================
constexpr GLenum kArrayBuffer             = 0x8892;
constexpr GLenum kArrayBufferBinding      = 0x8894;
constexpr GLenum kStreamDraw              = 0x88E0;
constexpr GLenum kVertexShader            = 0x8B31;
constexpr GLenum kFragmentShader          = 0x8B30;
constexpr GLenum kCompileStatus           = 0x8B81;
constexpr GLenum kLinkStatus              = 0x8B82;
constexpr GLenum kCurrentProgram          = 0x8B8D;
constexpr GLenum kTexture0                = 0x84C0;
constexpr GLenum kActiveTexture           = 0x84E0;
constexpr GLenum kVertexArrayBinding      = 0x85B5;
constexpr GLenum kDrawFramebuffer         = 0x8CA9;
constexpr GLenum kDrawFramebufferBinding  = 0x8CA6;
constexpr GLenum kSamplerBinding          = 0x8919;
constexpr GLenum kBlendDstRgb             = 0x80C8;
constexpr GLenum kBlendSrcRgb             = 0x80C9;
constexpr GLenum kBlendDstAlpha           = 0x80CA;
constexpr GLenum kBlendSrcAlpha           = 0x80CB;
constexpr GLenum kBlendEquationRgb        = 0x8009;
constexpr GLenum kBlendEquationAlpha      = 0x883D;
constexpr GLenum kFuncAdd                 = 0x8006;
constexpr GLenum kClampToEdge             = 0x812F;
constexpr GLenum kPixelUnpackBuffer       = 0x88EC;
constexpr GLenum kPixelUnpackBufferBinding = 0x88EF;
constexpr GLenum kFramebufferSrgb         = 0x8DB9;

struct GLApi {
    decltype(&::glGetIntegerv)        GetIntegerv;
    decltype(&::glGetBooleanv)        GetBooleanv;
    decltype(&::glIsEnabled)          IsEnabled;
    decltype(&::glEnable)             Enable;
    decltype(&::glDisable)            Disable;
    decltype(&::glViewport)           Viewport;
    decltype(&::glScissor)            Scissor;
    decltype(&::glColorMask)          ColorMask;
    decltype(&::glGenTextures)        GenTextures;
    decltype(&::glDeleteTextures)     DeleteTextures;
    decltype(&::glBindTexture)        BindTexture;
    decltype(&::glTexParameteri)      TexParameteri;
    decltype(&::glTexImage2D)         TexImage2D;
    decltype(&::glPixelStorei)        PixelStorei;
    decltype(&::glDrawArrays)         DrawArrays;
    decltype(&::wglGetCurrentContext) GetCurrentContext;
    GLuint (APIENTRY* CreateShader)(GLenum);
    void   (APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
    void   (APIENTRY* CompileShader)(GLuint);
    void   (APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* DeleteShader)(GLuint);
    GLuint (APIENTRY* CreateProgram)();
    void   (APIENTRY* DeleteProgram)(GLuint);
    void   (APIENTRY* AttachShader)(GLuint, GLuint);
    void   (APIENTRY* BindAttribLocation)(GLuint, GLuint, const char*);
    void   (APIENTRY* LinkProgram)(GLuint);
    void   (APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void   (APIENTRY* UseProgram)(GLuint);
    GLint  (APIENTRY* GetUniformLocation)(GLuint, const char*);
    void   (APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void   (APIENTRY* Uniform1i)(GLint, GLint);
    void   (APIENTRY* GenBuffers)(GLsizei, GLuint*);
    void   (APIENTRY* BindBuffer)(GLenum, GLuint);
    void   (APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
    void   (APIENTRY* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void   (APIENTRY* EnableVertexAttribArray)(GLuint);
    void   (APIENTRY* ActiveTexture)(GLenum);
    void   (APIENTRY* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
    void   (APIENTRY* BlendEquation)(GLenum);
    void   (APIENTRY* BlendEquationSeparate)(GLenum, GLenum);
    void   (APIENTRY* GenVertexArrays)(GLsizei, GLuint*);
    void   (APIENTRY* BindVertexArray)(GLuint);
    void   (APIENTRY* BindFramebuffer)(GLenum, GLuint);  // optional (GL 3.0)
    void   (APIENTRY* BindSampler)(GLuint, GLuint);      // optional (GL 3.3)
};

GLApi g_gl{};
bool  g_glLoaded    = false;
bool  g_glAvailable = false;

template <class T>
bool LoadFunction(T& slot, const char* name)
{
    slot = reinterpret_cast<T>(GetRealGLFunction(name));
    return slot != nullptr;
}

bool LoadGL()
{
    bool ok = true;
    ok = LoadFunction(g_gl.GetIntegerv, "glGetIntegerv") && ok;
    ok = LoadFunction(g_gl.GetBooleanv, "glGetBooleanv") && ok;
    ok = LoadFunction(g_gl.IsEnabled, "glIsEnabled") && ok;
    ok = LoadFunction(g_gl.Enable, "glEnable") && ok;
    ok = LoadFunction(g_gl.Disable, "glDisable") && ok;
    ok = LoadFunction(g_gl.Viewport, "glViewport") && ok;
    ok = LoadFunction(g_gl.Scissor, "glScissor") && ok;
    ok = LoadFunction(g_gl.ColorMask, "glColorMask") && ok;
    ok = LoadFunction(g_gl.GenTextures, "glGenTextures") && ok;
    ok = LoadFunction(g_gl.DeleteTextures, "glDeleteTextures") && ok;
    ok = LoadFunction(g_gl.BindTexture, "glBindTexture") && ok;
    ok = LoadFunction(g_gl.TexParameteri, "glTexParameteri") && ok;
    ok = LoadFunction(g_gl.TexImage2D, "glTexImage2D") && ok;
    ok = LoadFunction(g_gl.PixelStorei, "glPixelStorei") && ok;
    ok = LoadFunction(g_gl.DrawArrays, "glDrawArrays") && ok;
    ok = LoadFunction(g_gl.GetCurrentContext, "wglGetCurrentContext") && ok;
    ok = LoadFunction(g_gl.CreateShader, "glCreateShader") && ok;
    ok = LoadFunction(g_gl.ShaderSource, "glShaderSource") && ok;
    ok = LoadFunction(g_gl.CompileShader, "glCompileShader") && ok;
    ok = LoadFunction(g_gl.GetShaderiv, "glGetShaderiv") && ok;
    ok = LoadFunction(g_gl.DeleteShader, "glDeleteShader") && ok;
    ok = LoadFunction(g_gl.CreateProgram, "glCreateProgram") && ok;
    ok = LoadFunction(g_gl.DeleteProgram, "glDeleteProgram") && ok;
    ok = LoadFunction(g_gl.AttachShader, "glAttachShader") && ok;
    ok = LoadFunction(g_gl.BindAttribLocation, "glBindAttribLocation") && ok;
    ok = LoadFunction(g_gl.LinkProgram, "glLinkProgram") && ok;
    ok = LoadFunction(g_gl.GetProgramiv, "glGetProgramiv") && ok;
    ok = LoadFunction(g_gl.UseProgram, "glUseProgram") && ok;
    ok = LoadFunction(g_gl.GetUniformLocation, "glGetUniformLocation") && ok;
    ok = LoadFunction(g_gl.Uniform2f, "glUniform2f") && ok;
    ok = LoadFunction(g_gl.Uniform1i, "glUniform1i") && ok;
    ok = LoadFunction(g_gl.GenBuffers, "glGenBuffers") && ok;
    ok = LoadFunction(g_gl.BindBuffer, "glBindBuffer") && ok;
    ok = LoadFunction(g_gl.BufferData, "glBufferData") && ok;
    ok = LoadFunction(g_gl.VertexAttribPointer, "glVertexAttribPointer") && ok;
    ok = LoadFunction(g_gl.EnableVertexAttribArray, "glEnableVertexAttribArray") && ok;
    ok = LoadFunction(g_gl.ActiveTexture, "glActiveTexture") && ok;
    ok = LoadFunction(g_gl.BlendFuncSeparate, "glBlendFuncSeparate") && ok;
    ok = LoadFunction(g_gl.BlendEquation, "glBlendEquation") && ok;
    ok = LoadFunction(g_gl.BlendEquationSeparate, "glBlendEquationSeparate") && ok;
    ok = LoadFunction(g_gl.GenVertexArrays, "glGenVertexArrays") && ok;
    ok = LoadFunction(g_gl.BindVertexArray, "glBindVertexArray") && ok;
    LoadFunction(g_gl.BindFramebuffer, "glBindFramebuffer");
    LoadFunction(g_gl.BindSampler, "glBindSampler");
    return ok;
}

// ===========================================================================
// Saved GL state
// ===========================================================================
struct SavedState {
    GLint     program = 0, vao = 0, arrayBuffer = 0, activeTexture = 0, texture = 0, sampler = 0, drawFramebuffer = 0;
    GLint     viewport[4] = {}, scissorBox[4] = {};
    GLint     blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0, blendEqRgb = 0, blendEqAlpha = 0;
    GLboolean colorMask[4] = {};
    bool      blend = false, depthTest = false, cullFace = false, scissorTest = false, stencilTest = false, srgb = false;
};

void SaveState(SavedState& s)
{
    g_gl.GetIntegerv(kCurrentProgram, &s.program);
    g_gl.GetIntegerv(kVertexArrayBinding, &s.vao);
    g_gl.GetIntegerv(kArrayBufferBinding, &s.arrayBuffer);
    g_gl.GetIntegerv(kActiveTexture, &s.activeTexture);
    g_gl.ActiveTexture(kTexture0);
    g_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture);
    if (g_gl.BindSampler)
        g_gl.GetIntegerv(kSamplerBinding, &s.sampler);
    if (g_gl.BindFramebuffer)
        g_gl.GetIntegerv(kDrawFramebufferBinding, &s.drawFramebuffer);
    g_gl.GetIntegerv(GL_VIEWPORT, s.viewport);
    g_gl.GetIntegerv(GL_SCISSOR_BOX, s.scissorBox);
    g_gl.GetIntegerv(kBlendSrcRgb, &s.blendSrcRgb);
    g_gl.GetIntegerv(kBlendDstRgb, &s.blendDstRgb);
    g_gl.GetIntegerv(kBlendSrcAlpha, &s.blendSrcAlpha);
    g_gl.GetIntegerv(kBlendDstAlpha, &s.blendDstAlpha);
    g_gl.GetIntegerv(kBlendEquationRgb, &s.blendEqRgb);
    g_gl.GetIntegerv(kBlendEquationAlpha, &s.blendEqAlpha);
    g_gl.GetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
    s.blend       = g_gl.IsEnabled(GL_BLEND) != GL_FALSE;
    s.depthTest   = g_gl.IsEnabled(GL_DEPTH_TEST) != GL_FALSE;
    s.cullFace    = g_gl.IsEnabled(GL_CULL_FACE) != GL_FALSE;
    s.scissorTest = g_gl.IsEnabled(GL_SCISSOR_TEST) != GL_FALSE;
    s.stencilTest = g_gl.IsEnabled(GL_STENCIL_TEST) != GL_FALSE;
    s.srgb        = g_gl.BindFramebuffer && g_gl.IsEnabled(kFramebufferSrgb) != GL_FALSE;  // GL 3.0+
}

void SetCapability(GLenum capability, bool enabled)
{
    if (enabled)
        g_gl.Enable(capability);
    else
        g_gl.Disable(capability);
}

void RestoreState(const SavedState& s)
{
    g_gl.BindVertexArray(s.vao);
    g_gl.BindBuffer(kArrayBuffer, s.arrayBuffer);
    g_gl.UseProgram(s.program);
    g_gl.BindTexture(GL_TEXTURE_2D, s.texture);  // texture unit 0 is still active here
    if (g_gl.BindSampler)
        g_gl.BindSampler(0, s.sampler);
    g_gl.ActiveTexture(s.activeTexture);
    if (g_gl.BindFramebuffer)
        g_gl.BindFramebuffer(kDrawFramebuffer, s.drawFramebuffer);
    g_gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    g_gl.Scissor(s.scissorBox[0], s.scissorBox[1], s.scissorBox[2], s.scissorBox[3]);
    g_gl.BlendFuncSeparate(s.blendSrcRgb, s.blendDstRgb, s.blendSrcAlpha, s.blendDstAlpha);
    g_gl.BlendEquationSeparate(s.blendEqRgb, s.blendEqAlpha);
    g_gl.ColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    SetCapability(GL_BLEND, s.blend);
    SetCapability(GL_DEPTH_TEST, s.depthTest);
    SetCapability(GL_CULL_FACE, s.cullFace);
    SetCapability(GL_SCISSOR_TEST, s.scissorTest);
    SetCapability(GL_STENCIL_TEST, s.stencilTest);
    if (g_gl.BindFramebuffer)
        SetCapability(kFramebufferSrgb, s.srgb);
}

// ===========================================================================
// Atlas: solid block + anti-aliased disc + glyphs + the logo
// ===========================================================================
constexpr int kAtlasSize = 2048;
constexpr int kDiscX = 64, kDiscY = 64, kDiscSize = 64;  // (well apart from the rest: its smaller copies stay clean)
constexpr int kGlyphTop = kDiscY + kDiscSize + 64;

// kFontTab: the tabs down the left. kFontButton: the buttons. kFontSection: a section's heading.
// kFontLabel: a setting's name. kFontValue: its value (and the search box). kFontBody: the help.
enum FontId { kFontTitle, kFontBody, kFontValue, kFontSmall, kFontTab, kFontButton, kFontSection, kFontLabel, kFontCount };

// The logo in the atlas, at the size it's drawn (kLogoHeight at the atlas's scale).
struct AtlasImage {
    bool  valid = false;
    float w = 0, h = 0, u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};
AtlasImage g_logo;
constexpr float kLogoHeight = 32.0f;
// The logo's "Remastered" as a white silhouette (the logo's alpha, nothing left of where it starts),
// right under the logo in the atlas: what the light sweeping across it lights (LogoSweep).
AtlasImage g_logoShine;

// The logo's PNG (logo_png.h), decoded once: RGBA, straight alpha.
struct DecodedImage {
    bool tried = false;
    int  w = 0, h = 0;
    std::vector<uint8_t> rgba;
};
DecodedImage g_logoImage;

template <class T>
void ReleaseCom(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

#ifdef _WIN32
bool DecodeLogo()
{
    DecodedImage& img = g_logoImage;
    if (img.tried)
        return !img.rgba.empty();
    img.tried = true;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // (RPC_E_CHANGED_MODE: COM is already up)
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(kLogoPng), static_cast<DWORD>(sizeof kLogoPng));
    if (SUCCEEDED(hr))
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr))
        hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr))
        hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr))
        hr = converter->GetSize(&w, &h);
    if (SUCCEEDED(hr) && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        img.rgba.resize(static_cast<size_t>(w) * h * 4);
        hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(img.rgba.size()), img.rgba.data());
        if (FAILED(hr))
            img.rgba.clear();
        img.w = static_cast<int>(w);
        img.h = static_cast<int>(h);
    }
    ReleaseCom(converter);
    ReleaseCom(frame);
    ReleaseCom(decoder);
    ReleaseCom(stream);
    ReleaseCom(factory);
    if (init == S_OK || init == S_FALSE)
        CoUninitialize();
    if (img.rgba.empty())
        Log("overlay: could not decode the logo (0x%08lX): the title is drawn as text", static_cast<unsigned long>(hr));
    return !img.rgba.empty();
}
#else
bool DecodeLogo()
{
    DecodedImage& img = g_logoImage;
    if (img.tried)
        return !img.rgba.empty();
    img.tried = true;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(kLogoPng, static_cast<int>(sizeof kLogoPng), &w, &h, &channels, 4);
    if (pixels && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        img.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
        img.w = w;
        img.h = h;
    }
    stbi_image_free(pixels);
    if (img.rgba.empty())
        Log("overlay: could not decode the logo (%s): the title is drawn as text", stbi_failure_reason());
    return !img.rgba.empty();
}
#endif

// The logo shrunk to w x h (area-averaged, through premultiplied alpha so its edges stay clean),
// written into the atlas's RGBA at (x, y).
void PlaceLogo(std::vector<uint8_t>& atlas, int x, int y, int w, int h)
{
    const DecodedImage& img = g_logoImage;
    const double sx = static_cast<double>(img.w) / w, sy = static_cast<double>(img.h) / h;
    for (int ty = 0; ty < h; ++ty) {
        const double y0 = ty * sy, y1 = (ty + 1) * sy;
        for (int tx = 0; tx < w; ++tx) {
            const double x0 = tx * sx, x1 = (tx + 1) * sx;
            double sum[4] = {}, area = 0;
            for (int py = static_cast<int>(y0); py < static_cast<int>(std::ceil(y1)) && py < img.h; ++py) {
                const double wy = std::fmin(y1, py + 1.0) - std::fmax(y0, static_cast<double>(py));
                for (int px = static_cast<int>(x0); px < static_cast<int>(std::ceil(x1)) && px < img.w; ++px) {
                    const double wx = std::fmin(x1, px + 1.0) - std::fmax(x0, static_cast<double>(px));
                    const double weight = wx * wy;
                    const uint8_t* p = &img.rgba[(static_cast<size_t>(py) * img.w + px) * 4];
                    const double a = p[3] / 255.0;
                    sum[0] += p[0] * a * weight;
                    sum[1] += p[1] * a * weight;
                    sum[2] += p[2] * a * weight;
                    sum[3] += a * weight;
                    area += weight;
                }
            }
            uint8_t* out = &atlas[(static_cast<size_t>(y + ty) * kAtlasSize + x + tx) * 4];
            const double alpha = area > 0 ? sum[3] / area : 0.0;
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<uint8_t>(sum[3] > 1e-6 ? Clamp(sum[c] / sum[3], 0.0, 255.0) + 0.5 : 0.0);
            out[3] = static_cast<uint8_t>(Clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
        }
    }
}

struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float width = 0, height = 0, bearing = 0, advance = 0;
    bool  valid = false;
};

struct Font {
    Glyph glyphs[256];
    float lineHeight = 0;
};

Font  g_fonts[kFontCount];
float g_atlasScale = 0;

bool BuildAtlas(GLuint texture, float scale)
{
    std::vector<uint8_t> coverage(kAtlasSize * kAtlasSize, 0);

    // Solid block for plain rectangles (sampled at its centre, away from the edges).
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            coverage[y * kAtlasSize + x] = 255;

    // Anti-aliased disc: knobs, the toggle and rounded corners are cut from it.
    const float radius = kDiscSize * 0.5f - 1.0f;
    for (int y = 0; y < kDiscSize; ++y) {
        for (int x = 0; x < kDiscSize; ++x) {
            const float dx = x + 0.5f - kDiscSize * 0.5f, dy = y + 0.5f - kDiscSize * 0.5f;
            const float alpha = Clamp(radius - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            coverage[(kDiscY + y) * kAtlasSize + kDiscX + x] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
        }
    }

#ifdef _WIN32
    // Glyphs: each drawn white-on-black by GDI at kSuper times its size, then shrunk into the atlas
    // (averaged), so they're smooth at any size. (Drawn at their own size, GDI turns smoothing off
    // for Arial at the small sizes the menu uses - the font asks it to - and the text came out jagged.)
    constexpr int kSuper = 4;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return false;
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);

    struct FontSpec { int pixels; int weight; const wchar_t* face; };
    const FontSpec specs[kFontCount] = {
        { 26, FW_BOLD,     L"Segoe UI" },  // kFontTitle
        { 16, FW_NORMAL,   L"Segoe UI" },  // kFontBody
        { 16, FW_BOLD,     L"Arial" },     // kFontValue
        { 13, FW_SEMIBOLD, L"Segoe UI" },  // kFontSmall
        { 28, FW_BOLD,     L"Arial" },     // kFontTab
        { 22, FW_BOLD,     L"Arial" },     // kFontButton
        { 17, FW_BOLD,     L"Arial" },     // kFontSection
        { 18, FW_NORMAL,   L"Arial" },     // kFontLabel
    };
    const int pad = 2;
    int penX = 0, penY = kGlyphTop, rowHeight = 0;
    bool fits = true;

    for (int f = 0; f < kFontCount && fits; ++f) {
        HFONT font = CreateFontW(-static_cast<int>(std::lround(specs[f].pixels * scale * kSuper)), 0, 0, 0, specs[f].weight,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, specs[f].face);
        HGDIOBJ previousFont = SelectObject(dc, font);
        TEXTMETRICW metrics = {};
        GetTextMetricsW(dc, &metrics);
        Font& out = g_fonts[f];
        out = Font{};
        // Drawn this far down the scratch bitmap that the baseline falls on a texel's edge: the letters'
        // feet stay sharp (not smeared over two texels).
        const int lift = (kSuper - metrics.tmAscent % kSuper) % kSuper;
        const int cellHeight = (metrics.tmHeight + lift + kSuper - 1) / kSuper;
        out.lineHeight = static_cast<float>(cellHeight);

        // A scratch bitmap a glyph big (at kSuper times), top-down.
        const int scratchW = 2 * metrics.tmMaxCharWidth + 4 * pad * kSuper, scratchH = cellHeight * kSuper;
        BITMAPINFO info = {};
        info.bmiHeader.biSize        = sizeof info.bmiHeader;
        info.bmiHeader.biWidth       = scratchW;
        info.bmiHeader.biHeight      = -scratchH;
        info.bmiHeader.biPlanes      = 1;
        info.bmiHeader.biBitCount    = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits) {
            if (bitmap)
                DeleteObject(bitmap);
            SelectObject(dc, previousFont);
            DeleteObject(font);
            fits = false;
            break;
        }
        HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
        const uint8_t* pixels = static_cast<const uint8_t*>(bits);

        // Printable ASCII plus the degree sign and middle dot.
        for (int code = 32; code <= 0xB7 && fits; ++code) {
            if (code > 126 && code != 0xB0 && code != 0xB7)
                continue;
            const wchar_t ch = static_cast<wchar_t>(code);
            ABC abc = {};
            if (!GetCharABCWidthsW(dc, ch, ch, &abc)) {
                SIZE size = {};
                GetTextExtentPoint32W(dc, &ch, 1, &size);
                abc.abcA = 0;
                abc.abcB = static_cast<UINT>(size.cx);
                abc.abcC = 0;
            }
            const int ink = static_cast<int>(abc.abcB > 0 ? abc.abcB : 1);
            const int cellWidth = (ink + kSuper - 1) / kSuper + 2 * pad;
            if (penX + cellWidth > kAtlasSize) {
                penX = 0;
                penY += rowHeight + pad;
                rowHeight = 0;
            }
            if (penY + cellHeight > kAtlasSize) {
                fits = false;
                break;
            }
            memset(bits, 0, static_cast<size_t>(scratchW) * scratchH * 4);
            TextOutW(dc, pad * kSuper - abc.abcA, lift, &ch, 1);  // its ink from pad texels in
            GdiFlush();
            const int readW = cellWidth * kSuper < scratchW ? cellWidth * kSuper : scratchW;
            for (int ty = 0; ty < cellHeight; ++ty) {
                for (int tx = 0; tx < cellWidth; ++tx) {
                    int sum = 0;
                    for (int sy = ty * kSuper; sy < (ty + 1) * kSuper; ++sy)
                        for (int sx = tx * kSuper; sx < (tx + 1) * kSuper && sx < readW; ++sx)
                            sum += pixels[(static_cast<size_t>(sy) * scratchW + sx) * 4 + 2];  // (grey: any channel)
                    coverage[static_cast<size_t>(penY + ty) * kAtlasSize + penX + tx] =
                        static_cast<uint8_t>((sum + kSuper * kSuper / 2) / (kSuper * kSuper));
                }
            }

            Glyph& g  = out.glyphs[code];
            g.u0      = static_cast<float>(penX) / kAtlasSize;
            g.v0      = static_cast<float>(penY) / kAtlasSize;
            g.u1      = static_cast<float>(penX + cellWidth) / kAtlasSize;
            g.v1      = static_cast<float>(penY + cellHeight) / kAtlasSize;
            g.width   = static_cast<float>(cellWidth);
            g.height  = static_cast<float>(cellHeight);
            g.bearing = static_cast<float>(abc.abcA) / kSuper - pad;
            g.advance = static_cast<float>(abc.abcA + static_cast<int>(abc.abcB) + abc.abcC) / kSuper;
            g.valid   = true;

            penX += cellWidth + pad;
            rowHeight = rowHeight > cellHeight ? rowHeight : cellHeight;
        }
        penX = 0;
        penY += rowHeight + pad;
        rowHeight = 0;
        SelectObject(dc, previousBitmap);
        DeleteObject(bitmap);
        SelectObject(dc, previousFont);
        DeleteObject(font);
    }
    DeleteDC(dc);
#else
    // Glyphs: the same faces from the system's fonts (fontconfig picks the file: Arial is Liberation
    // Sans or the like, Segoe UI the system's sans-serif), each drawn by stb_truetype at kSuper times
    // its size and shrunk into the atlas the same way as on Windows, laid out the same.
    constexpr int kSuper = 4;
    struct FontSpec { int pixels; int weight; const char* face; };
    const FontSpec specs[kFontCount] = {
        { 26, 700, "Segoe UI" },  // kFontTitle
        { 16, 400, "Segoe UI" },  // kFontBody
        { 16, 700, "Arial" },     // kFontValue
        { 13, 600, "Segoe UI" },  // kFontSmall
        { 28, 700, "Arial" },     // kFontTab
        { 22, 700, "Arial" },     // kFontButton
        { 17, 700, "Arial" },     // kFontSection
        { 18, 400, "Arial" },     // kFontLabel
    };
    const int pad = 2;
    int penX = 0, penY = kGlyphTop, rowHeight = 0;
    bool fits = true;

    for (int f = 0; f < kFontCount && fits; ++f) {
        const std::vector<unsigned char>& file = platform::FontFile(specs[f].face, specs[f].weight);
        stbtt_fontinfo info;
        if (file.empty() || !stbtt_InitFont(&info, file.data(), stbtt_GetFontOffsetForIndex(file.data(), 0))) {
            Log("overlay: no usable font for %s (install fontconfig and a TrueType sans-serif font)", specs[f].face);
            fits = false;
            break;
        }
        // (GDI's negative height is the em: the same here.)
        const float k = stbtt_ScaleForMappingEmToPixels(&info, std::round(specs[f].pixels * scale * kSuper));
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
        const int tmAscent = static_cast<int>(std::lround(ascent * k));
        const int tmHeight = static_cast<int>(std::lround((ascent - descent) * k));
        Font& out = g_fonts[f];
        out = Font{};
        const int lift = (kSuper - tmAscent % kSuper) % kSuper;
        const int cellHeight = (tmHeight + lift + kSuper - 1) / kSuper;
        out.lineHeight = static_cast<float>(cellHeight);

        for (int code = 32; code <= 0xB7 && fits; ++code) {
            if (code > 126 && code != 0xB0 && code != 0xB7)
                continue;
            int advanceUnits = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointHMetrics(&info, code, &advanceUnits, &lsb);
            stbtt_GetCodepointBitmapBox(&info, code, k, k, &x0, &y0, &x1, &y1);
            const int abcA = x0, abcB = x1 > x0 ? x1 - x0 : 0;
            const int advance = static_cast<int>(std::lround(advanceUnits * k));
            const int ink = abcB > 0 ? abcB : 1;
            const int cellWidth = (ink + kSuper - 1) / kSuper + 2 * pad;
            if (penX + cellWidth > kAtlasSize) {
                penX = 0;
                penY += rowHeight + pad;
                rowHeight = 0;
            }
            if (penY + cellHeight > kAtlasSize) {
                fits = false;
                break;
            }
            // The glyph at kSuper times, its ink pad texels in and its baseline tmAscent below lift.
            const int scratchW = cellWidth * kSuper, scratchH = cellHeight * kSuper;
            std::vector<uint8_t> scratch(static_cast<size_t>(scratchW) * scratchH, 0);
            int gw = 0, gh = 0, gx0 = 0, gy0 = 0;
            unsigned char* glyph = abcB > 0 ? stbtt_GetCodepointBitmap(&info, k, k, code, &gw, &gh, &gx0, &gy0) : nullptr;
            if (glyph) {
                const int ox = pad * kSuper, oy = lift + tmAscent + gy0;
                for (int gy = 0; gy < gh; ++gy) {
                    const int sy = oy + gy;
                    if (sy < 0 || sy >= scratchH)
                        continue;
                    for (int gx = 0; gx < gw; ++gx) {
                        const int sx = ox + gx;
                        if (sx >= 0 && sx < scratchW)
                            scratch[static_cast<size_t>(sy) * scratchW + sx] = glyph[gy * gw + gx];
                    }
                }
                stbtt_FreeBitmap(glyph, nullptr);
            }
            for (int ty = 0; ty < cellHeight; ++ty) {
                for (int tx = 0; tx < cellWidth; ++tx) {
                    int sum = 0;
                    for (int sy = ty * kSuper; sy < (ty + 1) * kSuper; ++sy)
                        for (int sx = tx * kSuper; sx < (tx + 1) * kSuper; ++sx)
                            sum += scratch[static_cast<size_t>(sy) * scratchW + sx];
                    coverage[static_cast<size_t>(penY + ty) * kAtlasSize + penX + tx] =
                        static_cast<uint8_t>((sum + kSuper * kSuper / 2) / (kSuper * kSuper));
                }
            }

            Glyph& g  = out.glyphs[code];
            g.u0      = static_cast<float>(penX) / kAtlasSize;
            g.v0      = static_cast<float>(penY) / kAtlasSize;
            g.u1      = static_cast<float>(penX + cellWidth) / kAtlasSize;
            g.v1      = static_cast<float>(penY + cellHeight) / kAtlasSize;
            g.width   = static_cast<float>(cellWidth);
            g.height  = static_cast<float>(cellHeight);
            g.bearing = static_cast<float>(abcA) / kSuper - pad;
            g.advance = static_cast<float>(advance) / kSuper;
            g.valid   = true;

            penX += cellWidth + pad;
            rowHeight = rowHeight > cellHeight ? rowHeight : cellHeight;
        }
        penX = 0;
        penY += rowHeight + pad;
        rowHeight = 0;
    }
#endif
    const int logoTop = penY + 2;  // (under the glyphs)
    if (!fits)
        return false;

    // RGBA, white everywhere: the shader multiplies the vertex colour by the alpha.
    std::vector<uint8_t> rgba(kAtlasSize * kAtlasSize * 4, 255);
    for (int i = 0; i < kAtlasSize * kAtlasSize; ++i)
        rgba[i * 4 + 3] = coverage[i];
    // The logo, in colour (the only texels that aren't white).
    g_logo = AtlasImage{};
    if (DecodeLogo()) {
        const int h = static_cast<int>(std::lround(kLogoHeight * scale));
        const int w = static_cast<int>(std::lround(static_cast<double>(h) * g_logoImage.w / g_logoImage.h));
        if (h > 4 && w + 2 <= kAtlasSize && logoTop + h + 2 <= kAtlasSize) {
            PlaceLogo(rgba, 1, logoTop, w, h);
            g_logo.valid = true;
            g_logo.w = static_cast<float>(w);
            g_logo.h = static_cast<float>(h);
            g_logo.u0 = 1.0f / kAtlasSize;
            g_logo.v0 = static_cast<float>(logoTop) / kAtlasSize;
            g_logo.u1 = static_cast<float>(1 + w) / kAtlasSize;
            g_logo.v1 = static_cast<float>(logoTop + h) / kAtlasSize;
            // "Remastered" starts right of the last red (the flag and "MXS" are red, it's silver).
            g_logoShine = AtlasImage{};
            const int shineTop = logoTop + h + 2;
            if (shineTop + h + 2 <= kAtlasSize) {
                int lastRed = -1;
                for (int x = 0; x < w; ++x)
                    for (int y = 0; y < h; ++y) {
                        const uint8_t* t = &rgba[(static_cast<size_t>(logoTop + y) * kAtlasSize + 1 + x) * 4];
                        if (t[3] > 96 && t[0] > 110 && t[0] > t[1] + 60)
                            lastRed = x;
                    }
                const int from = lastRed + 1;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const uint8_t* t = &rgba[(static_cast<size_t>(logoTop + y) * kAtlasSize + 1 + x) * 4];
                        uint8_t* o = &rgba[(static_cast<size_t>(shineTop + y) * kAtlasSize + 1 + x) * 4];
                        o[0] = o[1] = o[2] = 255;
                        o[3] = x >= from ? t[3] : 0;
                    }
                g_logoShine.valid = lastRed >= 0 && from < w;
                g_logoShine.w = static_cast<float>(w);
                g_logoShine.h = static_cast<float>(h);
                g_logoShine.u0 = 1.0f / kAtlasSize;
                g_logoShine.v0 = static_cast<float>(shineTop) / kAtlasSize;
                g_logoShine.u1 = static_cast<float>(1 + w) / kAtlasSize;
                g_logoShine.v1 = static_cast<float>(shineTop + h) / kAtlasSize;
            }
        }
    }

    GLint alignment = 4, rowLength = 0, unpackBuffer = 0;
    g_gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    g_gl.GetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    g_gl.GetIntegerv(kPixelUnpackBufferBinding, &unpackBuffer);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_gl.BindBuffer(kPixelUnpackBuffer, 0);

    // With its smaller copies (mipmaps): the disc drawn small (a slider's knob, a checkbox's corners, a
    // line's round ends) is cut from a copy its own size, its edge smooth. (Shrunk from the full-size
    // disc instead, the edge came out jagged.) Text and the logo are drawn at their own size: the
    // full-size atlas.
    g_gl.BindTexture(GL_TEXTURE_2D, texture);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlasSize, kAtlasSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    {
        std::vector<uint8_t> level = rgba, smaller;
        int size = kAtlasSize;
        for (int mip = 1; size > 1; ++mip) {
            const int half = size / 2;
            smaller.assign(static_cast<size_t>(half) * half * 4, 0);
            for (int y = 0; y < half; ++y)
                for (int x = 0; x < half; ++x)
                    for (int c = 0; c < 4; ++c) {
                        const size_t i0 = (static_cast<size_t>(2 * y) * size + 2 * x) * 4 + c, row = static_cast<size_t>(size) * 4;
                        smaller[(static_cast<size_t>(y) * half + x) * 4 + c] =
                            static_cast<uint8_t>((level[i0] + level[i0 + 4] + level[i0 + row] + level[i0 + row + 4] + 2) / 4);
                    }
            g_gl.TexImage2D(GL_TEXTURE_2D, mip, GL_RGBA8, half, half, 0, GL_RGBA, GL_UNSIGNED_BYTE, smaller.data());
            level.swap(smaller);
            size = half;
        }
    }

    g_gl.BindBuffer(kPixelUnpackBuffer, unpackBuffer);
    g_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    g_gl.PixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    g_atlasScale = scale;
    Log("overlay: font atlas built at scale %.2f (%d of %d rows used%s)", scale,
        g_logo.valid ? logoTop + static_cast<int>(g_logo.h) * (g_logoShine.valid ? 2 : 1) + (g_logoShine.valid ? 2 : 0) : logoTop,
        kAtlasSize, g_logo.valid ? ", logo included" : ", no logo");
    return true;
}

// ===========================================================================
// GPU resources (per GL context)
// ===========================================================================
const char* const kVertexBody =
    "IN vec2 a_pos;\n"
    "IN vec2 a_uv;\n"
    "IN vec4 a_color;\n"
    "OUT vec2 v_uv;\n"
    "OUT vec4 v_color;\n"
    "uniform vec2 u_screen;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "}\n";

const char* const kFragmentBody =
    "IN vec2 v_uv;\n"
    "IN vec4 v_color;\n"
    "uniform sampler2D u_atlas;\n"
    "void main() { FRAG_COLOR = v_color * TEX(u_atlas, v_uv); }\n";  // (the atlas is white but for the logo)

// The same shader in the three GLSL dialects a game context might accept (core 3.2+, 3.0/3.1,
// and 2.x compatibility); the first one that compiles wins.
struct ShaderDialect {
    const char* vertexHeader;
    const char* fragmentHeader;
};
const ShaderDialect kDialects[] = {
    { "#version 150\n#define IN in\n#define OUT out\n",
      "#version 150\n#define IN in\n#define TEX texture\nout vec4 o_color;\n#define FRAG_COLOR o_color\n" },
    { "#version 130\n#define IN in\n#define OUT out\n",
      "#version 130\n#define IN in\n#define TEX texture\nout vec4 o_color;\n#define FRAG_COLOR o_color\n" },
    { "#version 120\n#define IN attribute\n#define OUT varying\n",
      "#version 120\n#define IN varying\n#define TEX texture2D\n#define FRAG_COLOR gl_FragColor\n" },
};

struct Vertex {
    float    x, y, u, v;
    uint32_t color;  // RGBA bytes
};

struct GpuResources {
    HGLRC  context = nullptr;
    bool   failed  = false;
    GLuint program = 0, vao = 0, vbo = 0, texture = 0;
    GLint  screenUniform = -1, atlasUniform = -1;
};
GpuResources g_gpu;

bool Compiled(GLuint shader)
{
    GLint status = 0;
    g_gl.GetShaderiv(shader, kCompileStatus, &status);
    return status != 0;
}

GLuint CompileProgram()
{
    for (const ShaderDialect& dialect : kDialects) {
        const char* vertexSource[] = { dialect.vertexHeader, kVertexBody };
        const char* fragmentSource[] = { dialect.fragmentHeader, kFragmentBody };
        const GLuint vs = g_gl.CreateShader(kVertexShader);
        const GLuint fs = g_gl.CreateShader(kFragmentShader);
        g_gl.ShaderSource(vs, 2, vertexSource, nullptr);
        g_gl.ShaderSource(fs, 2, fragmentSource, nullptr);
        g_gl.CompileShader(vs);
        g_gl.CompileShader(fs);

        GLuint program = 0;
        if (Compiled(vs) && Compiled(fs)) {
            program = g_gl.CreateProgram();
            g_gl.AttachShader(program, vs);
            g_gl.AttachShader(program, fs);
            g_gl.BindAttribLocation(program, 0, "a_pos");
            g_gl.BindAttribLocation(program, 1, "a_uv");
            g_gl.BindAttribLocation(program, 2, "a_color");
            g_gl.LinkProgram(program);
            GLint linked = 0;
            g_gl.GetProgramiv(program, kLinkStatus, &linked);
            if (!linked) {
                g_gl.DeleteProgram(program);
                program = 0;
            }
        }
        g_gl.DeleteShader(vs);  // freed together with the program
        g_gl.DeleteShader(fs);
        if (program) {
            Log("overlay: ready (GLSL %.11s)", dialect.vertexHeader + 1);
            return program;
        }
    }
    return 0;
}

// Creates (or recreates, after a context change) everything the panel draws with.
bool EnsureResources(float scale)
{
    const HGLRC context = g_gl.GetCurrentContext();
    if (context != g_gpu.context) {
        // New context: the old objects belonged to the old one.
        g_gpu = GpuResources{};
        g_gpu.context = context;
        g_atlasScale = 0;
    }
    if (g_gpu.failed)
        return false;

    if (!g_gpu.program) {
        g_gpu.program = CompileProgram();
        if (!g_gpu.program) {
            Log("overlay: could not compile the panel shader; settings panel disabled");
            g_gpu.failed = true;
            return false;
        }
        g_gpu.screenUniform = g_gl.GetUniformLocation(g_gpu.program, "u_screen");
        g_gpu.atlasUniform  = g_gl.GetUniformLocation(g_gpu.program, "u_atlas");
    }
    if (!g_gpu.vao) {
        g_gl.GenVertexArrays(1, &g_gpu.vao);
        g_gl.GenBuffers(1, &g_gpu.vbo);
        g_gl.BindVertexArray(g_gpu.vao);
        g_gl.BindBuffer(kArrayBuffer, g_gpu.vbo);
        g_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, x)));
        g_gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, u)));
        g_gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, color)));
        g_gl.EnableVertexAttribArray(0);
        g_gl.EnableVertexAttribArray(1);
        g_gl.EnableVertexAttribArray(2);
    }
    if (!g_gpu.texture)
        g_gl.GenTextures(1, &g_gpu.texture);
    if (std::fabs(scale - g_atlasScale) > 0.01f && !BuildAtlas(g_gpu.texture, scale)) {
        Log("overlay: could not build the font atlas; settings panel disabled");
        g_gpu.failed = true;
        return false;
    }
    return true;
}

// ===========================================================================
// Drawing primitives (pixel coordinates, top-left origin)
// ===========================================================================
std::vector<Vertex> g_batch;

uint32_t Rgba(int r, int g, int b, float a)
{
    const uint32_t alpha = static_cast<uint32_t>(Clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return static_cast<uint32_t>(r) | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b) << 16 | alpha << 24;
}
uint32_t White(float a) { return Rgba(255, 255, 255, a); }
uint32_t Gray(int level, float a = 1.0f) { return Rgba(level, level, level, a); }
// Colour theme: black and red. Near-black panels with a red glow rising from the bottom left and a
// red outline, red for whatever is on or selected, white text.
const uint32_t kPanelColor = Rgba(14, 14, 16, 0.97f);
const uint32_t kPanelSolid = Rgba(14, 14, 16, 1.0f);
const uint32_t kBoxColor   = Rgba(8, 8, 10, 0.95f);       // the settings' box
const uint32_t kBoxSolid   = Rgba(8, 8, 10, 1.0f);
const uint32_t kAccent     = Rgba(206, 18, 26, 1.0f);    // on, selected, primary
const uint32_t kAccentHot  = Rgba(236, 44, 50, 1.0f);
const uint32_t kAccentDeep = Rgba(96, 6, 12, 1.0f);      // the far end of a red gradient
const uint32_t kBorder     = Rgba(200, 16, 24, 0.95f);   // panel outline
const uint32_t kLine       = Rgba(255, 255, 255, 0.10f); // separators
const uint32_t kOutline    = Rgba(118, 118, 124, 1.0f);  // idle outlines: value boxes, checkboxes
const uint32_t kTrack      = Rgba(168, 16, 24, 1.0f);    // slider track
const uint32_t kLabel      = Rgba(232, 232, 234, 1.0f);
const uint32_t kDim        = Rgba(116, 116, 122, 1.0f);
const uint32_t kSection    = Rgba(236, 38, 44, 1.0f);    // section headings
const uint32_t kInfo       = Rgba(150, 150, 156, 1.0f);  // the readout under the settings

void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color)
{
    const Vertex a{ x0, y0, u0, v0, color }, b{ x1, y0, u1, v0, color };
    const Vertex c{ x1, y1, u1, v1, color }, d{ x0, y1, u0, v1, color };
    g_batch.insert(g_batch.end(), { a, b, c, a, c, d });
}

// A quad with a colour at each corner (top left, top right, bottom right, bottom left), all from the
// atlas's solid block.
void ShadedQuad(float x0, float y0, float x1, float y1, uint32_t c00, uint32_t c10, uint32_t c11, uint32_t c01)
{
    const float u = 4.0f / kAtlasSize;
    const Vertex a{ x0, y0, u, u, c00 }, b{ x1, y0, u, u, c10 };
    const Vertex c{ x1, y1, u, u, c11 }, d{ x0, y1, u, u, c01 };
    g_batch.insert(g_batch.end(), { a, b, c, a, c, d });
}

void GradientH(float x, float y, float w, float h, uint32_t left, uint32_t right)
{
    if (w > 0 && h > 0)
        ShadedQuad(std::round(x), std::round(y), std::round(x + w), std::round(y + h), left, right, right, left);
}

void GradientV(float x, float y, float w, float h, uint32_t top, uint32_t bottom)
{
    if (w > 0 && h > 0)
        ShadedQuad(std::round(x), std::round(y), std::round(x + w), std::round(y + h), top, top, bottom, bottom);
}

// A soft glow of (r, g, b): maxAlpha at (cx, cy), fading out over the radii.
struct GlowSpec {
    float cx, cy, rx, ry;
    int   r, g, b;
    float maxAlpha;
    uint32_t At(float px, float py) const
    {
        const float dx = (px - cx) / rx, dy = (py - cy) / ry;
        const float t = Clamp(1.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
        return Rgba(r, g, b, maxAlpha * t * t * (3.0f - 2.0f * t));
    }
};

// The glow over a rectangle.
void Glow(float x, float y, float w, float h, const GlowSpec& glow)
{
    if (w <= 0 || h <= 0)
        return;
    const int nx = static_cast<int>(Clamp(w / 40.0f, 2.0f, 48.0f)), ny = static_cast<int>(Clamp(h / 40.0f, 1.0f, 32.0f));
    const auto At = [&](float px, float py) { return glow.At(px, py); };
    for (int j = 0; j < ny; ++j) {
        const float y0 = y + h * j / ny, y1 = y + h * (j + 1) / ny;
        for (int i = 0; i < nx; ++i) {
            const float x0 = x + w * i / nx, x1 = x + w * (i + 1) / nx;
            ShadedQuad(x0, y0, x1, y1, At(x0, y0), At(x1, y0), At(x1, y1), At(x0, y1));
        }
    }
}

// Parts of the batch drawn only inside a rectangle (the scrolled settings list): [begin, end).
struct ClipRange {
    size_t begin, end;
    float  x, y, w, h;
};
std::vector<ClipRange> g_clips;

// A light sweeping across the logo's "Remastered" (drawn over the logo at x, y): a soft, slanted
// band of white through its silhouette, left to right, now and then. Only while the menu is drawn:
// a few triangles, nothing else.
constexpr double kSweepEvery   = 6.0;  // s from one sweep to the next
constexpr double kSweepLasts   = 1.3;  // s it takes to cross
constexpr double kSweepFirstAt = 0.8;  // s after the menu opens
void LogoSweep(float x, float y, double sinceOpen)
{
    const AtlasImage& m = g_logoShine;
    // (For automated tests: held this far across, 0..1.)
    static const double testAt = [] {
        char text[16] = "";
        return GetEnvironmentVariableA("MXCAM_TEST_LOGO_SWEEP", text, sizeof text) > 0 ? atof(text) : -1.0;
    }();
    if (!m.valid || (testAt < 0 && sinceOpen < kSweepFirstAt))
        return;
    const double t = std::fmod(sinceOpen - kSweepFirstAt, kSweepEvery);
    if (testAt < 0 && t >= kSweepLasts)
        return;
    double p = testAt >= 0 ? testAt : t / kSweepLasts;
    p = p * p * (3.0 - 2.0 * p);  // eased in and out
    const float w = m.w, h = m.h;
    const float slant = h * 0.55f;  // the band leans forward: its top ahead of its bottom
    const float half = h * 0.85f;   // half its width, the soft edges included
    // From wholly left of "Remastered" to wholly right of the logo.
    const float from = x + w * 0.35f - half - slant, to = x + w + half;
    const float centre = from + static_cast<float>(p) * (to - from);
    // (Each corner's texel is where it is on the logo: the silhouette lines up with the logo's.)
    const auto Corner = [&](float sx, float sy, float alpha) {
        const float u = m.u0 + (sx - x) / w * (m.u1 - m.u0), v = m.v0 + (sy - y) / h * (m.v1 - m.v0);
        return Vertex{ sx, sy, u, v, White(alpha) };
    };
    const auto Piece = [&](float left, float right, float alphaLeft, float alphaRight) {
        const Vertex bl = Corner(left, y + h, alphaLeft), br = Corner(right, y + h, alphaRight);
        const Vertex tr = Corner(right + slant, y, alphaRight), tl = Corner(left + slant, y, alphaLeft);
        g_batch.insert(g_batch.end(), { tl, tr, br, tl, br, bl });
    };
    const size_t begin = g_batch.size();
    const float peak = 0.95f, core = half * 0.22f;
    Piece(centre - half, centre - core, 0.0f, peak);
    Piece(centre - core, centre + core, peak, peak);
    Piece(centre + core, centre + half, peak, 0.0f);
    g_clips.push_back({ begin, g_batch.size(), x, y, w, h });  // (nothing past the logo's edges)
}

void Segment(float x0, float y0, float x1, float y1, float w, uint32_t color);  // (with the dab indicator, below)

// A chevron (an arrow head) centred at (x, y): dir 0 left, 1 right, 2 up, 3 down.
void Chevron(float x, float y, float size, int dir, float width, uint32_t color)
{
    const float a = size;  // (the arrow is size wide, twice that tall)
    switch (dir) {
    case 0:
        Segment(x + a * 0.5f, y - size, x - a * 0.5f, y, width, color);
        Segment(x - a * 0.5f, y, x + a * 0.5f, y + size, width, color);
        break;
    case 1:
        Segment(x - a * 0.5f, y - size, x + a * 0.5f, y, width, color);
        Segment(x + a * 0.5f, y, x - a * 0.5f, y + size, width, color);
        break;
    case 2:
        Segment(x - size, y + a * 0.5f, x, y - a * 0.5f, width, color);
        Segment(x, y - a * 0.5f, x + size, y + a * 0.5f, width, color);
        break;
    default:
        Segment(x - size, y - a * 0.5f, x, y + a * 0.5f, width, color);
        Segment(x, y + a * 0.5f, x + size, y - a * 0.5f, width, color);
        break;
    }
}

void Rect(float x, float y, float w, float h, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    const float u = 4.0f / kAtlasSize;  // middle of the solid block
    Quad(std::round(x), std::round(y), std::round(x + w), std::round(y + h), u, u, u, u, color);
}

constexpr float kDiscU0 = static_cast<float>(kDiscX) / kAtlasSize;
constexpr float kDiscV0 = static_cast<float>(kDiscY) / kAtlasSize;
constexpr float kDiscU1 = static_cast<float>(kDiscX + kDiscSize) / kAtlasSize;
constexpr float kDiscV1 = static_cast<float>(kDiscY + kDiscSize) / kAtlasSize;
constexpr float kDiscUM = (kDiscU0 + kDiscU1) * 0.5f;
constexpr float kDiscVM = (kDiscV0 + kDiscV1) * 0.5f;

void Disc(float cx, float cy, float r, uint32_t color)
{
    Quad(cx - r, cy - r, cx + r, cy + r, kDiscU0, kDiscV0, kDiscU1, kDiscV1, color);
}

// Rectangle with anti-aliased rounded corners cut from the disc.
void RoundedRect(float x, float y, float w, float h, float r, uint32_t color)
{
    r = std::floor(std::fmin(r, std::fmin(w, h) * 0.5f));
    Quad(x, y, x + r, y + r, kDiscU0, kDiscV0, kDiscUM, kDiscVM, color);
    Quad(x + w - r, y, x + w, y + r, kDiscUM, kDiscV0, kDiscU1, kDiscVM, color);
    Quad(x, y + h - r, x + r, y + h, kDiscU0, kDiscVM, kDiscUM, kDiscV1, color);
    Quad(x + w - r, y + h - r, x + w, y + h, kDiscUM, kDiscVM, kDiscU1, kDiscV1, color);
    Rect(x + r, y, w - 2 * r, h, color);
    Rect(x, y + r, r, h - 2 * r, color);
    Rect(x + w - r, y + r, r, h - 2 * r, color);
}

// The glow over a rounded rectangle's inside (its rounded corners cut from the disc, as RoundedRect's).
void GlowRounded(float x, float y, float w, float h, float r, const GlowSpec& glow)
{
    r = std::floor(std::fmin(r, std::fmin(w, h) * 0.5f));
    Glow(x + r, y, w - 2 * r, h, glow);
    Glow(x, y + r, r, h - 2 * r, glow);
    Glow(x + w - r, y + r, r, h - 2 * r, glow);
    const auto Corner = [&](float x0, float y0, float u0, float v0, float u1, float v1) {
        const float x1 = x0 + r, y1 = y0 + r;
        const Vertex a{ x0, y0, u0, v0, glow.At(x0, y0) }, b{ x1, y0, u1, v0, glow.At(x1, y0) };
        const Vertex c{ x1, y1, u1, v1, glow.At(x1, y1) }, d{ x0, y1, u0, v1, glow.At(x0, y1) };
        g_batch.insert(g_batch.end(), { a, b, c, a, c, d });
    };
    Corner(x, y, kDiscU0, kDiscV0, kDiscUM, kDiscVM);
    Corner(x + w - r, y, kDiscUM, kDiscV0, kDiscU1, kDiscVM);
    Corner(x, y + h - r, kDiscU0, kDiscVM, kDiscUM, kDiscV1);
    Corner(x + w - r, y + h - r, kDiscUM, kDiscVM, kDiscU1, kDiscV1);
}

float LineHeight(FontId font) { return g_fonts[font].lineHeight; }

float TextWidth(FontId font, const char* text, float tracking)
{
    float width = 0;
    int count = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p, ++count)
        width += g_fonts[font].glyphs[*p].advance;
    return count ? width + tracking * (count - 1) : 0;
}

constexpr int kMaxWrapLines = 6;
constexpr int kWrapLineSize = 160;

// Splits text into lines no wider than width, breaking at spaces. Returns the line count.
int WrapText(FontId font, const char* text, float width, char (&lines)[kMaxWrapLines][kWrapLineSize])
{
    int count = 0;
    while (*text && count < kMaxWrapLines) {
        char* line = lines[count++];
        size_t length = 0;
        while (*text) {
            const char* end = text;
            while (*end && *end != ' ')
                ++end;
            const size_t word = static_cast<size_t>(end - text);
            const size_t joined = length + (length ? 1 : 0) + word;
            if (joined >= kWrapLineSize)
                break;
            char candidate[kWrapLineSize];
            snprintf(candidate, sizeof candidate, "%.*s%s%.*s", static_cast<int>(length), line, length ? " " : "",
                     static_cast<int>(word), text);
            if (length && TextWidth(font, candidate, 0) > width)
                break;  // the word starts the next line
            memcpy(line, candidate, joined + 1);
            length = joined;
            text = *end ? end + 1 : end;
        }
        line[length] = '\0';
    }
    return count;
}

void Text(FontId font, float x, float y, const char* text, uint32_t color, float tracking)
{
    float pen = x;
    y = std::round(y);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        const Glyph& g = g_fonts[font].glyphs[*p];
        if (g.valid && *p != ' ') {
            const float gx = std::round(pen + g.bearing);
            Quad(gx, y, gx + g.width, y + g.height, g.u0, g.v0, g.u1, g.v1, color);
        }
        pen += g.advance + tracking;
    }
}

// ===========================================================================
// Panel state, input and window subclass
// ===========================================================================
enum class RowKind { Slider, Toggle, Choice, Key, Pad };

struct RowDef {
    int              tab;
    const char*      section;   // starts a new section when set
    const char*      label;
    RowKind          kind;
    double Config::* number;    // Slider
    bool Config::*   flag;      // Toggle / Choice of two
    double           min, max, step;
    double           display;   // multiplier for the shown value (e.g. fraction -> %)
    int              decimals;
    const char*      unit;
    const char*      zeroText;  // shown instead of the minimum, e.g. "Off"
    const char*      offText;   // Choice: the first two options
    const char*      onText;
    bool             customOnly;  // only does something with the custom camera
    int Config::*    key = nullptr;  // Key: a virtual-key code, picked by pressing the key
    const char*      help = nullptr; // shown beside the panel while the row is selected / under the mouse
    bool             locked = false; // work in progress: drawn dimmed and can't be changed
    const char*      badge = nullptr; // a small tag after the label ("COMING SOON")
    int Config::*    option = nullptr;     // Choice of three or four: 0 / 1 / 2 (/ 3), instead of flag
    const char*      thirdText = nullptr;  // its third option
    const char*      fourthText = nullptr; // and fourth, if it has one
    double Config::* sideNumber = nullptr; // Slider: what it sets instead with the helmet camera on a side mount
    double Config::* chestNumber = nullptr; // and on the chest mount
    bool             clock = false;          // Slider: hours, shown as a time of day (HH:MM)
    int              column = -1;            // starts this column of its tab (later rows follow it)
    int Config::*    pad = nullptr;          // Pad: a gamepad::Control, picked by pressing it

    RowDef Col(int index) const
    {
        RowDef def = *this;
        def.column = index;
        return def;
    }
    RowDef Side(double Config::* field) const
    {
        RowDef def = *this;
        def.sideNumber = field;
        return def;
    }
    RowDef Chest(double Config::* field) const
    {
        RowDef def = *this;
        def.chestNumber = field;
        return def;
    }

    RowDef Help(const char* text) const
    {
        RowDef def = *this;
        def.help = text;
        return def;
    }
    RowDef Locked() const
    {
        RowDef def = *this;
        def.locked = true;
        return def;
    }
    RowDef Badge(const char* text) const
    {
        RowDef def = *this;
        def.badge = text;
        return def;
    }
    RowDef Clock() const
    {
        RowDef def = *this;
        def.clock = true;
        return def;
    }
};

RowDef Slider(int tab, const char* section, const char* label, double Config::* field, double min, double max,
              double step, double display, int decimals, const char* unit, const char* zeroText = nullptr,
              bool customOnly = false)
{
    return { tab, section, label, RowKind::Slider, field, nullptr, min, max, step, display, decimals, unit,
             zeroText, nullptr, nullptr, customOnly };
}

RowDef Toggle(int tab, const char* section, const char* label, bool Config::* flag, bool customOnly = false)
{
    return { tab, section, label, RowKind::Toggle, nullptr, flag, 0, 1, 1, 1, 0, "", nullptr, nullptr, nullptr, customOnly };
}

RowDef Choice(int tab, const char* section, const char* label, bool Config::* flag, const char* offText, const char* onText)
{
    return { tab, section, label, RowKind::Choice, nullptr, flag, 0, 1, 1, 1, 0, "", nullptr, offText, onText, false };
}

RowDef Choice3(int tab, const char* section, const char* label, int Config::* option, const char* first,
               const char* second, const char* third)
{
    RowDef def = Choice(tab, section, label, nullptr, first, second);
    def.option = option;
    def.thirdText = third;
    return def;
}

RowDef Choice4(int tab, const char* section, const char* label, int Config::* option, const char* first,
               const char* second, const char* third, const char* fourth)
{
    RowDef def = Choice3(tab, section, label, option, first, second, third);
    def.fourthText = fourth;
    return def;
}

// The field a slider sets: per helmet camera mount where it has them.
double Config::* SliderField(const RowDef& def, const Config& cfg)
{
    if (def.chestNumber && cfg.helmetMount == 3)
        return def.chestNumber;
    if (def.sideNumber && (cfg.helmetMount == 1 || cfg.helmetMount == 2))
        return def.sideNumber;
    return def.number;
}

RowDef KeyRow(int tab, const char* section, const char* label, int Config::* key)
{
    return { tab, section, label, RowKind::Key, nullptr, nullptr, 0, 1, 1, 1, 0, "", nullptr, nullptr, nullptr, false, key };
}

RowDef PadRow(int tab, const char* section, const char* label, int Config::* control)
{
    RowDef def = KeyRow(tab, section, label, nullptr);
    def.kind = RowKind::Pad;
    def.pad = control;
    return def;
}

const char* const kTabs[] = { "FIRST PERSON", "LOOK", "GRAPHICS", "WEATHER", "REPLAY", "KEYBINDS", "SOUNDS", "OTHER" };
constexpr int kTabFirstPerson = 0, kTabLook = 1, kTabGraphics = 2, kTabWeather = 3, kTabReplay = 4, kTabKeybinds = 5,
              kTabSounds = 6, kTabOther = 7;
constexpr int kTabCount = static_cast<int>(sizeof kTabs / sizeof kTabs[0]);
// Preset codes (config.cpp) number their tab themselves - 0 the first person camera's, 1 the
// graphics' - as they always have (codes already shared keep loading), whatever order the menu's
// tabs are in.
int CodeOfTab(int tab) { return tab == kTabFirstPerson ? 0 : tab == kTabGraphics ? 1 : -1; }
int TabOfCode(int code) { return code == 0 ? kTabFirstPerson : code == 1 ? kTabGraphics : -1; }

// Each tab's rows, column by column (.Col starts the next one).
const RowDef kRows[] = {
    // ======== FIRST PERSON ========
    Toggle(kTabFirstPerson, "CAMERA",          "First person mod",     &Config::enabled)
        .Help("The first-person camera mod (its key is on the KEYBINDS tab). Off gives you the game's own "
              "cameras and replays untouched, and only the shaders stay on (and Controller look, if it's "
              "on: it still turns the game's camera).")
        .Col(0),
    Choice(kTabFirstPerson, nullptr,           "Camera",               &Config::customCamera, "GAME", "CUSTOM")
        .Help("GAME adjusts the game's own first-person camera (and its 1st Person Angle). CUSTOM builds "
              "the view from your bike instead. The first time, ride a few metres so it can find your bike; "
              "after that it knows it as soon as you spawn."),
    Slider(kTabFirstPerson, nullptr,           "Look-down angle",      &Config::cameraAngleDeg,   -10.0, 30.0, 0.5,  1.0,   1, "\xB0", nullptr, true)
        .Help("Custom camera: how far the view tilts down. Negative tilts it up. (The GAME camera uses "
              "the game's own 1st Person Angle.)"),
    Slider(kTabFirstPerson, "STABILIZATION",   "Horizon lock",         &Config::horizonLock,      0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("How much of the bike's lean is removed from the view. 0% leans with the bike, 100% keeps "
              "the horizon level."),
    Slider(kTabFirstPerson, nullptr,           "Pitch lock",           &Config::pitchLock,        0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("How much of the bike's nose up / down tilt (wheelies, jump faces) is removed. 0% tilts "
              "with the bike, 100% keeps the view steady."),
    Slider(kTabFirstPerson, nullptr,           "Steadiness",           &Config::steadiness,       0.0,   2.0,  0.01, 100.0, 0, "%", "Off")
        .Help("Keeps the view calm: the quick jolts of the bike (bumps, landings, the suspension diving "
              "under braking) don't reach it. It still eases into a corner's lean or a wheelie, as far as "
              "the locks above let it. Off follows every jolt."),
    Slider(kTabFirstPerson, "HEAD POSITION",   "Head forward / back",  &Config::headForward,      -3.0,  3.0,  0.01, 1.0,   2, " m")
        .Help("Moves your eyes forward (+) or back (-) along the bike."),
    Slider(kTabFirstPerson, nullptr,           "Head height",          &Config::headHeight,       -3.0,  3.0,  0.01, 1.0,   2, " m")
        .Help("Moves your eyes up (+) or down (-)."),
    Toggle(kTabFirstPerson, nullptr,           "Hide torso & head",    &Config::hideOwnBody)
        .Help("Hides your own rider's torso and head (helmet included), so a high FOV doesn't show "
              "them from the inside. Arms and legs stay, and your shadow keeps them all. Only yours: "
              "other riders keep theirs."),
    Slider(kTabFirstPerson, "CORNERS",         "Look to apex",         &Config::apexLook,         0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("In corners the head turns toward the apex, the way the bike is turning: more the "
              "tighter the corner. Higher looks further into the turn (100% looks about a second "
              "down your line, at most 45\xB0). Off at walking pace and in the air.")
        .Col(1),
    Slider(kTabFirstPerson, nullptr,           "Apex turn speed",      &Config::apexSpeed,        0.5,   5.0,  0.5, 1.0,   1, "")
        .Help("How quickly the head turns toward the apex and back to straight ahead after the "
              "corner. It eases in and out gently either way; lower is more gradual: 2.5 takes "
              "about a second to settle, 1 about three. Too low and it lags behind short corners."),
    Slider(kTabFirstPerson, nullptr,           "Anticipation",         &Config::apexAnticipation, 0.0,   0.5,  0.05, 1.0,  2, " s",
           "Work in progress")
        .Help("Work in progress, off for now. It will set how early the head turns: reading where the "
              "corner is going, so it turns in sooner as a corner tightens and comes back to straight "
              "ahead sooner as it opens up on the exit.")
        .Locked(),
    Toggle(kTabLook,        "LOOKING AROUND",  "Controller look",      &Config::controllerLook)
        .Help("Look around with a controller: the d-pad's left / right to look sideways and down to "
              "look behind, unless you bind other buttons, paddles or stick directions on the KEYBINDS "
              "tab. Release to look ahead again. It works with the First person mod off too: then it "
              "only turns the game's own camera (not in replays)."),
    Slider(kTabLook,        nullptr,           "Side look angle",      &Config::lookSideDeg,      30.0,   90.0, 1.0, 1.0,   0, "\xB0")
        .Help("How far the head turns when looking left / right with the controller."),
    Slider(kTabLook,        nullptr,           "Turn speed",           &Config::lookTurnSpeed,    2.0,   40.0, 1.0, 1.0,   0, "")
        .Help("How quickly the head turns to look left / right / back and ahead again. Higher is "
              "faster: 12 turns 90\xB0 in about a quarter of a second, 40 is nearly instant."),
    Slider(kTabLook,        nullptr,           "Sideways: forward",    &Config::lookSideForward,  -1.0,  1.0,  0.01, 1.0,   2, " m", nullptr, true)
        .Help("Custom camera: moves your eyes forward (+) or back (-) while looking left / right."),
    Slider(kTabLook,        nullptr,           "Sideways: height",     &Config::lookSideHeight,   -1.0,  1.0,  0.01, 1.0,   2, " m", nullptr, true)
        .Help("Custom camera: moves your eyes up (+) or down (-) while looking left / right."),
    Slider(kTabLook,        nullptr,           "Back: forward",        &Config::lookBackForward,  -1.0,  1.0,  0.01, 1.0,   2, " m", nullptr, true)
        .Help("Custom camera: moves your eyes forward (+) or back (-) while looking back."),
    Slider(kTabLook,        nullptr,           "Back: height",         &Config::lookBackHeight,   -1.0,  1.0,  0.01, 1.0,   2, " m", nullptr, true)
        .Help("Custom camera: moves your eyes up (+) or down (-) while looking back."),
    Slider(kTabFirstPerson, "IN THE AIR",      "Looseness",            &Config::airLooseness,     0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("In the air only: the view keeps pointing forward while the bike swings left / right "
              "(scrubs, whips), by up to 15\xB0 x this. Off follows the bike tightly.")
        .Col(2),
    Slider(kTabFirstPerson, nullptr,           "Whip lock",            &Config::whipLock,         0.0,   1.0,  0.01, 100.0, 0, "%", "Off")
        .Help("In the air only: holds the view on your direction of travel, horizon level, at your "
              "normal look-down angle, while the bike swings, leans and tilts under it. 100% keeps "
              "it completely straight; it eases back onto the bike after landing."),
    Slider(kTabFirstPerson, "SMOOTHING",       "Rotation response",    &Config::rotationResponse, 0.0,   40.0,  0.5,    1.0,    1, " /s", "Off")
        .Help("How fast the view's rotation catches up to the bike. Higher is sharper, lower is "
              "floatier. Works on the ground and in the air."),
    Slider(kTabFirstPerson, nullptr,           "Max lag",              &Config::maxLagDeg,        0.0,   90.0,  0.5,    1.0,    1, "\xB0")
        .Help("The furthest the smoothed view can fall behind the bike. Past this it turns with the "
              "bike instead of trailing further."),
    Slider(kTabFirstPerson, "NECK & IMPACTS",  "Landing compression",  &Config::compressionPerG,  0.0,   0.05,  0.0005, 1000.0, 1, " mm/g", "Off")
        .Help("How much your head sinks on landings and big hits (and lifts in the air)."),
    Slider(kTabFirstPerson, nullptr,           "Max compression",      &Config::maxCompression,   0.0,   0.2,   0.001,  1000.0, 0, " mm")
        .Help("The most your head can sink on a landing."),
    Slider(kTabFirstPerson, nullptr,           "Neck stiffness",       &Config::springHz,         0.5,   10.0,  0.1,    1.0,    1, " Hz")
        .Help("How quickly your head springs back after a hit. Higher is a tighter neck."),
    Slider(kTabFirstPerson, nullptr,           "Neck damping",         &Config::dampingRatio,     0.05,  2.0,   0.05,   1.0,    2, "")
        .Help("How much your head bounces after a hit. Lower is bouncier, 1.00 settles with no bounce."),
    Slider(kTabFirstPerson, nullptr,           "Head nod",             &Config::nodDegPerMeter,   0.0,   150.0, 1.0,    1.0,    0, "\xB0" "/m", "Off")
        .Help("How far your head nods forward as your neck compresses on a landing."),
    Slider(kTabFirstPerson, nullptr,           "Height smoothing",     &Config::verticalResponse, 0.0,   60.0,  1.0,    1.0,    0, " /s", "Off")
        .Help("Smooths small up / down head jitter. Higher follows the bike's height more closely."),
    // ======== GRAPHICS ========
    Toggle(kTabGraphics, "SHADERS",         "Shaders",              &Config::shaders)
        .Help("Turns every effect on this tab on or off (its key is on the KEYBINDS tab). Off is the game "
              "exactly as it looks without the mod.")
        .Col(0),
    Slider(kTabGraphics, "TIME OF DAY",     "Time of day",          &Config::timeOfDay,        -1.0,  24.0,  0.25,   1.0,    0, "", "Game")
        .Clock()
        .Help("Sets the hour: the sun moves across the sky, sunsets turn the light orange, and at "
              "night the sky fills with stars and a moon lights the track. Game keeps the game's "
              "own lighting."),
    Toggle(kTabGraphics, nullptr,           "Move the sun",         &Config::moveSun)
        .Help("The time of day also moves the game's sun, so lighting and shading on bikes and "
              "objects follow it. Off only changes the colours and the sky."),
    Slider(kTabGraphics, nullptr,           "Night brightness",     &Config::nightBrightness,  0.0,   2.0,   0.05,   100.0,  0, "%")
        .Help("How much the moon lights the track at night. Lower is darker (the headlight stands "
              "out more), 0% is pitch black."),
    Slider(kTabGraphics, "HEADLIGHT",       "Headlight",            &Config::headlight,        0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("A headlight on your bike's front plate that lights the track ahead when the time of "
              "day makes it dark. It turns with the bike, not your head."),
    Slider(kTabGraphics, nullptr,           "Beam width",           &Config::headlightBeamDeg, 10.0,  90.0,  1.0,    2.0,    0, "\xB0")
        .Help("How far the headlight's beam spreads to the sides. It always lights the ground from "
              "just in front of the plate out, narrower only makes it skinnier."),
    Slider(kTabGraphics, nullptr,           "Beam aim",             &Config::headlightAimDeg,  -20.0, 15.0,  0.5,    1.0,    1, "\xB0")
        .Help("Where the headlight is aimed. Up shows more of what's ahead (the light reaches further "
              "and lights up jump faces), down keeps it close in front of the bike. 0 is level."),
    Toggle(kTabGraphics, nullptr,           "Every bike's headlight", &Config::headlightEveryBike)
        .Help("A headlight on every bike around you too, not just yours: any model, found by its "
              "wheels once it has ridden a moment. The nearest bikes' are drawn."),
    Slider(kTabGraphics, "GLOW",            "Sun rays",             &Config::sunRays,          0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Light shafts from the sun when it's on screen."),
    Slider(kTabGraphics, nullptr,           "Bloom",                &Config::bloom,            0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("A soft glow around bright parts of the picture: the sky, the sun, white plastics."),
    Toggle(kTabGraphics, nullptr,           "Hide game flare",      &Config::hideGameFlare)
        .Help("With a time of day set, hides the game's own lens flare: it stays where the game's "
              "sun is, so at night it would glare out of the dark sky."),
    Slider(kTabGraphics, "SHADOWS",         "Shadows",              &Config::shadows,          0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Real-time sun shadows from bikes, riders, trees and track objects. Fades out at night "
              "(the moon casts faint ones).")
        .Col(1),
    Toggle(kTabGraphics, nullptr,           "Detailed shadows",     &Config::shadowMap)
        .Help("Draws bikes, riders and objects again from the sun for full, sharp shadows, even from "
              "things off screen. Off uses cheaper screen-space shadows only."),
    Slider(kTabGraphics, nullptr,           "Shadow distance",      &Config::shadowDistance,   5.0,   200.0, 1.0,    1.0,    0, " m")
        .Help("Detailed shadows: how far around you they reach. Shadows near you stay sharp at any "
              "distance; the far ones get softer the further it goes. Further costs some speed."),
    Choice3(kTabGraphics, nullptr,          "Shadow resolution",    &Config::shadowResolution, "2048", "4096", "8192")
        .Help("Detailed shadows: how sharp they are, near and far. Higher is sharper, at the cost of "
              "graphics memory and speed (8192 is for high-end graphics cards)."),
    Slider(kTabGraphics, nullptr,           "Shadow softness",      &Config::shadowSoftness,   0.0,   1.0,   0.01,   100.0,  0, "%", "Crisp")
        .Help("Detailed shadows: how soft their edges are. Lower is crisper; higher smooths the steps a "
              "low sun can leave along shadows on berms and slopes. No cost either way."),
    Slider(kTabGraphics, nullptr,           "Contact shadows",      &Config::shadowLength,     1.0,   20.0,  0.5,    1.0,    1, " m")
        .Help("Screen-space shadows: how far they reach from what casts them. They add shadows from "
              "the ground's own shape (ruts, jumps)."),
    Slider(kTabGraphics, nullptr,           "Ambient occlusion",    &Config::ambientOcclusion, 0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Soft contact shadows in ruts, corners and under the bikes, where light can't reach."),
    Choice3(kTabGraphics, "ANTI-ALIASING",   "Anti-aliasing",        &Config::antiAliasing, "OFF", "FXAA", "HIGH")
        .Help("Smooths jagged edges (FXAA). HIGH follows long edges further for cleaner lines at "
              "a slightly higher cost."),
    Slider(kTabGraphics, nullptr,           "Sharpen",              &Config::sharpen,          0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Brings back fine detail, more where the picture is soft. Pairs well with anti-aliasing."),
    Slider(kTabGraphics, "TEXTURES",        "Anisotropic filter",   &Config::anisotropy,       1.0,   16.0,  1.0,    1.0,    0, "x", "Off")
        .Help("Keeps textures sharp when you look along them at a low angle: the track ahead, "
              "the ruts in the distance. 16x is the sharpest."),
    Toggle(kTabGraphics, nullptr,           "Trilinear filtering",  &Config::trilinear)
        .Help("Blends between texture detail levels, so there are no visible lines where the "
              "ground's texture changes sharpness."),
    Slider(kTabGraphics, nullptr,           "Texture LOD bias",     &Config::textureLodBias,   -2.0,  0.0,   0.05,   1.0,    2, "")
        .Help("Below 0 uses sharper texture detail further away. Too low shimmers in motion; "
              "-0.5 is a good start with 16x anisotropic."),
    Slider(kTabGraphics, "GROUND & TIRES",  "Dirt detail",          &Config::dirtDetail,       0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Adds fine high-resolution grain, pebbles and clods to the track surface up close, "
              "lit by the sun. Only on the ground, never on bikes or riders.")
        .Col(2),
    Slider(kTabGraphics, nullptr,           "Detail size",          &Config::dirtScale,        0.5,   2.0,   0.05,   1.0,    2, "x")
        .Help("How big the dirt detail's grains and pebbles are."),
    Slider(kTabGraphics, nullptr,           "Tire marks",           &Config::tireMarks,        0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Your tires dig ruts in the dirt behind you: packed, darker dirt with the knobs' "
              "imprint and loose clods along the edges. They stay on the track for about 160 m "
              "around you, and come back exactly as you made them whenever you return to a spot "
              "you'd ridden (see the mark cache below for how far back that reaches)."),
    Slider(kTabGraphics, nullptr,           "Mark depth",           &Config::markDepth,        0.0,   1.0,   0.01,   100.0,  0, "%", "Flat")
        .Help("How dug-in the ruts look: the groove's floor is read where your eye actually sees "
              "it through the groove, so at low angles the near lip covers the floor and the far "
              "wall shows, and the floors lose light near the lips. 0 keeps the marks flat on the ground."),
    Slider(kTabGraphics, nullptr,           "Mark cache",           &Config::markCacheSegs,    10000.0, 1000000.0, 25000.0, 1.0, 0, "", nullptr)
        .Help("How far back along your ride the tire marks are remembered. "
              "A cache of 40 000 is ~11 minutes of riding at 60fps"
              "The cache size doesn't effect FPS, it only changes how much memory is used"),
    Slider(kTabGraphics, nullptr,           "Tire dirt",            &Config::tireDirt,         0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Mud caked on your tires, in clumps that turn with the wheels, lit by the sun."),
    Toggle(kTabGraphics, nullptr,           "Marks & dirt on every bike", &Config::marksEveryBike)
        .Help("Tire marks and tire dirt on every bike around you in a lobby or a replay, not just yours: "
              "any model, found by its wheels turning. Their ruts are pressed where the ground under "
              "them is on screen, out to about 100 m."),
    Slider(kTabGraphics, "COLOR",           "Brightness",           &Config::exposure,         -2.0,  2.0,   0.05,   1.0,    2, " EV")
        .Help("Overall exposure. +1 EV is twice as bright, -1 EV half."),
    Slider(kTabGraphics, nullptr,           "Contrast",             &Config::contrast,         -0.5,  0.5,   0.01,   100.0,  0, "%")
        .Help("More (+) or less (-) difference between dark and bright."),
    Slider(kTabGraphics, nullptr,           "Saturation",           &Config::saturation,       -1.0,  1.0,   0.01,   100.0,  0, "%")
        .Help("More (+) or less (-) colourful. -100% is black and white."),
    Slider(kTabGraphics, nullptr,           "Vignette",             &Config::vignette,         0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Darkens the corners of the picture, like a camera lens."),
    Slider(kTabGraphics, "CAMERA EFFECTS",  "Motion blur",          &Config::motionBlur,       0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Blurs the picture with the camera's movement for a sense of speed. Your own bike "
              "stays sharp."),
    Slider(kTabGraphics, nullptr,           "Depth of field",       &Config::depthOfField,     0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Blurs what's out of focus: the bars and the far distance when focused down the "
              "track."),
    Slider(kTabGraphics, nullptr,           "Focus distance",       &Config::dofFocus,         0.0,   100.0, 0.5,    1.0,    1, " m", "Auto")
        .Help("Where depth of field focuses. Auto focuses on the middle of the screen, easing "
              "between distances like an eye."),
    // ======== REPLAY ========
    Choice3(kTabReplay, "REPLAY CAMERA",   "Camera",               &Config::replayCamera, "GAME", "3RD PERSON", "HELMET")
        .Help("GAME is the replay's own camera, which faces one way while the rider swivels in front "
              "of it. 3RD PERSON locks the camera behind the rider. HELMET is a GoPro on the rider's "
              "helmet, on top or on its side.")
        .Col(0),
    Slider(kTabReplay, "3RD PERSON CAMERA", "Distance behind",      &Config::replayDistance,   0.5,   12.0,  0.1,    1.0,    1, " m")
        .Help("3rd person replays: how far behind the rider the camera sits. Lower moves it forward, "
              "closer to the rider."),
    Slider(kTabReplay, nullptr,           "Height",               &Config::replayHeight,     -0.5,  5.0,   0.05,   1.0,    2, " m")
        .Help("3rd person replays: how high above the rider the camera sits."),
    Slider(kTabReplay, nullptr,           "Pitch",                &Config::replayPitchDeg,   -10.0, 45.0,  0.5,    1.0,    1, "\xB0")
        .Help("3rd person replays: how far the camera looks down. Negative looks up."),
    Choice4(kTabReplay, "HELMET CAMERA",   "Mount",                &Config::helmetMount, "TOP", "LEFT", "RIGHT", "CHEST")
        .Help("Helmet replays: TOP mounts the camera on top of the helmet, LEFT and RIGHT on its side, "
              "CHEST on the rider's chest (it turns with the body, not the head). Each keeps its own "
              "height, forward and tilt.")
        .Col(1),
    Slider(kTabReplay, nullptr,           "Mount height",         &Config::helmetHeight,     -1.5,  2.0,   0.01,   1.0,    2, " m")
        .Side(&Config::sideHeight).Chest(&Config::chestHeight)
        .Help("Helmet replays: how far above the rider's eyes the camera sits (chest mount: above the "
              "middle of the shoulders, negative is below them)."),
    Slider(kTabReplay, nullptr,           "Mount forward",        &Config::helmetForward,    -1.5,  1.5,   0.01,   1.0,    2, " m")
        .Side(&Config::sideForward).Chest(&Config::chestForward)
        .Help("Helmet replays: moves the camera forward (+) or back (-) along the helmet (chest mount: "
              "how far in front of the shoulders it sits)."),
    Slider(kTabReplay, nullptr,           "Mount out",            &Config::sideOut,          0.0,   1.5,   0.01,   1.0,    2, " m")
        .Help("Side-mounted helmet replays: how far out to the side of the head the camera sits. Raise "
              "it if the camera ends up inside the helmet."),
    Slider(kTabReplay, nullptr,           "Tilt",                 &Config::helmetTiltDeg,    -10.0, 60.0,  0.5,    1.0,    1, "\xB0")
        .Side(&Config::sideTiltDeg).Chest(&Config::chestTiltDeg)
        .Help("Helmet replays: how far the camera is angled down from where the helmet faces. More "
              "shows more of the bars and the bike, less shows more of the track ahead."),
    Slider(kTabReplay, nullptr,           "Stabilization",        &Config::helmetSmoothing,  0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Helmet replays: smooths the camera's rotation, like a GoPro's stabilization. Off shakes "
              "with every jolt of the rider's head."),
    Slider(kTabReplay, nullptr,           "Horizon lock",         &Config::helmetLevel,      0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Helmet replays: how much of the rider's lean is taken out of the view. 0% leans with "
              "the head, 100% keeps the horizon level."),
    // ======== KEYBINDS ========
    KeyRow(kTabKeybinds, "KEYBINDS",       "Open / close this menu",    &Config::menuKey)
        .Help("Opens and closes this menu. Click the key, then press the new one. Esc cancels."),
    KeyRow(kTabKeybinds, nullptr,          "First person mod on / off", &Config::toggleKey)
        .Help("Turns the first-person camera mod on and off in game (the FIRST PERSON tab's First "
              "person mod switch). Click the key, then press the new one. Esc cancels."),
    KeyRow(kTabKeybinds, nullptr,          "Shaders on / off",          &Config::shaderToggleKey)
        .Help("Turns all the shaders on and off in game, for before / after comparisons (the GRAPHICS "
              "tab's Shaders switch). Click the key, then press the new one. Esc cancels."),
    PadRow(kTabKeybinds, "CONTROLLER",     "Look left",                 &Config::lookLeftControl)
        .Help("Hold to look left (LOOK tab: Controller look, Side look angle). Click it, then "
              "press any button, trigger or paddle on your controller, or push a stick the way you want. "
              "Backspace clears it, Esc cancels."),
    PadRow(kTabKeybinds, nullptr,          "Look right",                &Config::lookRightControl)
        .Help("Hold to look right. Click it, then press any button, trigger or paddle on your controller, "
              "or push a stick the way you want. Backspace clears it, Esc cancels."),
    PadRow(kTabKeybinds, nullptr,          "Look back",                 &Config::lookBackControl)
        .Help("Hold to look behind you (it wins over left and right). Click it, then press any button, "
              "trigger or paddle on your controller, or push a stick the way you want. Backspace clears "
              "it, Esc cancels. Paddles: an Xbox Elite controller's paddles work when they aren't mapped "
              "to another button in the Xbox Accessories app; other controllers' paddles work when they "
              "show up as buttons of their own."),
    // ======== OTHER ========
    Toggle(kTabOther, "WHILE RIDING",   "Hide HUD",                  &Config::hideHud)
        .Help("Hides the game's HUD (timing, positions, chat) while you ride, spectate or watch a "
              "replay, for a clean view. Open the game's menu (Esc) and it's all there as usual; "
              "close it and the HUD goes again. Never hidden in the game's menus.")
        .Col(0),
    Toggle(kTabOther, nullptr,          "Hide cursor",               &Config::hideCursor)
        .Help("Hides the game's mouse cursor while you ride or watch a replay. It comes back as soon "
              "as you move the mouse, and is never hidden in the game's menus."),
    Toggle(kTabOther, nullptr,          "Dab indicator",             &Config::dabIndicator)
        .Help("Shows at the bottom right, next to the speedometer, whether your rider is standing or "
              "sitting: a red rider standing on the bike, or a green one sitting with a foot down while "
              "you hold the game's Sit/Dab key. Follows the game's own settings: your Sit/Dab key or "
              "button and Dab When Stopped. Turning it on asks whether the game's Sticky Sit/Dab is on "
              "(turn it off and on again to change your answer)."),
    Toggle(kTabOther, "WINDOW",         "Fullscreen window",         &Config::fullscreenWindow)
        .Help("With the game in windowed mode, makes its window cover the whole screen with no border "
              "(borderless fullscreen): Alt+Tab stays instant. Off puts the window back as it was. "
              "Shared by all the presets."),
    Toggle(kTabOther, "THIS MENU",      "Fade while adjusting",      &Config::fadeMenu)
        .Help("While you drag a GRAPHICS slider (or step it with the arrow keys) the menu fades out, "
              "all but that slider, so you can see the whole picture change."),
    Toggle(kTabSounds, "SOUND MODS",     "125 two-stroke",            &Config::soundPack125)
        .Help("A fuller engine sound for every bike that uses the game's stock 125 two-stroke engine. "
              "The stock sounds repeat one recorded engine cycle over and over; this one plays 16 "
              "cycles in a random order, each firing a little differently, like a real engine. "
              "Heard from the next time the game starts.")
        .Col(1),
    Toggle(kTabSounds, nullptr,          "250 two-stroke",            &Config::soundPack250)
        .Help("A fuller engine sound for every bike that uses the game's stock 250 two-stroke engine "
              "(16 varied cycles instead of one repeated). Heard from the next time the game starts."),
    Toggle(kTabSounds, nullptr,          "250 four-stroke",           &Config::soundPack250f)
        .Help("A fuller engine sound for every bike that uses the game's stock 250 four-stroke engine "
              "(16 varied cycles instead of one repeated). Heard from the next time the game starts."),
    Toggle(kTabSounds, nullptr,          "450 four-stroke",           &Config::soundPack450f)
        .Help("A fuller engine sound for every bike that uses the game's stock 450 four-stroke engine "
              "(16 varied cycles instead of one repeated). Heard from the next time the game starts. "
              "Bikes with their own custom sounds keep them."),
    Toggle(kTabSounds, nullptr,          "450 four-stroke (clear)",   &Config::soundPack450fClear)
        .Help("The same 450 four-stroke sound, less muffled: everything but the rev limiter brighter, the "
              "limiter very slightly more muffled. One 450 sound or the other: turning this on turns the "
              "other off. Heard from the next time the game starts."),
    Slider(kTabSounds, "BRAKES",       "Brake squeal",              &Config::brakeSqueal,      0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("Your brakes squeal when you brake hard: the game's own brake controls pressed most of "
              "the way while you're moving and slowing down. Its loudness; heard right away, with "
              "any bike."),

    // ======== WEATHER ========
    Toggle(kTabWeather, "RAIN",           "Rain",                      &Config::rain)
        .Help("Rain is coming in a future update: wet, darker dirt, puddles and rain falling around you.")
        .Badge("COMING SOON")
        .Locked(),
    Toggle(kTabWeather, "SNOW",           "Snow",                      &Config::snow)
        .Help("Snow on the track: the ground turns white (thinner on steep slopes, your ruts dig "
              "through it to the dirt) under a grey, overcast sky. Needs the shaders on (GRAPHICS)."),
    Slider(kTabWeather, nullptr,          "Snowfall",                  &Config::snowfall,         0.0,   1.0,   0.01,   100.0,  0, "%", "Off")
        .Help("How heavily it snows: flakes falling all around you (they fly past as you ride), and "
              "the haze they make in the distance. Off keeps the snow on the ground and nothing falling."),
    Slider(kTabWeather, nullptr,          "Wind",                      &Config::snowWind,         0.0,   1.0,   0.01,   100.0,  0, "%", "Calm")
        .Help("How hard the wind blows the falling snow sideways."),
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);

// Keyboard focus order: the tab bar, the on/off switch, the current tab's rows, then the buttons.
constexpr int kTabBarItem = 0;
constexpr int kFirstRowItem = 1;

struct PanelState {
    bool   open         = false;
    int    tab          = 0;
    int    selected     = kTabBarItem;  // (the tabs: their marker shows from the first time the menu opens)
    int    dragging     = -1;  // index into kRows of the slider being dragged with the mouse
    int    pressedItem  = -1;  // button the mouse went down on
    int    listening    = -1;  // index into kRows of the key row waiting for a key press
    double listenEnded  = -1;  // when it last stopped waiting (its key may still be held)
    bool   helpForMouse = false;  // the help box follows the mouse (true) or the selection
    float  mouseX = -1, mouseY = -1;
    bool   dirty        = false;
    bool   closeRequest = false;
    char   status[96]   = "";
    double statusUntil  = 0;
    // Fading while a GRAPHICS slider is adjusted (Config::fadeMenu): all but that slider.
    float  opacity      = 1.0f;
    double lastFrame    = 0;
    int    adjustRow    = -1;   // index into kRows of the slider last adjusted
    double adjustedAt   = -1;   // when (by the keys; dragging counts for as long as it lasts)
    // The dab indicator was just turned on: asking whether the game's Sticky Sit/Dab is on (the
    // game's settings file can't be trusted for it: see Config::dabSticky). 0 OFF, 1 ON highlighted.
    bool   askSticky    = false;
    int    askChoice    = 0;
    // DEFAULTS was pressed: asking first (0 CANCEL, 1 RESET highlighted). Too many lost their
    // settings to one click.
    bool   askDefaults  = false;
    double openedAt     = 0;  // when the menu last opened (the logo's light sweep counts from it)
    // Something just changed that only takes effect once the game restarts (the sound mods): saying
    // so, with an OK. restartText: what changed.
    bool   askRestart   = false;
    char   restartText[128] = "";
    // A preset being renamed (its index, -1: none): what's typed so far, and when typing started
    // (the caret blinks from then).
    int    renaming     = -1;
    std::string nameDraft;
    double renameStarted = 0;
    // The search box: what's typed (query: the same, trimmed), whether keys type into it, and when
    // typing started (the caret blinks from then).
    std::string search, query;
    bool   searchFocus  = false;
    double searchStarted = 0;
    // The settings list's scroll (pixels at the current scale), eased toward scrollTarget; the
    // scrollbar's thumb while it's dragged; and what the list showed (a new tab or search: back to the top).
    float  scroll       = 0;
    float  scrollTarget = 0;
    bool   scrollDrag   = false;
    float  scrollGrab   = 0;
    double scrollTime   = 0;
    int    listTab      = -1;
    std::string listQuery;
    // The red bar behind the current tab, sliding to a tab when it's chosen (in tabs, from the top).
    float  tabBar       = -1;
    double tabBarTime   = 0;
};
PanelState g_panel;

// Parts of the batch the fade leaves alone (the slider being adjusted, the cursor): [begin, end).
struct KeepRange { size_t begin, end; };
std::vector<KeepRange> g_keepRanges;
constexpr float  kFadedOpacity = 0.06f;
constexpr double kAdjustHold   = 0.6;  // s the menu stays faded after the last arrow key step

// ---- The game's HUD and cursor while riding (Config::hideHud / hideCursor) -----------------
// The cursor: activity is the mouse moving or a button / Esc going down; it shows it for a few
// seconds. The HUD: hidden all the while you ride, shown with the game's menu (GameMenuOverScene),
// which Esc opens: Esc presses to the game are counted for it (TakeGameEscapes).
constexpr ULONGLONG kRevealMs = 3000;
std::atomic<ULONGLONG> g_lastMouseActivity{ 0 };
std::atomic<int>       g_gameEscapes{ 0 };       // Esc presses that reached the game
std::atomic<bool>      g_keyMessages{ false };   // the game gets its keys as window messages
std::atomic<long long> g_lastMousePos{ 0 };   // (x << 32) | y, screen pixels
std::atomic<HWND>      g_gameWindow{ nullptr };

// ---- Sound mods (Config::soundPack*) ----------------------------------------------------------
// A switch turned in the panel installs or removes its pack right away (see sound_packs.h).
void SetStatus(const char* text, double now);

void UpdateSoundPacks(double now)
{
    Config& cfg = EditConfig();
    bool* const switches[soundpacks::kCount] = { &cfg.soundPack125, &cfg.soundPack250, &cfg.soundPack250f, &cfg.soundPack450f,
                                                 &cfg.soundPack450fClear };
    // The two 450 packs replace the same stock engine: turning one on turns the other off (removed
    // first below, then the new one installed: its line is the one shown).
    if (cfg.soundPack450f && cfg.soundPack450fClear) {
        if (soundpacks::Installed(3))
            cfg.soundPack450f = false;       // (the clear one was just turned on)
        else
            cfg.soundPack450fClear = false;  // (the plain one was)
    }
    for (int i = 0; i < soundpacks::kCount; ++i) {
        if (*switches[i] == soundpacks::Installed(i))
            continue;
        const char* status = soundpacks::Set(i, *switches[i]);
        SetStatus(status, now);
        // (Installed or taken out: heard only once the game starts again. Not when it failed.)
        if (g_panel.open && (strstr(status, "RESTART") || strstr(status, "NEXT TIME") || strstr(status, "CLOSE THE GAME"))) {
            snprintf(g_panel.restartText, sizeof g_panel.restartText, "%s", status);
            g_panel.askRestart = true;
            g_panel.askChoice = 0;
        }
        *switches[i] = soundpacks::Installed(i);  // (back off if it couldn't be installed)
    }
}

#ifdef _WIN32
// ---- Fullscreen window (Config::fullscreenWindow) --------------------------------------------
// The game's window in windowed mode made borderless and stretched over its monitor; what it was
// (style, extended style, placement) is kept to put it back when the option is turned off. A
// window already bare and covering its monitor (the game's own fullscreen) is left alone.
struct FullscreenWindow {
    HWND            window = nullptr;   // the window made fullscreen (null: none)
    LONG_PTR        style = 0, exStyle = 0;
    WINDOWPLACEMENT placement{};
    ULONGLONG       lastTry = 0;
};
FullscreenWindow g_fullscreen;

constexpr LONG_PTR kFrameStyles   = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
constexpr LONG_PTR kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

void UpdateFullscreenWindow(HWND window, bool wanted)
{
    FullscreenWindow& f = g_fullscreen;
    if (!window || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD))
        return;  // drawn into a window inside another: not one to stretch
    if (f.window && (f.window != window || !IsWindow(f.window)))
        f.window = nullptr;  // the game made a new window: the old one's state is gone with it
    if (!wanted) {
        if (!f.window)
            return;
        // Back as it was: its frame, then its place and size.
        SetWindowLongPtrW(window, GWL_STYLE, f.style);
        SetWindowLongPtrW(window, GWL_EXSTYLE, f.exStyle);
        SetWindowPlacement(window, &f.placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
        f.window = nullptr;
        Log("window: back to a normal window");
        return;
    }
    if (IsIconic(window))
        return;  // minimised: nothing to fill
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    RECT rect;
    if (!GetWindowRect(window, &rect))
        return;
    // One pixel taller than the screen (the extra row is off its bottom edge): a window exactly the
    // size of the screen is taken for real fullscreen and no longer synced to the screen by Windows,
    // which tears the picture into bands whenever the game runs without vsync. The taskbar still
    // goes behind it.
    const RECT& m = monitor.rcMonitor;
    const RECT target = { m.left, m.top, m.right, m.bottom + 1 };
    const bool bare = !(style & (WS_CAPTION | WS_THICKFRAME));
    if (bare && (EqualRect(&rect, &target) || EqualRect(&rect, &m)))
        return;  // already fullscreen (ours, or the game's own)
    // At most once a second, so a window that won't take it isn't fought every frame.
    const ULONGLONG now = GetTickCount64();
    if (now - f.lastTry < 1000)
        return;
    f.lastTry = now;
    if (!f.window) {
        f.window = window;
        f.style = style;
        f.exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        f.placement.length = sizeof f.placement;
        GetWindowPlacement(window, &f.placement);
        Log("window: fullscreen window (%ldx%ld, borderless, one pixel taller than the screen so it stays in sync with it)",
            monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    }
    if (IsZoomed(window))
        ShowWindow(window, SW_RESTORE);  // a maximised window keeps its own size
    SetWindowLongPtrW(window, GWL_STYLE, style & ~kFrameStyles);
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) & ~kFrameExStyles);
    SetWindowPos(window, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
}

#else
void UpdateFullscreenWindow(HWND, bool) {}  // (the window manager's own fullscreen)
#endif

void NoteMouseActivity()
{
    // For automated tests: the real mouse (whoever is at the desk) doesn't count.
    static const bool testIdle = GetEnvironmentVariableA("MXCAM_TEST_MOUSE_IDLE", nullptr, 0) > 0;
    if (testIdle)
        return;
    POINT p;
    if (!GetCursorPos(&p))
        return;
    const long long packed = (static_cast<long long>(p.x) << 32) | static_cast<unsigned int>(p.y);
    const bool moved = g_lastMousePos.exchange(packed) != packed;
    const bool pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                         (GetAsyncKeyState(VK_MBUTTON) & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
    if (moved || pressed)
        g_lastMouseActivity = GetTickCount64();
}

struct Input {
    float mx = -1, my = -1;
    bool  inside = false, down = false, pressed = false, released = false;
    int   nav = 0;   // -1 up, +1 down
    int   step = 0;  // -1 left, +1 right
    bool  activate = false, coarse = false;
    bool  rename = false;  // F2: rename the preset in use (with the presets focused)
    float wheel = 0;       // mouse wheel notches since the last frame (+ away from you: up the list)

    bool Inside(float x, float y, float w, float h) const
    {
        return inside && mx >= x && mx < x + w && my >= y && my < y + h;
    }
};

struct KeyState {
    bool   down = false;
    double nextRepeat = 0;
};
struct Keys {
    KeyState menu, escape, up, downArrow, left, right, enter, space, mouse, f2;
};
Keys g_keys;

// Edge-triggered key poll; with repeat, a held key fires again after 0.35 s, then every 45 ms.
bool PollKey(KeyState& key, int vk, bool repeat, double now)
{
    const bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fire = false;
    if (down && !key.down) {
        fire = true;
        key.nextRepeat = now + 0.35;
    } else if (down && repeat && now >= key.nextRepeat) {
        fire = true;
        key.nextRepeat = now + 0.045;
    }
    key.down = down;
    return fire;
}

double Seconds()
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

std::atomic<bool> g_menuOpen{ false };  // read by the window procedure (the window's thread)
std::atomic<int>  g_wheelDelta{ 0 };    // the mouse wheel while the menu is open (WHEEL_DELTA a notch)
#ifdef _WIN32
HWND    g_hookedWindow   = nullptr;
WNDPROC g_previousProc   = nullptr;
bool    g_windowUnicode  = true;
#endif

#ifdef _WIN32
LRESULT CALLBACK PanelWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Esc going down for the game (not held, not to this panel, which keeps presses to itself): the
    // game's menu opens or closes (Config::hideHud shows the HUD with it).
    if (message == WM_KEYDOWN) {
        g_keyMessages = true;
        if (wParam == VK_ESCAPE && !(lParam & (1 << 30)) && !g_menuOpen.load(std::memory_order_relaxed))
            ++g_gameEscapes;
    }
    // The game's cursor while riding (Config::hideCursor), if it's the system's: none over the
    // game while the mouse is left alone. A move sends this first, and shows the game's again.
    if (message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && !g_menuOpen.load(std::memory_order_relaxed)) {
        NoteMouseActivity();
        if (HideGameOverlayNow(true)) {
            SetCursor(nullptr);
            return TRUE;
        }
    }
    if (g_menuOpen.load(std::memory_order_relaxed)) {
        if (message == WM_MOUSEWHEEL)
            g_wheelDelta += GET_WHEEL_DELTA_WPARAM(wParam);
        switch (message) {
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(nullptr);  // the panel draws its own cursor
                return TRUE;
            }
            break;
        // Presses go to the panel only. Releases (WM_KEYUP, WM_*BUTTONUP) still reach the game so
        // nothing it saw go down can get stuck; Alt combinations (WM_SYSKEYDOWN) pass too.
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_KEYDOWN:
        case WM_CHAR:
            return 0;
        default:
            break;
        }
    }
    return g_windowUnicode ? CallWindowProcW(g_previousProc, window, message, wParam, lParam)
                           : CallWindowProcA(g_previousProc, window, message, wParam, lParam);
}

void SubclassWindow(HWND window)
{
    if (window == g_hookedWindow)
        return;
    // A recreated window (e.g. a fullscreen switch): give the old one its procedure back.
    if (g_hookedWindow && IsWindow(g_hookedWindow) &&
        GetWindowLongPtrW(g_hookedWindow, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&PanelWindowProc)) {
        SetWindowLongPtrW(g_hookedWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_previousProc));
    }
    g_windowUnicode = IsWindowUnicode(window) != FALSE;
    // Read the old procedure first so a message arriving mid-swap can always be forwarded.
    g_previousProc = reinterpret_cast<WNDPROC>(g_windowUnicode ? GetWindowLongPtrW(window, GWLP_WNDPROC)
                                                               : GetWindowLongPtrA(window, GWLP_WNDPROC));
    if (g_windowUnicode)
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PanelWindowProc));
    else
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PanelWindowProc));
    g_hookedWindow = window;
}

#else
// (Linux: the game's X11 events pass linux/input_linux.cpp, which asks the panel with
// OverlayTakesInput and gives it the wheel with OverlayWheel.)
void SubclassWindow(HWND) {}
#endif

void OpenPanel(HWND window)
{
    SubclassWindow(window);
    g_panel.open = true;
    g_panel.dragging = -1;
    g_panel.pressedItem = -1;
    g_panel.listening = -1;
    g_panel.closeRequest = false;
    g_panel.openedAt = Seconds();
    g_menuOpen = true;
    Log("panel: opened");
}

void FinishRename(bool keep, double now);
void ClearSearch();

void ClosePanel()
{
    if (g_panel.renaming >= 0)
        FinishRename(true, Seconds());
    ClearSearch();
    g_panel.scrollDrag = false;
    g_panel.askDefaults = false;
    g_panel.askRestart = false;
    g_panel.open = false;
    g_panel.dragging = -1;
    g_panel.listening = -1;
    g_menuOpen = false;
    Log("panel: closed%s", g_panel.dirty ? " (unsaved changes kept for this session)" : "");
}

Input ReadInput(HWND window, bool focus, int width, int height, double now)
{
    Input in;
    const bool up    = PollKey(g_keys.up, VK_UP, true, now);
    const bool down  = PollKey(g_keys.downArrow, VK_DOWN, true, now);
    const bool left  = PollKey(g_keys.left, VK_LEFT, true, now);
    const bool right = PollKey(g_keys.right, VK_RIGHT, true, now);
    const bool enter = PollKey(g_keys.enter, VK_RETURN, false, now);
    const bool space = PollKey(g_keys.space, VK_SPACE, false, now);
    const bool f2    = PollKey(g_keys.f2, VK_F2, false, now);
    const bool wasDown = g_keys.mouse.down;
    PollKey(g_keys.mouse, VK_LBUTTON, false, now);
    if (!focus) {
        g_wheelDelta = 0;
        return in;
    }

    POINT cursor;
    if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor)) {
        in.mx = static_cast<float>(cursor.x);
        in.my = static_cast<float>(cursor.y);
        in.inside = cursor.x >= 0 && cursor.y >= 0 && cursor.x < width && cursor.y < height;
    }
    in.down     = g_keys.mouse.down;
    in.pressed  = in.down && !wasDown && in.inside;
    in.released = !in.down && wasDown;
    in.nav      = (down ? 1 : 0) - (up ? 1 : 0);
    in.step     = (right ? 1 : 0) - (left ? 1 : 0);
    in.activate = enter || space;
    in.rename   = f2;
    in.wheel    = static_cast<float>(g_wheelDelta.exchange(0)) / WHEEL_DELTA;
    in.coarse   = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return in;
}

const char* KeyName(int vk)
{
    static char name[32];
    const UINT scanCode = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (!scanCode || GetKeyNameTextA(static_cast<LONG>(scanCode << 16), name, sizeof name) <= 0)
        snprintf(name, sizeof name, "KEY %02X", vk);
    for (char* c = name; *c; ++c)
        *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    return name;
}

void SetStatus(const char* text, double now)
{
    snprintf(g_panel.status, sizeof g_panel.status, "%s", text);
    g_panel.statusUntil = now + 2.5;
}

// The first keyboard key to go down since the last call, or 0. Mouse buttons don't count.
int PollNewKey()
{
    static bool wasDown[256];
    int pressed = 0;
    for (int vk = VK_BACK; vk < 0xFF; ++vk) {
        if (vk >= VK_LSHIFT && vk <= VK_RMENU)  // reported as plain Shift / Ctrl / Alt as well
            continue;
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !wasDown[vk] && !pressed)
            pressed = vk;
        wasDown[vk] = down;
    }
    return pressed;
}

// ---- Typing: a preset's name, the search -------------------------------------------------------
// Typed straight from the keyboard (the game's window may never be sent key messages), each key
// made its character by the keyboard layout, with Shift, Caps Lock and AltGr. Renaming: Enter keeps
// the name, Esc (OverlayFrame) leaves it as it was, and clicking anywhere else keeps it. Search:
// Enter stops typing (the results stay), Esc clears it.
constexpr int kSearchMax = 32;
struct Typing {
    bool   down[256] = {};
    double repeatAt[256] = {};
};
Typing g_typing;

bool TypingKey(int vk)
{
    return vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_CAPITAL && !(vk >= VK_LSHIFT && vk <= VK_RMENU);
}

void PrimeTyping()
{
    for (int vk = VK_BACK; vk < 0xFF; ++vk)  // keys already held (the F2 that got here) aren't typed
        g_typing.down[vk] = TypingKey(vk) && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void StartRename(int preset, double now)
{
    g_panel.searchFocus = false;
    g_panel.renaming = preset;
    g_panel.nameDraft = PresetName(preset);
    g_panel.renameStarted = now;
    g_panel.listening = -1;
    PrimeTyping();
    Log("panel: renaming preset %d", preset + 1);
}

void FinishRename(bool keep, double now)
{
    const int preset = g_panel.renaming;
    g_panel.renaming = -1;
    if (preset < 0 || !keep || g_panel.nameDraft == PresetName(preset))
        return;
    SetPresetName(preset, g_panel.nameDraft);
    g_panel.dirty = true;
    char text[96];
    snprintf(text, sizeof text, "PRESET %d IS NOW \"%s\" - SAVE TO KEEP IT", preset + 1, PresetLabel(preset).c_str());
    SetStatus(text, now);
}

void UpdateQuery()
{
    const std::string& s = g_panel.search;
    const size_t a = s.find_first_not_of(' ');
    g_panel.query = a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(' ') - a + 1);
}

void StartSearch(double now)
{
    if (g_panel.renaming >= 0)
        FinishRename(true, now);
    g_panel.searchFocus = true;
    g_panel.searchStarted = now;
    g_panel.listening = -1;
    PrimeTyping();
}

void ClearSearch()
{
    g_panel.search.clear();
    g_panel.searchFocus = false;
    UpdateQuery();
}

// The keys typed since the last call, into text (printable ASCII, at most maxLength characters):
// true if Enter was pressed.
bool TypeInto(std::string& text, int maxLength, double now)
{
    bool entered = false;
    BYTE state[256] = {};
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        state[VK_SHIFT] = 0x80;
    if (ctrl)
        state[VK_CONTROL] = 0x80;
    if (alt)
        state[VK_MENU] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1)
        state[VK_CAPITAL] = 0x01;
    for (int vk = VK_BACK; vk < 0xFF; ++vk) {
        if (!TypingKey(vk))
            continue;
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool fire = false;
        if (down && !g_typing.down[vk]) {
            fire = true;
            g_typing.repeatAt[vk] = now + 0.4;
        } else if (down && now >= g_typing.repeatAt[vk]) {
            fire = true;
            g_typing.repeatAt[vk] = now + 0.05;
        }
        g_typing.down[vk] = down;
        if (!fire || entered || vk == VK_ESCAPE)
            continue;
        std::string& draft = text;
        if (vk == VK_RETURN) {
            entered = true;
            continue;
        }
        if (vk == VK_BACK) {
            if (ctrl)
                draft.clear();
            else if (!draft.empty())
                draft.pop_back();
            continue;
        }
        if (vk == VK_DELETE) {
            draft.clear();
            continue;
        }
        if (ctrl && !alt)
            continue;  // (Ctrl + a letter isn't one; Ctrl + Alt is AltGr)
        wchar_t out[4];
        const int n = ToUnicode(static_cast<UINT>(vk), MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC), state, out, 4,
                                0x4 /* leave the keyboard's dead-key state alone */);
        if (n == 1 && out[0] >= 32 && out[0] <= 126 && static_cast<int>(draft.size()) < maxLength)
            draft += static_cast<char>(out[0]);
    }
    return entered;
}

// Is row i in the list now: the current tab's rows, or with a search typed, every row on any tab
// whose name, section or tab has it (any case).
bool Contains(const char* text, const std::string& query)
{
    if (!text || query.empty())
        return false;
    const size_t n = strlen(text), m = query.size();
    for (size_t i = 0; i + m <= n; ++i) {
        size_t k = 0;
        while (k < m && tolower(static_cast<unsigned char>(text[i + k])) == tolower(static_cast<unsigned char>(query[k])))
            ++k;
        if (k == m)
            return true;
    }
    return false;
}

// A row's section: its own, or the last one above it on its tab.
const char* RowSection(int i)
{
    for (int r = i; r >= 0 && kRows[r].tab == kRows[i].tab; --r)
        if (kRows[r].section)
            return kRows[r].section;
    return "";
}

bool RowShown(int i)
{
    if (i < 0 || i >= kRowCount)
        return false;
    const RowDef& def = kRows[i];
    if (g_panel.query.empty())
        return def.tab == g_panel.tab;
    return Contains(def.label, g_panel.query) || Contains(RowSection(i), g_panel.query) || Contains(kTabs[def.tab], g_panel.query);
}

void StartListening(int row)
{
    g_panel.listening = row;
    PollNewKey();  // keys already held (the Enter that got here) don't count as the answer
    if (kRows[row].kind == RowKind::Pad)
        gamepad::StartCapture();  // nor do controls already held
}


void Upper(const char* text, char* out, size_t size)
{
    size_t i = 0;
    for (; text[i] && i + 1 < size; ++i)
        out[i] = static_cast<char>(toupper(static_cast<unsigned char>(text[i])));
    out[i] = '\0';
}

// Keys the panel steers with, or that another hotkey already has, can't be taken.
const char* KeyConflict(int vk, int Config::* field, const Config& cfg)
{
    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_RETURN: case VK_SPACE: case VK_SHIFT:
        return "USED BY THE MENU";
    default:
        break;
    }
    const struct { int Config::* key; const char* what; } bound[] = {
        { &Config::menuKey,         "THE MENU KEY" },
        { &Config::toggleKey,       "THE FIRST PERSON MOD KEY" },
        { &Config::shaderToggleKey, "THE SHADERS KEY" },
        { &Config::reloadKey,       "THE RELOAD KEY (MXCAM.INI)" },
        { &Config::dumpKey,         "THE FRAME DUMP KEY (MXCAM.INI)" },
        { &Config::traceKey,        "THE TRACE KEY (MXCAM.INI)" },
    };
    for (const auto& b : bound)
        if (b.key != field && cfg.*b.key == vk)
            return b.what;
    return nullptr;
}

// A controller row is waiting: a control pressed on the controller is taken (unless another look
// has it); on the keyboard, Backspace / Delete clears the bind and Esc cancels.
void ListenForPad(double now)
{
    const RowDef& def = kRows[g_panel.listening];
    const int vk = PollNewKey();
    gamepad::Control control = gamepad::PollCapture();
    if (!vk && control == gamepad::kNone)
        return;
    if (vk && vk != VK_BACK && vk != VK_DELETE && vk != VK_ESCAPE && control == gamepad::kNone)
        return;  // other keys: still waiting for the controller
    g_panel.listening = -1;
    g_panel.listenEnded = now;
    if (vk == VK_ESCAPE)
        return;
    if (vk == VK_BACK || vk == VK_DELETE)
        control = gamepad::kNone;

    Config& cfg = EditConfig();
    char text[128], label[64];
    Upper(def.label, label, sizeof label);
    const struct { int Config::* field; const char* what; } looks[] = {
        { &Config::lookLeftControl,  "LOOK LEFT" },
        { &Config::lookRightControl, "LOOK RIGHT" },
        { &Config::lookBackControl,  "LOOK BACK" },
    };
    for (const auto& l : looks) {
        if (control != gamepad::kNone && l.field != def.pad && cfg.*l.field == control) {
            snprintf(text, sizeof text, "%s IS ALREADY %s", gamepad::ControlName(control), l.what);
            SetStatus(text, now);
            return;
        }
    }
    int& bound = cfg.*def.pad;
    if (bound != control) {
        bound = control;
        g_panel.dirty = true;
    }
    snprintf(text, sizeof text, "%s: %s - NOT SAVED YET", label, gamepad::ControlName(control));
    SetStatus(text, now);
}

// A key row is waiting: Esc cancels, any other key is taken (unless it clashes).
void ListenForKey(double now)
{
    if (!RowShown(g_panel.listening)) {  // switched tabs (or searched) with the mouse
        g_panel.listening = -1;
        return;
    }
    if (kRows[g_panel.listening].kind == RowKind::Pad) {
        ListenForPad(now);
        return;
    }
    const int vk = PollNewKey();
    if (!vk)
        return;
    const RowDef& def = kRows[g_panel.listening];
    g_panel.listening = -1;
    g_panel.listenEnded = now;
    if (vk == VK_ESCAPE)
        return;

    Config& cfg = EditConfig();
    char text[128], label[64];
    Upper(def.label, label, sizeof label);
    if (const char* conflict = KeyConflict(vk, def.key, cfg)) {
        snprintf(text, sizeof text, "%s IS ALREADY %s", KeyName(vk), conflict);
        SetStatus(text, now);
        return;
    }
    int& key = cfg.*def.key;
    if (key != vk) {
        key = vk;
        g_panel.dirty = true;
    }
    g_keys.menu.down = true;  // the press that picked the key must not also close the panel
    snprintf(text, sizeof text, "%s: %s - NOT SAVED YET", label, KeyName(vk));
    SetStatus(text, now);
}

// ===========================================================================
// The panel (immediate mode: layout, input and drawing in one pass)
// ===========================================================================
void FormatValue(const RowDef& def, double value, char* out, size_t size)
{
    if (def.number == &Config::markCacheSegs) {
        // The mark cache: the count it keeps, and the memory it costs (both wheels, kMarkSegBytes
        // a segment), so the size can be weighed before it's set.
        // (With every bike's marks on, theirs as well: a cache of their own, four times one of yours.)
        const double bytes = value * (GetConfig().marksEveryBike ? 6.0 : 2.0) * static_cast<double>(kMarkSegBytes);
        snprintf(out, size, "%.0f segs: %.1f MB", value, bytes / (1024.0 * 1024.0));
        return;
    }
    if (def.zeroText && value <= def.min + def.step * 0.5) {
        snprintf(out, size, "%s", def.zeroText);
    } else if (def.clock) {
        const int minutes = static_cast<int>(std::lround(value * 60.0)) % (24 * 60);
        snprintf(out, size, "%02d:%02d", minutes / 60, minutes % 60);
    } else if (def.min < 0)
        snprintf(out, size, "%+.*f%s", def.decimals, value * def.display, def.unit);
    else
        snprintf(out, size, "%.*f%s", def.decimals, value * def.display, def.unit);
}

void SetSlider(const RowDef& def, double& value, double wanted)
{
    const double snapped = def.min + std::round((wanted - def.min) / def.step) * def.step;
    const double clamped = Clamp(snapped, def.min, def.max);
    if (clamped != value) {
        value = clamped;
        g_panel.dirty = true;
    }
}

void SetFlag(bool& flag, bool wanted)
{
    if (flag != wanted) {
        flag = wanted;
        g_panel.dirty = true;
    }
}

void SetOption(int& option, int wanted)
{
    if (option != wanted) {
        option = wanted;
        g_panel.dirty = true;
    }
}

// Rows that currently have no effect are drawn dimmed (they can still be adjusted).
bool RowInactive(const RowDef& def, const Config& cfg)
{
    if (def.locked || (def.customOnly && !cfg.customCamera))
        return true;
    // (Controller look and its angle and speed work with the mod off too.)
    const bool lookRow = def.flag == &Config::controllerLook || def.number == &Config::lookSideDeg ||
                         def.number == &Config::lookTurnSpeed;
    if ((def.tab == kTabFirstPerson || def.tab == kTabLook || def.tab == kTabReplay) && !cfg.enabled &&
        def.flag != &Config::enabled && !lookRow)
        return true;  // the first person mod is off (it does the replay cameras too)
    if (def.tab == kTabReplay && def.number) {  // placement for a replay camera that isn't the one chosen
        const bool chase = def.number == &Config::replayDistance || def.number == &Config::replayHeight ||
                           def.number == &Config::replayPitchDeg;
        if (def.number == &Config::sideOut && cfg.helmetMount != 1 && cfg.helmetMount != 2)
            return true;
        return cfg.replayCamera != (chase ? 1 : 2);
    }
    if (def.option == &Config::helmetMount)
        return cfg.replayCamera != 2;
    if (def.tab == kTabGraphics) {  // the shaders' rows: off with the master switch, or what they refine
        if (def.flag == &Config::shaders)
            return false;
        if (!cfg.shaders)
            return true;
        if (def.flag == &Config::moveSun || def.flag == &Config::hideGameFlare || def.number == &Config::nightBrightness)
            return cfg.timeOfDay < 0;
        if (def.number == &Config::shadowLength)  // (screen-space: only without the shadow map)
            return cfg.shadows <= 0 || cfg.shadowMap;
        if (def.flag == &Config::shadowMap)
            return cfg.shadows <= 0;
        if (def.number == &Config::shadowDistance || def.option == &Config::shadowResolution)
            return cfg.shadows <= 0 || !cfg.shadowMap;
        if (def.number == &Config::headlight)
            return cfg.timeOfDay < 0;
        if (def.number == &Config::headlightBeamDeg || def.number == &Config::headlightAimDeg ||
            def.flag == &Config::headlightEveryBike)
            return cfg.timeOfDay < 0 || cfg.headlight <= 0;
        if (def.number == &Config::snowfall)
            return !cfg.snow;
        if (def.number == &Config::snowWind)
            return !cfg.snow || cfg.snowfall <= 0.0;
        if (def.number == &Config::dirtScale)
            return cfg.dirtDetail <= 0;
        if (def.number == &Config::markCacheSegs)
            return cfg.tireMarks <= 0;
        if (def.number == &Config::dofFocus)
            return cfg.depthOfField <= 0;
    }
    return false;
}

// The panel's size at scale 1 (pixels at 1080p); the window's scale fits it, with a margin.
constexpr float kPanelW = 1400.0f, kPanelH = 800.0f;

float PanelHeightAtScale1() { return kPanelH + 2.0f * 16.0f; }

#ifdef _WIN32
// The Windows clipboard, for preset codes (shared as text: Discord and the like).
bool CopyToClipboard(const std::string& text)
{
    HWND owner = g_hookedWindow ? g_hookedWindow : g_gameWindow.load();
    if (!OpenClipboard(owner))
        return false;
    bool ok = false;
    EmptyClipboard();
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (HGLOBAL memory = wide > 0 ? GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wide) * sizeof(wchar_t)) : nullptr) {
        if (wchar_t* to = static_cast<wchar_t*>(GlobalLock(memory))) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, to, wide);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok)
            GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

std::string ClipboardText()
{
    std::string text;
    HWND owner = g_hookedWindow ? g_hookedWindow : g_gameWindow.load();
    if (!OpenClipboard(owner))
        return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* from = static_cast<const wchar_t*>(GlobalLock(data))) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, from, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1 && bytes < (1 << 20)) {
                text.resize(static_cast<size_t>(bytes));
                WideCharToMultiByte(CP_UTF8, 0, from, -1, &text[0], bytes, nullptr, nullptr);
                text.resize(static_cast<size_t>(bytes) - 1);
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

#else
// The clipboard (X11 or Wayland), for preset codes.
bool CopyToClipboard(const std::string& text) { return platform::ClipboardSet(text); }
std::string ClipboardText() { return platform::ClipboardGet(); }
#endif

// Where the panel is on screen (at the atlas's scale): its left, top, width and height.
struct PanelPlace { float x = 0, y = 0, w = 0, h = 0; };
PanelPlace PanelOnScreen(int screenHeight)
{
    const float s = g_atlasScale;
    const auto S = [s](float v) { return std::round(v * s); };
    PanelPlace p;
    p.w = S(kPanelW);
    p.h = S(kPanelH);
    p.x = S(32);
    p.y = std::fmax(S(16), std::round((screenHeight - p.h) * 0.5f));
    return p;
}

void BuildPanel(int screenWidth, int screenHeight, const Input& in, double now)
{
    Config& cfg = EditConfig();
    const float s = g_atlasScale;
    const auto S = [s](float v) { return std::round(v * s); };
    const float thin = std::fmax(1.0f, S(1)), stroke = std::fmax(1.5f, S(2));
    const bool searching = !g_panel.query.empty();

    int rowsShown = 0;
    for (int i = 0; i < kRowCount; ++i)
        rowsShown += RowShown(i) ? 1 : 0;
    const int presetItem = kFirstRowItem + rowsShown;
    const int saveItem = presetItem + 1, defaultsItem = saveItem + 1, closeItem = saveItem + 2;
    const int copyCodeItem = closeItem + 1, pasteCodeItem = closeItem + 2;
    const int cancelItem = pasteCodeItem + 1;  // (only while there are unsaved changes)
    const int itemCount = cancelItem + 1;
    if (g_panel.selected >= itemCount)
        g_panel.selected = itemCount - 1;
    if (in.nav)
        g_panel.selected = (g_panel.selected + in.nav + itemCount) % itemCount;
    if (!g_panel.dirty && g_panel.selected == cancelItem)
        g_panel.selected = in.nav < 0 ? cancelItem - 1 : 0;  // (not shown: stepped over)
    const bool reveal = in.nav != 0;  // moved with the keys: the row it's on scrolls into view

    // Help follows whichever was used last: the mouse (what's under it) or the keys (the selection).
    if (in.inside && (in.mx != g_panel.mouseX || in.my != g_panel.mouseY || in.pressed))
        g_panel.helpForMouse = true;
    if (in.nav || in.step || in.activate)
        g_panel.helpForMouse = false;
    g_panel.mouseX = in.mx;
    g_panel.mouseY = in.my;
    // Where the help box goes: beside the panel, level with the row (or under it, when the screen
    // is too narrow for the box beside the panel).
    struct HelpSpot { const char* text = nullptr; float top = 0, bottom = 0, left = 0, width = 0; };
    HelpSpot hoveredHelp, selectedHelp;
    const auto OfferHelp = [&](const char* text, float top, float bottom, float left, float width, bool hot, bool selected) {
        if (hot)
            hoveredHelp = { text, top, bottom, left, width };
        if (selected)
            selectedHelp = { text, top, bottom, left, width };
    };

    // ---- Layout: the tabs down the left (search over them), the logo and the presets along the
    // top, the settings in a box, the buttons along the bottom ----
    const PanelPlace place = PanelOnScreen(screenHeight);
    const float panelW = place.w, panelH = place.h;
    const float px = place.x;
    const float py = place.y;
    const float pad = S(34);
    const float sideX = px + pad, sideW = S(262);
    const float topY = py + pad, topH = S(48);
    const float boxX = sideX + sideW + S(44), boxR = px + panelW - pad - S(36), boxW = boxR - boxX;
    const float boxY = topY + topH + S(24);
    const float buttonH = S(48), buttonsY = py + panelH - pad - buttonH;
    const float boxB = buttonsY - S(46), boxH = boxB - boxY;

    // Keyboard-focus marker: a short red bar just left of the focused item.
    const auto Marker = [&](float left, float top, float height) { Rect(left, top, S(3), height, kAccentHot); };
    const auto PanelFill = [&](float bx, float by, float bw, float bh, float r) {
        RoundedRect(bx - S(2), by - S(2), bw + S(4), bh + S(4), r + S(2), kBorder);
        RoundedRect(bx, by, bw, bh, r, kPanelColor);
    };

    // ---- The panel: near black, a red glow rising from its bottom left, a red outline ----
    PanelFill(px, py, panelW, panelH, S(8));
    GlowRounded(px, py, panelW, panelH, S(8),
                { px - panelW * 0.02f, py + panelH * 1.08f, panelW * 0.62f, panelH * 0.85f, 150, 10, 18, 0.30f });

    // ---- Search (top left): type to find a setting on any tab ----
    {
        const float sh = S(36), sy = topY + std::round((topH - sh) * 0.5f);
        const bool hot = in.Inside(sideX, sy, sideW, sh);
        const float xcx = sideX + sideW - S(17), xcy = sy + std::round(sh * 0.5f);
        const bool canClear = !g_panel.search.empty();
        const bool xHot = canClear && in.Inside(xcx - S(13), sy, S(26), sh);
        if (in.pressed) {
            if (xHot)
                ClearSearch();
            else if (hot && !g_panel.searchFocus)
                StartSearch(now);
            else if (!hot && g_panel.searchFocus)
                g_panel.searchFocus = false;  // a click anywhere else: done typing, the results stay
        }
        const uint32_t fill = Rgba(30, 30, 33, 1.0f);
        RoundedRect(sideX - thin, sy - thin, sideW + 2 * thin, sh + 2 * thin, S(5) + thin,
                    g_panel.searchFocus ? kAccent : (hot ? kOutline : Rgba(66, 66, 72, 1.0f)));
        RoundedRect(sideX, sy, sideW, sh, S(5), fill);
        // A magnifying glass.
        const float mcx = sideX + S(18), mcy = sy + std::round(sh * 0.5f) - S(1);
        Disc(mcx, mcy, S(7), White(0.92f));
        Disc(mcx, mcy, S(7) - stroke, fill);
        Segment(mcx + S(5), mcy + S(5), mcx + S(9), mcy + S(9), stroke, White(0.92f));
        const float tx = sideX + S(34), ty = sy + std::round((sh - LineHeight(kFontValue)) * 0.5f);
        float textRight = sideX + sideW - S(12);
        if (!g_panel.search.empty()) {  // an x that clears it
            const float k = S(5);
            const uint32_t xColor = xHot ? White(1.0f) : kDim;
            Segment(xcx - k, xcy - k, xcx + k, xcy + k, stroke, xColor);
            Segment(xcx - k, xcy + k, xcx + k, xcy - k, stroke, xColor);
            textRight = xcx - S(14);
        }
        if (g_panel.search.empty() && !g_panel.searchFocus) {
            Text(kFontValue, tx, ty, "Search", Rgba(196, 196, 200, 1.0f), 0);
        } else {
            std::string shown = g_panel.search;  // (its end, if it's longer than the box)
            while (!shown.empty() && tx + TextWidth(kFontValue, shown.c_str(), 0) > textRight)
                shown.erase(0, 1);
            Text(kFontValue, tx, ty, shown.c_str(), White(1.0f), 0);
            if (g_panel.searchFocus && std::fmod(now - g_panel.searchStarted, 1.0) < 0.6)
                Rect(tx + TextWidth(kFontValue, shown.c_str(), 0) + S(2), ty, std::fmax(1.0f, S(2)), LineHeight(kFontValue),
                     White(1.0f));
        }
        OfferHelp("Type to find a setting on any tab: by its name, its section or its tab. Enter keeps the results "
                  "and lets the arrow keys move through them; Esc or the x clears it.",
                  sy, sy + sh, sideX, sideW, hot, false);
    }

    // ---- The logo (top right, over the settings) ----
    float logoLeft = boxR;
    if (g_logo.valid) {
        const float lx = boxR - g_logo.w, ly = topY + std::round((topH - g_logo.h) * 0.5f);
        Quad(lx, ly, lx + g_logo.w, ly + g_logo.h, g_logo.u0, g_logo.v0, g_logo.u1, g_logo.v1, White(1.0f));
        LogoSweep(lx, ly, now - g_panel.openedAt);
        logoLeft = lx;
    } else {
        const char* title = "MXS Remastered";
        const float tw = TextWidth(kFontTitle, title, S(1));
        Text(kFontTitle, boxR - tw, topY + std::round((topH - LineHeight(kFontTitle)) * 0.5f), title, White(1.0f), S(1));
        logoLeft = boxR - tw;
    }

    // ---- Presets (top, left of the logo): PRESET  1 | 2 | 3 | 4 | 5, each its name ----
    {
        const char* label = "PRESET";
        const float labelW = TextWidth(kFontSmall, label, S(2));
        const float segH = S(34), segY = topY + std::round((topH - segH) * 0.5f);
        const float minW = S(46), padW = S(28), editW = S(150);
        const float pencilW = S(22);  // the preset in use: room for its pencil (click to rename)
        const float lx = boxX, sx0 = lx + labelW + S(14);
        const float room = std::fmax(kPresetCount * minW, logoLeft - S(28) - sx0);
        // Each segment fits its name; when they don't all fit, the one being renamed keeps its room
        // and the rest share what's left (their names cut short).
        std::string names[kPresetCount];
        float w[kPresetCount], total = 0;
        const int inUse = ActivePreset();
        for (int p = 0; p < kPresetCount; ++p) {
            names[p] = p == g_panel.renaming ? g_panel.nameDraft : PresetLabel(p);
            w[p] = std::fmax(minW, TextWidth(kFontSmall, names[p].c_str(), S(2)) + padW);
            if (p == inUse && p != g_panel.renaming)
                w[p] += pencilW;
            if (p == g_panel.renaming)
                w[p] = std::fmax(w[p] + S(6), editW);
            total += w[p];
        }
        if (total > room) {
            // The longest names are cut to one width, as wide as leaves them all fitting (the short
            // ones keep theirs).
            const int editing = g_panel.renaming;
            if (editing >= 0)
                w[editing] = std::fmin(w[editing], room - (kPresetCount - 1) * minW);
            const float budget = room - (editing >= 0 ? w[editing] : 0.0f);
            float lo = minW, hi = budget;
            for (int step = 0; step < 24; ++step) {
                const float cap = 0.5f * (lo + hi);
                float sum = 0;
                for (int p = 0; p < kPresetCount; ++p)
                    sum += p == editing ? 0.0f : std::fmin(w[p], cap);
                (sum > budget ? hi : lo) = cap;
            }
            total = 0;
            for (int p = 0; p < kPresetCount; ++p) {
                if (p != editing && w[p] > lo)
                    w[p] = std::floor(lo);
                total += w[p];
            }
        }
        const bool focused = g_panel.selected == presetItem;
        const bool areaHot = in.Inside(lx, segY, sx0 + total - lx, segH);
        OfferHelp("Five presets, each with its own copy of every setting (not the keybinds). Click one to switch "
                  "to it: your changes stay with the preset you made them in. To rename the one in use, click it (its "
                  "pencil) or press F2: type the name, Enter keeps it, Esc leaves it as it was. A pasted code never "
                  "changes a name. SAVE writes them all to MXS_REMASTERED.ini, and the game starts with the one in use.",
                  segY, segY + segH, lx, sx0 + total - lx, areaHot, focused);
        if (focused)
            Marker(lx - S(12), segY, segH);
        Text(kFontSmall, lx, segY + std::round((segH - LineHeight(kFontSmall)) * 0.5f), label, focused ? kLabel : kDim, S(2));
        const int current = ActivePreset();
        int wanted = current;
        bool renameNow = false;
        RoundedRect(sx0 - thin, segY - thin, total + 2 * thin, segH + 2 * thin, S(5) + thin, Rgba(66, 66, 72, 1.0f));
        RoundedRect(sx0, segY, total, segH, S(5), Rgba(24, 24, 27, 1.0f));
        float sx = sx0;
        for (int p = 0; p < kPresetCount; ++p) {
            const bool hot = in.Inside(sx, segY, w[p], segH);
            const bool editing = p == g_panel.renaming;
            if (hot && in.pressed && !editing) {
                g_panel.selected = presetItem;
                if (p == current)
                    renameNow = true;
                else
                    wanted = p;
            }
            if (editing) {
                RoundedRect(sx, segY, w[p], segH, S(5), kAccentHot);
                RoundedRect(sx + stroke, segY + stroke, w[p] - 2 * stroke, segH - 2 * stroke, S(4), kBoxSolid);
            } else if (p == current) {
                RoundedRect(sx, segY, w[p], segH, S(5), hot ? kAccentHot : kAccent);
            } else if (hot) {
                RoundedRect(sx, segY, w[p], segH, S(5), White(0.06f));
            }
            // The name, cut short with ".." if its segment is too narrow (while typing, its end shows).
            const bool pencil = p == current && !editing;
            std::string shown = names[p];
            const float fit = w[p] - padW - (pencil ? pencilW : 0.0f);
            if (editing) {
                while (!shown.empty() && TextWidth(kFontSmall, shown.c_str(), S(2)) > fit - S(6))
                    shown.erase(0, 1);
            } else if (TextWidth(kFontSmall, shown.c_str(), S(2)) > fit) {
                while (shown.size() > 1 && TextWidth(kFontSmall, (shown + "..").c_str(), S(2)) > fit)
                    shown.pop_back();
                shown += "..";
            }
            const float textW = TextWidth(kFontSmall, shown.c_str(), S(2));
            const float ty = segY + std::round((segH - LineHeight(kFontSmall)) * 0.5f);
            const float tx = std::round(sx + (w[p] - textW - (editing ? S(6) : 0.0f) - (pencil ? pencilW : 0.0f)) * 0.5f);
            Text(kFontSmall, tx, ty, shown.c_str(), editing || p == current ? White(1.0f) : (hot ? kLabel : kDim), S(2));
            if (pencil) {
                // A pencil after the name: this one can be renamed (click it, or anywhere on the preset).
                const float cx = std::round(tx + textW + pencilW * 0.5f + S(4)), cy = std::round(segY + segH * 0.5f);
                const uint32_t ink = White(hot ? 1.0f : 0.72f);
                const float k = s;  // (the icon's size follows the menu's scale)
                Segment(cx - 3.5f * k, cy + 3.5f * k, cx + 3.0f * k, cy - 3.0f * k, std::fmax(2.0f, 3.0f * k), ink);  // its body
                Segment(cx - 3.5f * k, cy + 3.5f * k, cx - 5.0f * k, cy + 5.0f * k, std::fmax(1.0f, 1.4f * k), ink);  // the tip
                Segment(cx + 4.2f * k, cy - 4.2f * k, cx + 5.2f * k, cy - 5.2f * k, std::fmax(2.0f, 3.0f * k), ink);  // the eraser
            }
            if (editing && std::fmod(now - g_panel.renameStarted, 1.0) < 0.6)  // the caret
                Rect(tx + textW + S(2), ty, S(2), LineHeight(kFontSmall), White(1.0f));
            sx += w[p];
        }
        // A click anywhere but the name being typed keeps it (and does what it was for).
        if (g_panel.renaming >= 0 && in.pressed) {
            float ex = sx0;
            for (int p = 0; p < g_panel.renaming; ++p)
                ex += w[p];
            if (!in.Inside(ex, segY, w[g_panel.renaming], segH))
                FinishRename(true, now);
            renameNow = false;
        }
        if (g_panel.renaming < 0) {
            if (focused && in.step)
                wanted = Clamp(current + (in.step > 0 ? 1 : -1), 0, kPresetCount - 1);
            if (focused && in.activate)
                wanted = (current + 1) % kPresetCount;
            if (focused && in.rename)
                renameNow = true;
        }
        if (wanted != current) {
            SwitchPreset(wanted);
            camera::Reset();
            g_panel.dirty = true;
            char text[64];
            snprintf(text, sizeof text, "PRESET %s IN USE - SAVE TO KEEP IT", PresetLabel(wanted).c_str());
            SetStatus(text, now);
        } else if (renameNow && g_panel.renaming < 0) {
            StartRename(current, now);
        }
    }

    // ---- Tabs (down the left) ----
    {
        if (g_panel.selected == kTabBarItem && in.step) {
            g_panel.tab = (g_panel.tab + in.step + kTabCount) % kTabCount;
            ClearSearch();
        }
        const float itemH = S(56), barH = S(50), top = boxY + S(6);
        {
            const double dt = g_panel.tabBarTime > 0 ? Clamp(now - g_panel.tabBarTime, 0.0, 0.1) : 0.0;
            g_panel.tabBarTime = now;
            const float wanted = static_cast<float>(g_panel.tab);
            if (g_panel.tabBar < 0)
                g_panel.tabBar = wanted;
            g_panel.tabBar += (wanted - g_panel.tabBar) * static_cast<float>(1.0 - std::exp(-dt / 0.06));
            if (std::fabs(wanted - g_panel.tabBar) < 0.002f)
                g_panel.tabBar = wanted;
            if (g_panel.search.empty())
                GradientH(sideX, std::round(top + g_panel.tabBar * itemH), sideW, barH, kAccent, kAccentDeep);
        }
        for (int tab = 0; tab < kTabCount; ++tab) {
            const float iy = top + tab * itemH;
            const bool hot = in.Inside(sideX, iy, sideW, barH);
            if (hot && in.pressed) {
                g_panel.tab = tab;
                g_panel.selected = kTabBarItem;
                ClearSearch();
            }
            const bool current = !g_panel.search.size() && tab == g_panel.tab;
            if (hot && !current)
                GradientH(sideX, iy, sideW, barH, White(0.08f), White(0.0f));
            Text(kFontTab, sideX + S(24), iy + std::round((barH - LineHeight(kFontTab)) * 0.5f), kTabs[tab],
                 current || hot ? White(1.0f) : Rgba(204, 204, 208, 1.0f), 0);
        }
        if (g_panel.selected == kTabBarItem)
            Marker(sideX - S(12), std::round(top + g_panel.tabBar * itemH), barH);
        // Under the tabs, at the bottom: the keys, and the credit.
        const float lh = LineHeight(kFontSmall);
        const float creditY = buttonsY - S(22) - lh;
        char hint[64];
        snprintf(hint, sizeof hint, "%s TO CLOSE", KeyName(cfg.menuKey));
        Text(kFontSmall, sideX + S(4), creditY - S(16) - 2 * lh - S(4), hint, kDim, S(1));
        Text(kFontSmall, sideX + S(4), creditY - S(16) - lh, "ARROW KEYS ADJUST", kDim, S(1));
        Text(kFontSmall, sideX + S(4), creditY, "By: Voaks", kDim, S(1));
    }

    // ---- The settings: one list in the box, scrolled ----
    RoundedRect(boxX - S(2), boxY - S(2), boxW + S(4), boxH + S(4), S(6), kBorder);
    RoundedRect(boxX, boxY, boxW, boxH, S(4), kBoxColor);
    const float sectionH = S(60), rowH = S(46), inX = S(34), topPad = S(4), bottomPad = S(22);
    const float cx = boxX + inX, cw = boxW - 2 * inX, right = cx + cw;
    // How tall the list is: a heading wherever the section (or, searching, the tab) changes.
    float listH = topPad + bottomPad;
    {
        int lastTab = -1;
        const char* lastSection = nullptr;
        for (int i = 0; i < kRowCount; ++i) {
            if (!RowShown(i))
                continue;
            const char* section = RowSection(i);
            if (kRows[i].tab != lastTab || section != lastSection)
                listH += sectionH;
            lastTab = kRows[i].tab;
            lastSection = section;
            listH += rowH;
        }
    }
    const float maxScroll = std::fmax(0.0f, listH - boxH);
    {
        if (g_panel.listTab != g_panel.tab || g_panel.listQuery != g_panel.query) {  // a new list: from its top
            g_panel.listTab = g_panel.tab;
            g_panel.listQuery = g_panel.query;
            g_panel.scroll = g_panel.scrollTarget = 0;
        }
        if (in.wheel != 0 && in.Inside(boxX, boxY, boxR + S(36) - boxX, boxH))
            g_panel.scrollTarget -= in.wheel * rowH * 3;
        g_panel.scrollTarget = Clamp(g_panel.scrollTarget, 0.0f, maxScroll);
        const double dt = g_panel.scrollTime > 0 ? Clamp(now - g_panel.scrollTime, 0.0, 0.1) : 0.0;
        g_panel.scrollTime = now;
        g_panel.scroll += (g_panel.scrollTarget - g_panel.scroll) * static_cast<float>(1.0 - std::exp(-dt / 0.05));
        if (std::fabs(g_panel.scrollTarget - g_panel.scroll) < 0.5f || g_panel.scrollDrag)
            g_panel.scroll = g_panel.scrollTarget;
        g_panel.scroll = Clamp(g_panel.scroll, 0.0f, maxScroll);
    }

    const size_t clipBegin = g_batch.size();
    const bool mouseInBox = in.Inside(boxX, boxY, boxW, boxH);
    float y = boxY + topPad - g_panel.scroll;
    int item = kFirstRowItem, lastTab = -1;
    const char* lastSection = nullptr;
    for (int i = 0; i < kRowCount; ++i) {
        if (!RowShown(i))
            continue;
        const RowDef& def = kRows[i];
        const int thisItem = item++;
        const char* section = RowSection(i);
        float headingTop = -1;
        if (def.tab != lastTab || section != lastSection) {
            headingTop = y;
            char heading[96];
            if (searching)
                snprintf(heading, sizeof heading, "%s  \xB7  %s", kTabs[def.tab], section);
            else
                snprintf(heading, sizeof heading, "%s", section);
            Text(kFontSection, cx, y + S(24), heading, kSection, 0);
            Rect(cx, y + S(50), cw, thin, kLine);
            y += sectionH;
        }
        lastTab = def.tab;
        lastSection = section;

        const bool focused = g_panel.selected == thisItem;
        if (focused && reveal) {  // the keys moved here: into view
            const float top = (headingTop >= 0 ? headingTop : y) - S(4), bottom = y + rowH + S(4);
            if (top < boxY)
                g_panel.scrollTarget -= boxY - top;
            else if (bottom > boxB)
                g_panel.scrollTarget += bottom - boxB;
        }
        const bool visible = y + rowH > boxY && y < boxB;
        const bool inactive = RowInactive(def, cfg);
        const uint32_t labelColor = inactive ? kDim : kLabel;
        const uint32_t valueColor = inactive ? kDim : White(1.0f);
        const float hx = boxX + S(6), hw = boxW - S(12);  // the row's hover area: the box's width
        const bool hot = visible && mouseInBox && in.Inside(hx, y, hw, rowH);
        if (hot && in.pressed)
            g_panel.selected = thisItem;
        const bool active = g_panel.dragging == i;
        // The slider being adjusted while the menu fades: kept, on a backdrop of its own.
        const bool kept = i == g_panel.adjustRow && g_panel.opacity < 0.999f;
        const size_t keptBegin = g_batch.size();
        if (kept)
            RoundedRect(hx, y - S(2), hw, rowH + S(4), S(6), Rgba(8, 8, 10, 0.92f * (1.0f - g_panel.opacity)));
        if (hot || active)
            Rect(hx, y, hw, rowH, White(0.04f));
        if (focused)
            Marker(boxX + S(14), y + S(10), rowH - S(20));
        OfferHelp(def.help, std::fmax(y, boxY), std::fmin(y + rowH, boxB), hx, hw, hot, focused);
        const float midY = std::round(y + rowH * 0.5f);
        Text(kFontLabel, cx, y + std::round((rowH - LineHeight(kFontLabel)) * 0.5f), def.label, labelColor, 0);
        if (def.badge) {
            // A small red-outlined tag right after the label.
            const float tagX = cx + TextWidth(kFontLabel, def.label, 0) + S(14);
            const float tagW = TextWidth(kFontSmall, def.badge, S(2)) + S(16), tagH = S(22);
            const float tagY = std::round(y + (rowH - tagH) * 0.5f);
            RoundedRect(tagX, tagY, tagW, tagH, S(4), kAccent);
            RoundedRect(tagX + 1, tagY + 1, tagW - 2, tagH - 2, std::fmax(0.0f, S(4) - 1), kBoxSolid);
            Text(kFontSmall, tagX + S(8), tagY + std::round((tagH - LineHeight(kFontSmall)) * 0.5f), def.badge,
                 Rgba(236, 38, 44, 1.0f), S(2));
        }

        if (def.kind == RowKind::Slider) {
            // A white bar on a thin red track, the value at the right.
            double& value = cfg.*SliderField(def, cfg);
            char text[32];
            FormatValue(def, value, text, sizeof text);
            const float valueW = std::fmax(S(96), TextWidth(kFontValue, text, 0) + S(24));
            const float trackW = S(290), tx0 = right - valueW - trackW;
            const bool trackHot = hot && in.mx >= tx0 - S(16) && in.mx <= tx0 + trackW + S(16);
            if (trackHot && in.pressed && !def.locked)
                g_panel.dragging = i;
            if (active && in.down) {
                SetSlider(def, value, def.min + Clamp((in.mx - tx0) / trackW, 0.0f, 1.0f) * (def.max - def.min));
                g_panel.adjustRow = i;
            }
            if (focused && in.step && !def.locked) {
                SetSlider(def, value, value + in.step * def.step * (in.coarse ? 10 : 1));
                g_panel.adjustRow = i;
                g_panel.adjustedAt = now;
            }
            FormatValue(def, value, text, sizeof text);
            Text(kFontValue, right - TextWidth(kFontValue, text, 0), y + std::round((rowH - LineHeight(kFontValue)) * 0.5f), text,
                 valueColor, 0);
            const float t = static_cast<float>((value - def.min) / (def.max - def.min));
            const float knobX = std::round(tx0 + t * trackW);
            const float lineH = std::fmax(1.0f, S(2)), barH = std::fmax(3.0f, S(5));
            Rect(tx0, midY - std::round(lineH * 0.5f), trackW, lineH, inactive ? Rgba(74, 74, 80, 1.0f) : kTrack);
            float from = tx0;
            if (def.min < 0 && !def.clock) {  // signed values fill from the zero
                from = std::round(tx0 + static_cast<float>(-def.min / (def.max - def.min)) * trackW);
                Rect(from, midY - S(6), thin, S(12), kDim);
            }
            const float fillX = std::fmin(from, knobX), fillW = std::fabs(knobX - from);
            if (fillW >= 1.0f)
                RoundedRect(fillX, midY - std::round(barH * 0.5f), fillW, barH, barH * 0.5f, inactive ? kDim : White(0.96f));
            if (active || trackHot)
                Disc(knobX, midY, S(11), Rgba(206, 18, 26, 0.4f));
            Disc(knobX, midY, active ? S(7) : (trackHot ? S(6) : S(5)), inactive ? kDim : White(1.0f));
        } else if (def.kind == RowKind::Toggle) {
            // A checkbox: red with a white tick when on.
            bool& flag = cfg.*def.flag;
            if (!def.locked && ((hot && in.pressed) || (focused && (in.step || in.activate)))) {
                SetFlag(flag, !flag);
                if (def.flag == &Config::enabled)
                    camera::Reset();
                if (def.flag == &Config::dabIndicator && flag) {  // turned on: is the game's Sticky Sit/Dab on?
                    g_panel.askSticky = true;
                    g_panel.askChoice = cfg.dabSticky == 2 ? 1 : 0;
                }
            }
            const float bs = S(20), bx = right - bs, by = std::round(midY - bs * 0.5f);
            if (flag) {
                RoundedRect(bx, by, bs, bs, S(3), inactive ? kDim : (hot ? kAccentHot : kAccent));
                const float tw = std::fmax(1.5f, S(2.5f));
                Segment(bx + bs * 0.25f, by + bs * 0.52f, bx + bs * 0.43f, by + bs * 0.70f, tw, White(1.0f));
                Segment(bx + bs * 0.43f, by + bs * 0.70f, bx + bs * 0.76f, by + bs * 0.31f, tw, White(1.0f));
            } else {
                RoundedRect(bx, by, bs, bs, S(3), hot ? Rgba(196, 196, 202, 1.0f) : kOutline);
                RoundedRect(bx + stroke, by + stroke, bs - 2 * stroke, bs - 2 * stroke, std::fmax(0.0f, S(3) - stroke), kBoxSolid);
            }
        } else if (def.kind == RowKind::Key || def.kind == RowKind::Pad) {
            // The key's (control's) name in a box; click it, then press one.
            const bool listening = g_panel.listening == i;
            if (listening && in.pressed)  // a second click, here or anywhere else, cancels
                g_panel.listening = -1;
            else if ((hot && in.pressed) || (focused && in.activate))
                StartListening(i);
            const bool waiting = g_panel.listening == i;
            const char* text = def.kind == RowKind::Pad
                ? (waiting ? "PRESS OR PUSH" : gamepad::ControlName(cfg.*def.pad))
                : (waiting ? "PRESS A KEY" : KeyName(cfg.*def.key));
            const float vh = S(32), textW = TextWidth(kFontValue, text, 0);
            const float vw = std::fmax(textW + S(36), S(170)), vx = right - vw, vy = std::round(midY - vh * 0.5f);
            if (waiting) {
                RoundedRect(vx, vy, vw, vh, S(3), kAccent);
            } else {
                RoundedRect(vx, vy, vw, vh, S(3), hot ? Rgba(196, 196, 202, 1.0f) : kOutline);
                RoundedRect(vx + thin, vy + thin, vw - 2 * thin, vh - 2 * thin, S(2), Rgba(18, 18, 20, 1.0f));
            }
            Text(kFontValue, std::round(vx + (vw - textW) * 0.5f), vy + std::round((vh - LineHeight(kFontValue)) * 0.5f), text,
                 waiting ? White(1.0f) : valueColor, 0);
        } else {
            // A choice of two to four: < VALUE > with a bar under it, a segment per option.
            const char* options[4] = { def.offText, def.onText, def.thirdText, def.fourthText };
            const int count = def.option ? (def.fourthText ? 4 : 3) : 2;
            const auto Current = [&]() { return def.option ? cfg.*def.option : (cfg.*def.flag ? 1 : 0); };
            const auto Pick = [&](int o) {
                if (def.option)
                    SetOption(cfg.*def.option, o);
                else
                    SetFlag(cfg.*def.flag, o == 1);
            };
            float widest = 0;
            for (int o = 0; o < count; ++o)
                widest = std::fmax(widest, TextWidth(kFontValue, options[o], 0));
            const float vw = std::fmax(S(160), widest + S(40)), vh = S(30), arrowW = S(26);
            const float vx = right - arrowW - vw, vy = std::round(midY - vh * 0.5f) - S(3);
            const bool leftHot = hot && in.mx >= vx - arrowW && in.mx < vx;
            const bool rightHot = hot && in.mx >= vx + vw && in.mx < vx + vw + arrowW;
            const bool valueHot = hot && in.mx >= vx && in.mx < vx + vw;
            int current = Current();
            if (in.pressed) {
                if (leftHot && current > 0)
                    Pick(current - 1);
                else if (rightHot && current < count - 1)
                    Pick(current + 1);
                else if (valueHot)
                    Pick((current + 1) % count);
            }
            if (focused && in.step)
                Pick(Clamp(current + (in.step > 0 ? 1 : -1), 0, count - 1));
            if (focused && in.activate)
                Pick((current + 1) % count);
            current = Current();
            const uint32_t arrowOn = inactive ? kDim : White(0.95f), arrowOff = Rgba(70, 70, 76, 1.0f);
            const float ak = S(7);
            Chevron(vx - arrowW * 0.5f, vy + vh * 0.5f, ak, 0, stroke,
                    current > 0 ? (leftHot ? kAccentHot : arrowOn) : arrowOff);
            Chevron(vx + vw + arrowW * 0.5f, vy + vh * 0.5f, ak, 1, stroke,
                    current < count - 1 ? (rightHot ? kAccentHot : arrowOn) : arrowOff);
            RoundedRect(vx, vy, vw, vh, S(2), valueHot ? Rgba(214, 214, 220, 1.0f) : Rgba(150, 150, 156, 1.0f));
            RoundedRect(vx + thin, vy + thin, vw - 2 * thin, vh - 2 * thin, S(2), Rgba(10, 10, 12, 1.0f));
            const char* shown = options[Clamp(current, 0, count - 1)];
            Text(kFontValue, std::round(vx + (vw - TextWidth(kFontValue, shown, 0)) * 0.5f),
                 vy + std::round((vh - LineHeight(kFontValue)) * 0.5f), shown, valueColor, 0);
            const float gap = S(3), barY = vy + vh + S(4), barH = std::fmax(2.0f, S(3));
            const float segW = (vw - (count - 1) * gap) / count;
            for (int o = 0; o < count; ++o)
                Rect(vx + o * (segW + gap), barY, segW, barH,
                     o == current ? (inactive ? kDim : White(0.95f)) : Rgba(64, 64, 70, 1.0f));
        }
        if (kept)
            g_keepRanges.push_back({ keptBegin, g_batch.size() });
        y += rowH;
    }
    if (rowsShown == 0) {
        char none[96];
        snprintf(none, sizeof none, "Nothing matches \"%s\"", g_panel.query.c_str());
        Text(kFontLabel, cx, boxY + S(30), none, kDim, 0);
    }
    g_clips.push_back({ clipBegin, g_batch.size(), boxX, boxY, boxW, boxH });

    // ---- Scrollbar (right of the box): arrows at either end, the thumb between ----
    {
        const float sbW = S(6), sbX = boxR + S(17), mid = sbX + sbW * 0.5f;
        const float trackTop = boxY + S(26), trackLen = boxB - S(26) - trackTop;
        const bool scrollable = maxScroll > 0.5f;
        const bool upHot = scrollable && in.Inside(sbX - S(12), boxY - S(6), sbW + S(24), S(28));
        const bool downHot = scrollable && in.Inside(sbX - S(12), boxB - S(22), sbW + S(24), S(28));
        const uint32_t idle = Rgba(170, 170, 176, 1.0f), off = Rgba(70, 70, 76, 1.0f);
        Chevron(mid, boxY + S(8), S(7), 2, stroke, scrollable ? (upHot ? White(1.0f) : idle) : off);
        Chevron(mid, boxB - S(8), S(7), 3, stroke, scrollable ? (downHot ? White(1.0f) : idle) : off);
        RoundedRect(sbX, trackTop, sbW, trackLen, sbW * 0.5f, Rgba(36, 36, 40, 1.0f));
        const float thumbLen = scrollable ? std::fmax(S(36), trackLen * boxH / listH) : trackLen;
        const float thumbY = trackTop + (scrollable ? (trackLen - thumbLen) * (g_panel.scroll / maxScroll) : 0.0f);
        const bool thumbHot = scrollable && in.Inside(sbX - S(8), thumbY, sbW + S(16), thumbLen);
        if (in.pressed && scrollable) {
            if (upHot)
                g_panel.scrollTarget -= rowH * 3;
            else if (downHot)
                g_panel.scrollTarget += rowH * 3;
            else if (thumbHot) {
                g_panel.scrollDrag = true;
                g_panel.scrollGrab = in.my - thumbY;
            } else if (in.Inside(sbX - S(8), trackTop, sbW + S(16), trackLen)) {
                g_panel.scrollTarget += (in.my < thumbY ? -1.0f : 1.0f) * boxH * 0.9f;
            }
        }
        if (g_panel.scrollDrag) {
            if (!in.down || !scrollable) {
                g_panel.scrollDrag = false;
            } else {
                const float t = Clamp((in.my - g_panel.scrollGrab - trackTop) / std::fmax(1.0f, trackLen - thumbLen), 0.0f, 1.0f);
                g_panel.scrollTarget = g_panel.scroll = t * maxScroll;
            }
        }
        RoundedRect(sbX, thumbY, sbW, thumbLen, sbW * 0.5f,
                    !scrollable ? Rgba(60, 60, 66, 1.0f) : (thumbHot || g_panel.scrollDrag ? Rgba(214, 214, 220, 1.0f) : Rgba(150, 150, 156, 1.0f)));
        g_panel.scrollTarget = Clamp(g_panel.scrollTarget, 0.0f, maxScroll);
    }

    // ---- Under the box: what's going on (the tab's live readout; a message, while there is one) ----
    {
        char readout[256];
        const camera::Telemetry& t = camera::LastTelemetry();
        if (searching) {
            snprintf(readout, sizeof readout, "%d SETTING%s FOUND   \xB7   ESC CLEARS THE SEARCH", rowsShown, rowsShown == 1 ? "" : "S");
        } else if (g_panel.tab == kTabGraphics) {
            const char* tires = postfx::TireStatusText();
            snprintf(readout, sizeof readout, "%s   \xB7   %s", postfx::StatusText(), tires[0] ? tires : texfilter::StatusText());
        } else if (g_panel.tab == kTabKeybinds) {
            snprintf(readout, sizeof readout, "CLICK A KEY, THEN PRESS THE NEW ONE   \xB7   ESC CANCELS   \xB7   SAVE TO KEEP THEM");
        } else if (g_panel.tab == kTabWeather) {
            if (!cfg.shaders)
                snprintf(readout, sizeof readout, "WEATHER NEEDS THE SHADERS ON (GRAPHICS TAB)");
            else if (!cfg.snow)
                snprintf(readout, sizeof readout, "NO WEATHER   \xB7   TURN ON SNOW TO MAKE IT SNOW");
            else
                snprintf(readout, sizeof readout, "SNOW ON   \xB7   SNOWFALL %.0f%%   \xB7   WIND %.0f%%", cfg.snowfall * 100.0,
                         cfg.snowWind * 100.0);
        } else if (g_panel.tab == kTabLook) {
            snprintf(readout, sizeof readout, "%s   \xB7   THE LOOK BUTTONS ARE ON THE KEYBINDS TAB",
                     cfg.controllerLook ? gamepad::StatusText() : "CONTROLLER LOOK OFF");
        } else if (g_panel.tab == kTabSounds) {
            snprintf(readout, sizeof readout, "ENGINE SOUNDS ARE HEARD FROM THE NEXT TIME THE GAME STARTS   \xB7   "
                     "THE BRAKE SQUEAL RIGHT AWAY");
        } else if (g_panel.tab == kTabOther) {
            const char* status = GameOverlayStatusText();
            snprintf(readout, sizeof readout, "%s", status[0] ? status : "THE GAME'S MENU (ESC) SHOWS A HIDDEN HUD, THE MOUSE A HIDDEN CURSOR");
        } else if (!cfg.enabled) {
            snprintf(readout, sizeof readout, "FIRST PERSON MOD IS OFF%s",
                     g_panel.tab == kTabReplay ? " - REPLAYS USE THE GAME'S OWN CAMERA" : "");
        } else if (g_panel.tab == kTabFirstPerson) {
            char view[96];
            if (cfg.customCamera && !t.bikeTracked)
                snprintf(view, sizeof view, "%s - RIDE A FEW METRES", bike::StatusText());
            else
                snprintf(view, sizeof view, "%s   PITCH %+.0f\xB0   LEAN %.0f\xB0",
                         t.customActive ? "CUSTOM CAM" : (t.bikeTracked ? "BIKE FOUND" : "GAME CAM"),
                         t.pitchExcessDeg, std::fabs(t.rawRollDeg));
            snprintf(readout, sizeof readout, "%s   \xB7   %s   LOOK %+.0f\xB0   %s   \xB7   LAG %.1f\xB0   %.1f G   NECK %+.0f MM",
                     view, t.airborne > 0.5 ? "AIR" : "GROUND", t.lookYawDeg,
                     cfg.controllerLook ? gamepad::StatusText() : "LOOK OFF", t.lagDeg, 1.0 + t.verticalAccelG,
                     t.neckOffset * 1000.0);
        } else {
            snprintf(readout, sizeof readout, "%s", replay::StatusText());
        }
        const float ry = boxB + std::round((S(46) - LineHeight(kFontSmall)) * 0.5f);
        if (g_panel.listening >= 0) {
            char label[64], hint[160];
            Upper(kRows[g_panel.listening].label, label, sizeof label);
            if (kRows[g_panel.listening].kind == RowKind::Pad)
                snprintf(hint, sizeof hint, "PRESS A BUTTON OR PUSH A STICK FOR %s   \xB7   BACKSPACE CLEARS   \xB7   ESC CANCELS", label);
            else
                snprintf(hint, sizeof hint, "PRESS THE NEW KEY FOR %s   \xB7   ESC CANCELS", label);
            Text(kFontSmall, boxX, ry, hint, White(1.0f), S(1));
        } else if (now < g_panel.statusUntil) {
            Text(kFontSmall, boxX, ry, g_panel.status, White(1.0f), S(1));
        } else {
            Text(kFontSmall, boxX, ry, readout, kInfo, S(1));
        }
    }

    // ---- Buttons: CLOSE (under the tabs), the codes (under the box), DEFAULTS and SAVE (right) ----
    const auto Button = [&](int buttonItem, float bx, float bw, const char* label, bool primary, const char* help) {
        const float by = buttonsY;
        const bool hot = in.Inside(bx, by, bw, buttonH);
        OfferHelp(help, by, by + buttonH, bx, bw, hot, g_panel.selected == buttonItem);
        if (hot && in.pressed) {
            g_panel.pressedItem = buttonItem;
            g_panel.selected = buttonItem;
        }
        const bool clicked = (hot && in.released && g_panel.pressedItem == buttonItem) ||
                             (g_panel.selected == buttonItem && in.activate);
        const bool focused = hot || g_panel.selected == buttonItem;
        const bool held = hot && in.down && g_panel.pressedItem == buttonItem;
        const float r = S(4), b = std::fmax(1.0f, S(2));
        if (primary) {
            RoundedRect(bx, by, bw, buttonH, r, focused ? Rgba(255, 96, 100, 1.0f) : Rgba(232, 52, 58, 1.0f));
            GradientV(bx + b, by + b, bw - 2 * b, buttonH - 2 * b, held ? kAccent : (focused ? Rgba(236, 38, 44, 1.0f) : Rgba(214, 22, 30, 1.0f)),
                      held ? kAccentDeep : Rgba(140, 8, 14, 1.0f));
        } else {
            RoundedRect(bx, by, bw, buttonH, r, focused ? Rgba(176, 176, 182, 1.0f) : Rgba(88, 88, 94, 1.0f));
            GradientV(bx + b, by + b, bw - 2 * b, buttonH - 2 * b, held ? Rgba(30, 30, 33, 1.0f) : (focused ? Rgba(54, 54, 58, 1.0f) : Rgba(42, 42, 46, 1.0f)),
                      Rgba(18, 18, 20, 1.0f));
        }
        const float labelW = TextWidth(kFontButton, label, 0);
        Text(kFontButton, std::round(bx + (bw - labelW) * 0.5f), by + std::round((buttonH - LineHeight(kFontButton)) * 0.5f), label,
             White(1.0f), 0);
        return clicked;
    };

    const float bigW = S(210), gap = S(16);
    if (Button(saveItem, boxR - bigW, bigW, g_panel.dirty ? "SAVE" : "SAVED", g_panel.dirty,
               "Writes your settings and keybinds to MXS_REMASTERED.ini so they load next time.")) {
        if (SaveConfig()) {
            g_panel.dirty = false;
            SetStatus("SAVED TO MXS_REMASTERED.INI", now);
        } else {
            SetStatus("COULD NOT WRITE MXS_REMASTERED.INI", now);
        }
    }
    if (Button(defaultsItem, boxR - 2 * bigW - gap, bigW, "DEFAULTS", false,
               "Puts every first person, replay, graphics and other setting of this preset back to its "
               "default (not the keybinds). It asks first; press SAVE afterwards to keep it, CANCEL to undo it.")) {
        g_panel.askDefaults = true;
        g_panel.askChoice = 0;  // (CANCEL highlighted: Enter by mistake changes nothing)
    }
    // CANCEL: only while something is changed - every change since the last SAVE undone.
    if (g_panel.dirty) {
        const float cancelW = S(150);
        if (Button(cancelItem, boxR - 2 * bigW - 2 * gap - cancelW, cancelW, "CANCEL", false,
                   "Undoes every change since you last pressed SAVE: all your settings, presets and keybinds go "
                   "back to what's in MXS_REMASTERED.ini.")) {
            ReloadConfig();
            camera::Reset();
            g_panel.dirty = false;
            SetStatus("CHANGES CANCELLED - BACK TO YOUR SAVED SETTINGS", now);
            Log("panel: unsaved changes cancelled (settings read again from the ini)");
        }
    }
    if (Button(closeItem, sideX, sideW, "CLOSE", false, "Closes the menu. Unsaved changes stay until you quit the game."))
        g_panel.closeRequest = true;

    // Preset codes: COPY CODE / PASTE CODE (first person and graphics have a code each; on the other
    // tabs only PASTE CODE shows).
    {
        const float cwBtn = S(170);
        const int codeTab = searching ? -1 : CodeOfTab(g_panel.tab);
        const bool canCopy = codeTab >= 0 && HasPresetCode(codeTab);
        if (!canCopy && g_panel.selected == copyCodeItem)
            g_panel.selected = pasteCodeItem;  // (moved onto with the keyboard or a controller: it isn't shown)
        static char copyHelp[320];  // (the help box shows it after this block)
        snprintf(copyHelp, sizeof copyHelp,
                 "Copies your %s settings (this preset's) as a code, one line of text to share: paste it in Discord "
                 "or anywhere. Whoever loads it with PASTE CODE gets your %s settings and keeps the rest of theirs.",
                 codeTab == 0 ? "first person camera" : "graphics", codeTab == 0 ? "camera" : "graphics");
        if (canCopy && Button(copyCodeItem, boxX, cwBtn, "COPY CODE", false, copyHelp)) {
            const std::string code = PresetCode(codeTab);
            char text[112];
            if (code.empty()) {
                SetStatus("CODES ARE FOR THE FIRST PERSON AND GRAPHICS TABS - OPEN ONE TO COPY ITS CODE", now);
            } else if (CopyToClipboard(code)) {
                snprintf(text, sizeof text, "%s CODE COPIED (PRESET %s, %d CHARACTERS) - PASTE IT TO SHARE", kTabs[TabOfCode(codeTab)],
                         PresetLabel(ActivePreset()).c_str(), static_cast<int>(code.size()));
                SetStatus(text, now);
                Log("overlay: preset %d's %s code copied: %s", ActivePreset() + 1, kTabs[TabOfCode(codeTab)], code.c_str());
            } else {
                SetStatus("COULD NOT COPY TO THE CLIPBOARD", now);
            }
        }
        const float pasteX = canCopy ? boxX + cwBtn + S(12) : boxX;
        if (Button(pasteCodeItem, pasteX, cwBtn, "PASTE CODE", false,
                   "Copy someone's code first (from Discord or anywhere: the whole message is fine), then press "
                   "this to load it into the preset in use. A first person code sets the first person settings, a "
                   "graphics code the graphics: everything else stays yours, the preset's name too. Press SAVE to keep "
                   "it: quit without saving and the preset is as you last saved it.")) {
            const PresetCodeResult r = LoadPresetCode(ClipboardText().c_str());
            char text[128];
            switch (r.status) {
            case PresetCodeResult::NotFound:
                SetStatus("NO CODE ON THE CLIPBOARD - COPY ONE FIRST (IT STARTS WITH MXS-REMASTERED-)", now);
                break;
            case PresetCodeResult::Damaged:
                SetStatus("THAT CODE IS INCOMPLETE OR MISTYPED - COPY ALL OF IT", now);
                break;
            case PresetCodeResult::Newer:
                SetStatus("THAT CODE IS FROM A NEWER MXS REMASTERED - UPDATE TO LOAD IT", now);
                break;
            case PresetCodeResult::Loaded:
                camera::Reset();
                g_panel.dirty = true;
                snprintf(text, sizeof text, "%s CODE LOADED INTO PRESET %s%s - SAVE TO KEEP IT",
                         TabOfCode(r.tab) >= 0 ? kTabs[TabOfCode(r.tab)] : "",
                         PresetLabel(ActivePreset()).c_str(), r.unknown ? " (SOME NEWER SETTINGS LEFT OUT)" : "");
                SetStatus(text, now);
                break;
            }
        }
    }

    if (in.released || !in.down) {
        g_panel.dragging = -1;
        if (in.released)
            g_panel.pressedItem = -1;
    }

    // ---- Help box: beside the panel, level with its row; on a screen too narrow for that, just
    // under (or over) the row ----
    const HelpSpot& help = g_panel.helpForMouse ? hoveredHelp : selectedHelp;
    if (help.text) {
        const float padX = S(16), padY = S(12), lineH = LineHeight(kFontBody), margin = S(8);
        const bool beside = px + panelW + S(14) + S(340) <= screenWidth - margin;
        const float boxWidth = beside ? S(340) : std::fmax(help.width, S(300));
        char lines[kMaxWrapLines][kWrapLineSize];
        const int count = WrapText(kFontBody, help.text, boxWidth - 2 * padX, lines);
        const float boxHeight = 2 * padY + count * lineH;
        float bx, by;
        if (beside) {
            bx = px + panelW + S(14);
            by = std::fmax(S(16), std::fmin(help.top, screenHeight - boxHeight - S(16)));
        } else {
            bx = std::fmax(margin, std::fmin(help.left, screenWidth - boxWidth - margin));
            by = help.bottom + S(6);
            if (by + boxHeight > screenHeight - margin)
                by = std::fmax(margin, help.top - boxHeight - S(6));
        }
        PanelFill(bx, by, boxWidth, boxHeight, S(6));
        for (int i = 0; i < count; ++i)
            Text(kFontBody, bx + padX, by + padY + i * lineH, lines[i], kLabel, 0);
    }

    // Our own cursor (the system one is hidden over the game window while the panel is open).
    if (in.inside) {
        const size_t cursorBegin = g_batch.size();
        Disc(in.mx, in.my, S(6) + 1.5f, Rgba(0, 0, 0, 0.55f));
        Disc(in.mx, in.my, S(5), White(1.0f));
        g_keepRanges.push_back({ cursorBegin, g_batch.size() });
    }
}

// The fade (Config::fadeMenu): toward nearly see-through while a GRAPHICS slider is being dragged
// or stepped, back when it's let go; quick out, a little slower back in.
void UpdateFade(double now)
{
    const Config& cfg = GetConfig();
    const double dt = g_panel.lastFrame > 0 ? Clamp(now - g_panel.lastFrame, 0.0, 0.1) : 0.0;
    g_panel.lastFrame = now;
    // For automated tests: as if the GRAPHICS tab's first slider were being stepped.
    static const bool testFade = GetEnvironmentVariableA("MXCAM_TEST_MENU_FADE", nullptr, 0) > 0;
    if (testFade) {
        for (int i = 0; i < kRowCount; ++i)
            if (kRows[i].tab == kTabGraphics && kRows[i].kind == RowKind::Slider) {
                g_panel.adjustRow = i;
                g_panel.adjustedAt = now;
                break;
            }
    }
    const int row = g_panel.adjustRow;
    const bool graphicsSlider = row >= 0 && row < kRowCount && RowShown(row) && kRows[row].kind == RowKind::Slider &&
                                (kRows[row].tab == kTabGraphics || kRows[row].tab == kTabWeather);
    const bool adjusting = cfg.fadeMenu && graphicsSlider &&
                           (g_panel.dragging == row || (g_panel.adjustedAt >= 0 && now - g_panel.adjustedAt < kAdjustHold));
    const float target = adjusting ? kFadedOpacity : 1.0f;
    const double tau = target < g_panel.opacity ? 0.08 : 0.2;
    g_panel.opacity = target + (g_panel.opacity - target) * static_cast<float>(std::exp(-dt / tau));
    if (std::fabs(g_panel.opacity - target) < 0.002f)
        g_panel.opacity = target;
    if (!adjusting && g_panel.opacity >= 1.0f && g_panel.dragging != row)
        g_panel.adjustRow = -1;
}

// Everything drawn this frame but the kept parts, at the fade's opacity.
void ApplyFade()
{
    if (g_panel.opacity >= 1.0f)
        return;
    size_t next = 0;  // the kept ranges are in batch order
    for (size_t v = 0; v < g_batch.size(); ++v) {
        while (next < g_keepRanges.size() && v >= g_keepRanges[next].end)
            ++next;
        if (next < g_keepRanges.size() && v >= g_keepRanges[next].begin)
            continue;
        uint32_t& color = g_batch[v].color;
        const uint32_t alpha = static_cast<uint32_t>((color >> 24) * g_panel.opacity + 0.5f);
        color = (color & 0x00FFFFFFu) | (alpha << 24);
    }
}

void DrawBatch(int width, int height)
{
    if (g_batch.empty())
        return;
    if (g_gl.BindFramebuffer)
        g_gl.BindFramebuffer(kDrawFramebuffer, 0);
    g_gl.Viewport(0, 0, width, height);
    g_gl.Disable(GL_DEPTH_TEST);
    g_gl.Disable(GL_CULL_FACE);
    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.Disable(GL_STENCIL_TEST);
    if (g_gl.BindFramebuffer)
        g_gl.Disable(kFramebufferSrgb);
    g_gl.Enable(GL_BLEND);
    g_gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    g_gl.BlendEquation(kFuncAdd);
    g_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    g_gl.UseProgram(g_gpu.program);
    g_gl.Uniform2f(g_gpu.screenUniform, static_cast<GLfloat>(width), static_cast<GLfloat>(height));
    g_gl.Uniform1i(g_gpu.atlasUniform, 0);
    g_gl.BindTexture(GL_TEXTURE_2D, g_gpu.texture);  // unit 0 (made active by SaveState)
    if (g_gl.BindSampler)
        g_gl.BindSampler(0, 0);

    g_gl.BindVertexArray(g_gpu.vao);
    g_gl.BindBuffer(kArrayBuffer, g_gpu.vbo);
    g_gl.BufferData(kArrayBuffer, static_cast<ptrdiff_t>(g_batch.size() * sizeof(Vertex)), g_batch.data(), kStreamDraw);
    // In order: what's outside the clip ranges as it is, each clip range inside its rectangle.
    size_t at = 0;
    const auto Draw = [](size_t from, size_t to) {
        if (to > from)
            g_gl.DrawArrays(GL_TRIANGLES, static_cast<GLint>(from), static_cast<GLsizei>(to - from));
    };
    for (const ClipRange& clip : g_clips) {
        Draw(at, clip.begin);
        g_gl.Enable(GL_SCISSOR_TEST);
        const int x0 = static_cast<int>(std::floor(clip.x)), x1 = static_cast<int>(std::ceil(clip.x + clip.w));
        const int y0 = static_cast<int>(std::floor(clip.y)), y1 = static_cast<int>(std::ceil(clip.y + clip.h));
        g_gl.Scissor(x0, height - y1, x1 - x0 > 0 ? x1 - x0 : 0, y1 - y0 > 0 ? y1 - y0 : 0);
        Draw(clip.begin, clip.end);
        g_gl.Disable(GL_SCISSOR_TEST);
        at = clip.end;
    }
    Draw(at, g_batch.size());
}

// ===========================================================================
// The startup hint: every time the game starts, which key opens this menu (players kept asking).
// Shown kHintHold seconds from the first frame, then faded out over kHintFade; gone for good once
// the menu is opened. The key is the menu key as it is now, so a rebind shows on it at once.
// ===========================================================================
constexpr double kHintHold = 5.0;
constexpr double kHintFade = 1.0;
double g_hintStart = -1;
bool   g_hintDone  = false;

uint32_t Faded(uint32_t color, float a)
{
    const uint32_t alpha = static_cast<uint32_t>((color >> 24) * Clamp(a, 0.0f, 1.0f) + 0.5f);
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

void BuildStartupHint(int width, int height, double now)
{
    const float s = g_atlasScale;
    const auto S = [s](float v) { return std::round(v * s); };
    const double t = now - g_hintStart;
    const float a = t < kHintHold ? 1.0f : static_cast<float>(1.0 - (t - kHintHold) / kHintFade);
    const char* lead = "MXS Remastered:";
    char rest[96];
    snprintf(rest, sizeof rest, "Press \"%s\" to open menu", KeyName(GetConfig().menuKey));
    // In the middle of the screen, in the menu's title font, the corners only a little rounded.
    const float gap = S(14), padX = S(40), padY = S(24);
    const float leadW = TextWidth(kFontTitle, lead, S(1)), restW = TextWidth(kFontTitle, rest, 0);
    const float w = padX * 2 + leadW + gap + restW, h = padY * 2 + LineHeight(kFontTitle);
    const float x = std::round((width - w) * 0.5f), y = std::round((height - h) * 0.5f), r = S(8);
    RoundedRect(x - S(2), y - S(2), w + S(4), h + S(4), r + S(2), Faded(kBorder, a));
    RoundedRect(x, y, w, h, r, Faded(kPanelColor, a));
    Text(kFontTitle, x + padX, y + padY, lead, Faded(kAccentHot, a), S(1));
    Text(kFontTitle, x + padX + leadW + gap, y + padY, rest, Faded(White(1.0f), a), 0);
}

// Draws the startup hint over this frame (the menu closed), while it lasts.
void StartupHintFrame(HWND window, double now)
{
    if (g_hintDone)
        return;
    if (g_hintStart < 0)
        g_hintStart = now;
    if (now - g_hintStart > kHintHold + kHintFade) {
        g_hintDone = true;
        return;
    }
    RECT client;
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return;
    const int width = client.right, height = client.bottom;
    const float scale = std::round(Clamp(std::fmin(height / 1080.0f, width / 1440.0f), 0.6f, 2.0f) * 20.0f) / 20.0f;  // (as the menu's)
    SavedState saved;
    SaveState(saved);
    if (EnsureResources(scale)) {
        g_batch.clear();
        g_keepRanges.clear();
        g_clips.clear();
        BuildStartupHint(width, height, now);
        DrawBatch(width, height);
    }
    RestoreState(saved);
}

// ===========================================================================
// A question over the menu, centred on it (the rest of the screen dimmed): a title, the question,
// two lines under it and its buttons at the bottom right, "ESC CANCELS" at the left when Esc does.
// The buttons answer to the mouse, the arrow keys and Enter (g_panel.askChoice is the one lit).
// Returns the button chosen this frame, or -1.
int BuildQuestion(int screenWidth, int screenHeight, const Input& in, const char* title, const char* question,
                  const char* line1, const char* line2, const char* const* labels, int buttons, bool escCancels)
{
    const float s = g_atlasScale;
    const auto S = [s](float v) { return std::round(v * s); };
    Rect(0, 0, static_cast<float>(screenWidth), static_cast<float>(screenHeight), Rgba(0, 0, 0, 0.55f));
    const PanelPlace panel = PanelOnScreen(screenHeight);
    const float boxW = S(640), boxH = S(260);
    const float bx = std::round(panel.x + (panel.w - boxW) * 0.5f), by = std::round(panel.y + (panel.h - boxH) * 0.5f);
    RoundedRect(bx - 1, by - 1, boxW + 2, boxH + 2, S(10) + 1, kBorder);
    RoundedRect(bx, by, boxW, boxH, S(10), kPanelSolid);
    const float pad = S(28);
    float y = by + pad;
    Text(kFontSmall, bx + pad, y, title, kSection, S(2));
    y += LineHeight(kFontSmall) + S(14);
    Text(kFontBody, bx + pad, y, question, kLabel, 0);
    y += LineHeight(kFontBody) + S(10);
    if (line1 && *line1)
        Text(kFontSmall, bx + pad, y, line1, kDim, S(1));
    y += LineHeight(kFontSmall) + S(4);
    if (line2 && *line2)
        Text(kFontSmall, bx + pad, y, line2, kDim, S(1));

    const float buttonW = S(150), buttonH = S(42), gap = S(16);
    const float buttonsY = by + boxH - pad - buttonH;
    const float firstX = bx + boxW - pad - buttons * buttonW - (buttons - 1) * gap;
    if (in.step)
        g_panel.askChoice = Clamp(g_panel.askChoice + (in.step > 0 ? 1 : -1), 0, buttons - 1);
    g_panel.askChoice = Clamp(g_panel.askChoice, 0, buttons - 1);
    int chosen = -1;
    for (int b = 0; b < buttons; ++b) {
        const float x = firstX + b * (buttonW + gap);
        const bool hot = in.Inside(x, buttonsY, buttonW, buttonH);
        if (hot)
            g_panel.askChoice = b;
        if (hot && in.pressed)
            chosen = b;
        const bool lit = g_panel.askChoice == b;
        RoundedRect(x - 1, buttonsY - 1, buttonW + 2, buttonH + 2, S(6) + 1, lit ? kAccentHot : kBorder);
        RoundedRect(x, buttonsY, buttonW, buttonH, S(6), lit ? kAccent : kPanelSolid);
        Text(kFontSmall, std::round(x + (buttonW - TextWidth(kFontSmall, labels[b], S(2))) * 0.5f),
             buttonsY + std::round((buttonH - LineHeight(kFontSmall)) * 0.5f), labels[b], White(lit ? 1.0f : 0.88f), S(2));
    }
    if (escCancels)
        Text(kFontSmall, bx + pad, buttonsY + std::round((buttonH - LineHeight(kFontSmall)) * 0.5f), "ESC CANCELS", kDim, S(1));
    if (in.activate)
        chosen = g_panel.askChoice;
    // The menu's own cursor, over the question (the menu behind it was drawn without the mouse).
    if (in.inside) {
        Disc(in.mx, in.my, S(6) + 1.5f, Rgba(0, 0, 0, 0.55f));
        Disc(in.mx, in.my, S(5), White(1.0f));
    }
    return chosen;
}

// Turning the dab indicator on asks whether the game's Sticky Sit/Dab is on (Config::dabSticky): its
// settings file can say the old setting for a long while after it's changed (the game writes it only
// now and then), so the answer is asked, not read. Esc never minds (the indicator goes back off).
void BuildStickyQuestion(int screenWidth, int screenHeight, const Input& in, double now)
{
    static const char* const kLabels[2] = { "OFF", "ON" };
    const int chosen = BuildQuestion(screenWidth, screenHeight, in, "DAB INDICATOR",
                                     "Is Sticky Sit/Dab on in MX Simulator's settings?",
                                     "OFF: you sit while you hold the Sit/Dab button.",
                                     "ON: one press sits you, the next stands you up.", kLabels, 2, true);
    if (chosen >= 0) {
        EditConfig().dabSticky = chosen ? 2 : 1;
        g_panel.dirty = true;
        g_panel.askSticky = false;
        SetStatus(chosen ? "DAB INDICATOR ON (STICKY SIT/DAB ON) - SAVE TO KEEP IT"
                         : "DAB INDICATOR ON (STICKY SIT/DAB OFF) - SAVE TO KEEP IT", now);
    }
}

// DEFAULTS asks first (CANCEL lit to start with: Enter by mistake changes nothing); Esc never minds.
void BuildDefaultsQuestion(int screenWidth, int screenHeight, const Input& in, double now)
{
    static const char* const kLabels[2] = { "CANCEL", "RESET" };
    char question[96];
    snprintf(question, sizeof question, "Reset preset %s to its default settings?", PresetLabel(ActivePreset()).c_str());
    const int chosen = BuildQuestion(screenWidth, screenHeight, in, "DEFAULTS", question,
                                     "Every first person, replay, graphics and other setting goes back.",
                                     "Your keybinds stay. Nothing is saved until you press SAVE.", kLabels, 2, true);
    if (chosen == 1) {
        ResetTunables();
        camera::Reset();
        g_panel.dirty = true;
        SetStatus("DEFAULTS RESTORED - SAVE TO KEEP THEM, CANCEL TO UNDO", now);
        Log("panel: preset %d reset to its defaults (not saved yet)", ActivePreset() + 1);
    }
    if (chosen >= 0)
        g_panel.askDefaults = false;
}

// A change that only takes effect once the game restarts (the sound mods): said so, with an OK
// (Esc and Enter close it too).
void BuildRestartNotice(int screenWidth, int screenHeight, const Input& in, double)
{
    static const char* const kLabels[1] = { "OK" };
    const int chosen = BuildQuestion(screenWidth, screenHeight, in, "RESTART NEEDED",
                                     "Restart MX Simulator for this to take effect.", g_panel.restartText,
                                     "Press SAVE first, so the change is still there after the restart.", kLabels, 1, false);
    if (chosen >= 0)
        g_panel.askRestart = false;
}

// The dab indicator (Config::dabIndicator): bottom right, by the game's speedometer, whether your
// rider stands on the pegs (red, a rider standing on the bike) or sits or dabs (green, a rider sitting
// with a foot down), from the game's dab key and settings (dab.cpp).
// ===========================================================================
// A line of width w with round ends.
// Its sides feathered over a pixel (fading to clear), so it's smooth at any angle, and its ends round.
void Segment(float x0, float y0, float x1, float y1, float w, uint32_t color)
{
    const float dx = x1 - x0, dy = y1 - y0, length = std::sqrt(dx * dx + dy * dy);
    if (length > 1e-3f) {
        const float ax = -dy / length, ay = dx / length;  // across the line
        const float core = std::fmax(w - 1.0f, 0.0f) * 0.5f, outer = core + 1.0f;
        const uint32_t clear = color & 0x00FFFFFFu;
        const float u = 4.0f / kAtlasSize;  // the solid block
        const auto Strip = [&](float from, float to, uint32_t c0, uint32_t c1) {
            const Vertex a{ x0 + ax * from, y0 + ay * from, u, u, c0 }, b{ x1 + ax * from, y1 + ay * from, u, u, c0 };
            const Vertex c{ x1 + ax * to, y1 + ay * to, u, u, c1 }, d{ x0 + ax * to, y0 + ay * to, u, u, c1 };
            g_batch.insert(g_batch.end(), { a, b, c, a, c, d });
        };
        if (core > 0)
            Strip(-core, core, color, color);
        Strip(core, outer, color, clear);
        Strip(-core, -outer, color, clear);
    }
    Disc(x0, y0, w * 0.5f, color);
    Disc(x1, y1, w * 0.5f, color);
}

// The icon, in units of a 64 x 56 box (the bike facing right, its wheels on the ground at the bottom).
struct IconLine { float x0, y0, x1, y1, w; };
const IconLine kDabBike[] = {
    { 4, 27, 10, 30, 3.0f },    // rear fender
    { 10, 30, 30, 30, 3.8f },   // seat
    { 30, 30, 42, 25, 3.8f },   // tank
    { 13, 44, 29, 40, 3.2f },   // swingarm
    { 29, 40, 41, 28, 3.2f },   // frame and engine
    { 51, 44, 43, 22, 3.2f },   // fork
    { 43, 22, 40, 18, 3.0f },   // bars
    { 46, 31, 56, 29, 3.0f },   // front fender
};
const float kDabWheels[2][2] = { { 13, 44 }, { 51, 44 } };
constexpr float kDabWheelRadius = 8.5f;
struct IconRider {
    IconLine lines[5];
    float    headX, headY;
};
const IconRider kDabStanding = {
    { { 29, 40, 36, 31, 3.4f }, { 36, 31, 28, 20, 3.4f },   // shin from the peg, thigh
      { 28, 20, 35, 9, 3.4f },                              // torso
      { 35, 9, 42, 13, 3.4f }, { 42, 13, 40, 18, 3.4f } },  // arm to the bars
    38, 4.5f,
};
const IconRider kDabSitting = {
    { { 20, 27, 32, 27, 3.4f }, { 32, 27, 38, 51, 3.4f },   // thigh along the seat, the dabbed leg to the ground
      { 20, 27, 25, 13, 3.4f },                             // torso
      { 25, 13, 33, 16, 3.4f }, { 33, 16, 40, 18, 3.4f } }, // arm to the bars
    27.5f, 8,
};
constexpr float kDabHeadRadius = 4.3f;

void BuildDabIndicator(int width, int height, bool sitting)
{
    const float s = g_atlasScale;
    const auto S = [s](float v) { return std::round(v * s); };
    const uint32_t color = sitting ? Rgba(46, 204, 96, 1.0f) : Rgba(226, 58, 58, 1.0f);
    const uint32_t back = Rgba(13, 17, 26, 1.0f);
    const float unit = 1.15f * s, pad = S(10);
    const float w = std::round(64 * unit) + 2 * pad, h = std::round(56 * unit) + 2 * pad;
    // Bottom right, left of where the game draws its speedometer.
    const float x = std::round(width - S(285) - w), y = std::round(height - S(20) - h);
    RoundedRect(x - S(2), y - S(2), w + S(4), h + S(4), S(8), color);
    RoundedRect(x, y, w, h, S(6), Rgba(13, 17, 26, 0.88f));
    const auto X = [&](float u) { return x + pad + u * unit; };
    const auto Y = [&](float v) { return y + pad + v * unit; };
    for (const auto& wheel : kDabWheels) {
        const int n = 20;
        for (int i = 0; i < n; ++i) {
            const float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
            Segment(X(wheel[0] + kDabWheelRadius * std::cos(a0)), Y(wheel[1] + kDabWheelRadius * std::sin(a0)),
                    X(wheel[0] + kDabWheelRadius * std::cos(a1)), Y(wheel[1] + kDabWheelRadius * std::sin(a1)), 3.0f * unit,
                    color);
        }
    }
    for (const IconLine& l : kDabBike)
        Segment(X(l.x0), Y(l.y0), X(l.x1), Y(l.y1), l.w * unit, color);
    // The rider over the bike, with an outline in the box's colour so it stands apart from it.
    const IconRider& rider = sitting ? kDabSitting : kDabStanding;
    for (int pass = 0; pass < 2; ++pass) {
        const float extra = pass == 0 ? 2.4f : 0.0f;
        const uint32_t c = pass == 0 ? back : color;
        for (const IconLine& l : rider.lines)
            Segment(X(l.x0), Y(l.y0), X(l.x1), Y(l.y1), (l.w + extra) * unit, c);
        Disc(X(rider.headX), Y(rider.headY), (kDabHeadRadius + extra * 0.5f) * unit, c);
    }
}

// Draws the dab indicator over this frame (the menu closed), while you ride or watch.
void DabIndicatorFrame(HWND window)
{
    if (!GetConfig().dabIndicator)
        return;
    double speed = 0;
    const bool haveSpeed = postfx::RiderSpeed(speed);
    dab::Update(haveSpeed, speed);
    const bool sitting = dab::Sitting();
    if (!SceneOnScreen() || GameMenuOverScene())
        return;
    RECT client;
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return;
    const int width = client.right, height = client.bottom;
    const float scale = std::round(Clamp(std::fmin(height / 1080.0f, width / 1440.0f), 0.6f, 2.0f) * 20.0f) / 20.0f;  // (as the menu's)
    SavedState saved;
    SaveState(saved);
    if (EnsureResources(scale)) {
        g_batch.clear();
        g_keepRanges.clear();
        g_clips.clear();
        BuildDabIndicator(width, height, sitting);
        DrawBatch(width, height);
    }
    RestoreState(saved);
}

}  // namespace

bool OverlayListeningForKey()
{
    // And a moment after: the key that answered may still be held. (Typing a preset's name too.)
    return g_panel.listening >= 0 || g_panel.renaming >= 0 || g_panel.searchFocus || (g_panel.listenEnded >= 0 && Seconds() - g_panel.listenEnded < 0.5);
}

bool OverlayPanelOpen() { return g_panel.open; }

// The game's input events as they reach it (Linux: linux/input_linux.cpp; Windows has the window
// procedure above for the same).
bool OverlayTakesInput() { return g_menuOpen.load(std::memory_order_relaxed); }
void OverlayWheel(int notches) { g_wheelDelta += notches * 120; }
void OverlayGameKeyDown(int vk, bool repeat)
{
    g_keyMessages = true;
    if (vk == VK_ESCAPE && !repeat && !g_menuOpen.load(std::memory_order_relaxed))
        ++g_gameEscapes;
}

bool HideGameOverlayNow(bool cursor)
{
    const Config& cfg = GetConfig();
    if (!(cursor ? cfg.hideCursor : cfg.hideHud) || !SceneOnScreen() || GameMenuOverScene())
        return false;
    // The HUD: all the while you ride or watch (this panel open too: turned on, it goes at once).
    if (!cursor)
        return true;
    if (g_menuOpen.load(std::memory_order_relaxed))
        return false;
    return GetTickCount64() - g_lastMouseActivity.load(std::memory_order_relaxed) > kRevealMs;
}

int TakeGameEscapes()
{
    return g_gameEscapes.exchange(0);
}

bool GameCursorPos(float& x, float& y)
{
    const HWND window = g_gameWindow.load(std::memory_order_relaxed);
    POINT p;
    if (!window || !GetCursorPos(&p) || !ScreenToClient(window, &p))
        return false;
    x = static_cast<float>(p.x);
    y = static_cast<float>(p.y);
    return true;
}

void OverlayFrame(HDC dc)
{
    if (!g_glLoaded) {
        g_glLoaded = true;
        g_glAvailable = LoadGL();
        if (!g_glAvailable)
            Log("overlay: required OpenGL functions are missing; settings panel disabled");
    }

    const double now = Seconds();
    const Config& cfg = GetConfig();
    const bool focus = GameHasFocus();
    const HWND window = WindowFromDC(dc);
    g_gameWindow = window;
    NoteMouseActivity();
    UpdateFullscreenWindow(window, cfg.fullscreenWindow);
    UpdateSoundPacks(now);
    // The game's cursor while riding (Config::hideCursor): if it's the system's, the window
    // procedure keeps it hidden; it's put away here the moment hiding starts.
    if (cfg.hideCursor && window && g_glAvailable) {
        SubclassWindow(window);
        static bool wasHidden = false;
        const bool hidden = HideGameOverlayNow(true);
        float mx = 0, my = 0;
        RECT area;
        if (hidden && !wasHidden && focus && GameCursorPos(mx, my) && GetClientRect(window, &area) && mx >= 0 &&
            my >= 0 && mx < area.right && my < area.bottom)
            SetCursor(nullptr);
        wasHidden = hidden;
    }

    // Polled every frame, even while closed, so a key held during the toggle doesn't re-fire.
    const bool menuKey = PollKey(g_keys.menu, cfg.menuKey, false, now);
    const bool escape  = PollKey(g_keys.escape, VK_ESCAPE, false, now);
    // The game's menu (Config::hideHud): Esc presses are counted by the window procedure; if the
    // game's window never gets key messages (input read some other way), from the key itself.
    if (cfg.hideHud && window && g_glAvailable)
        SubclassWindow(window);
    if (escape && focus && !g_panel.open && !g_keyMessages.load(std::memory_order_relaxed))
        ++g_gameEscapes;

    // For automated tests: open the panel without a key press.
    static bool testOpenChecked = false;
    bool testOpen = false;
    if (!testOpenChecked) {
        testOpenChecked = true;
        testOpen = GetEnvironmentVariableA("MXCAM_TEST_OPEN_MENU", nullptr, 0) > 0;
        char tab[8] = "";
        if (GetEnvironmentVariableA("MXCAM_TEST_MENU_TAB", tab, sizeof tab) > 0)
            g_panel.tab = Clamp(atoi(tab), 0, kTabCount - 1);
        char search[64] = "";
        if (GetEnvironmentVariableA("MXCAM_TEST_MENU_SEARCH", search, sizeof search) > 0)
            g_panel.search = search;
        // (Shown with a change made - its CANCEL button - or with the DEFAULTS question up.)
        if (GetEnvironmentVariableA("MXCAM_TEST_MENU_DIRTY", nullptr, 0) > 0)
            g_panel.dirty = true;
        if (GetEnvironmentVariableA("MXCAM_TEST_MENU_ASK_DEFAULTS", nullptr, 0) > 0)
            g_panel.askDefaults = true;
        if (GetEnvironmentVariableA("MXCAM_TEST_MENU_ASK_RESTART", nullptr, 0) > 0) {
            snprintf(g_panel.restartText, sizeof g_panel.restartText, "250F SOUND ON - RESTART THE GAME TO HEAR IT");
            g_panel.askRestart = true;
        }
    }
    // (And scrolled this far down the list, pixels.)
    static const float testScroll = [] {
        char text[16] = "";
        return GetEnvironmentVariableA("MXCAM_TEST_MENU_SCROLL", text, sizeof text) > 0 ? static_cast<float>(atof(text)) : -1.0f;
    }();
    if (testScroll >= 0 && g_panel.open)
        g_panel.scroll = g_panel.scrollTarget = testScroll;

    if (!g_panel.open) {
        if (!window || !g_glAvailable)
            return;
        if (!((menuKey && focus) || testOpen)) {
            DabIndicatorFrame(window);
            StartupHintFrame(window, now);  // (which key opens the menu, for a few seconds after the start)
            return;
        }
        g_hintDone = true;  // (they've found it)
        OpenPanel(window);
    } else if (g_panel.askRestart && focus && escape) {
        g_panel.askRestart = false;  // Esc on the notice: read, the menu stays open
    } else if (g_panel.askDefaults && focus && escape) {
        g_panel.askDefaults = false;  // Esc on the question: nothing reset, the menu stays open
    } else if (g_panel.askSticky && focus && escape) {
        // Esc on the question: never mind (the indicator goes back off), the menu stays open.
        EditConfig().dabIndicator = false;
        g_panel.askSticky = false;
    } else if (g_panel.renaming >= 0 && focus && escape) {
        FinishRename(false, now);  // Esc while renaming: the name stays as it was, the menu open
    } else if (focus && escape && g_panel.listening < 0 && (g_panel.searchFocus || !g_panel.search.empty())) {
        ClearSearch();  // Esc with a search: the search goes, the menu stays
    } else if (g_panel.closeRequest || (focus && g_panel.listening < 0 && g_panel.renaming < 0 && !g_panel.searchFocus &&
                                        !g_panel.askSticky && !g_panel.askDefaults && !g_panel.askRestart &&
                                        (menuKey || escape))) {
        g_panel.askSticky = false;
        ClosePanel();
        return;
    }
    if (!window)
        return;

    RECT client;
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
        return;
    const int width = client.right, height = client.bottom;
    Input in = ReadInput(window, focus, width, height, now);
    UpdateQuery();
    if (g_panel.listening >= 0) {
        // The next key press is the answer, not navigation.
        if (focus)
            ListenForKey(now);
        else
            g_panel.listening = -1;
        in.nav = in.step = 0;
        in.activate = false;
    }
    if (g_panel.renaming >= 0 || g_panel.searchFocus) {
        // Keys type the name (or the search): none of them move around the menu.
        if (g_panel.renaming >= 0) {
            if (!focus || TypeInto(g_panel.nameDraft, kPresetNameMax, now))
                FinishRename(true, now);
        } else if (!focus || TypeInto(g_panel.search, kSearchMax, now)) {
            g_panel.searchFocus = false;  // Enter: done typing, the results stay
        }
        UpdateQuery();
        in.nav = in.step = 0;
        in.activate = in.rename = false;
    }

    // UI scale follows the window height (1080p = 1.0), held down on narrow windows so the panel
    // still fits across, in 5% steps so the atlas isn't rebuilt for every pixel of a resize. And
    // held down so the whole panel fits top to bottom: the tallest tab grew past what 1080 holds at
    // 1.0 (a window 1074 high cut the buttons off), and rounding to a step never takes it back over.
    const float panelHeight = PanelHeightAtScale1();
    float scale = std::round(Clamp(std::fmin(height / std::fmax(1080.0f, panelHeight), width / 1440.0f), 0.6f, 2.0f) * 20.0f) / 20.0f;
    while (scale > 0.6f && scale * panelHeight > height)
        scale -= 0.05f;
    // For automated tests: drawn at this scale whatever the window (to see that a 4K screen's fits).
    static const float testScale = [] {
        char text[16] = "";
        return GetEnvironmentVariableA("MXCAM_TEST_MENU_SCALE", text, sizeof text) > 0 ? static_cast<float>(atof(text)) : 0.0f;
    }();
    if (testScale > 0)
        scale = testScale;

    SavedState saved;
    SaveState(saved);
    if (EnsureResources(scale)) {
        g_batch.clear();
        g_keepRanges.clear();
        g_clips.clear();
        UpdateFade(now);
        if (g_panel.askSticky || g_panel.askDefaults || g_panel.askRestart) {
            // The question takes every click and key; the menu behind it is shown, untouched.
            Input none;
            none.mx = none.my = -1;
            BuildPanel(width, height, none, now);
            ApplyFade();
            if (g_panel.askRestart)
                BuildRestartNotice(width, height, in, now);
            else if (g_panel.askDefaults)
                BuildDefaultsQuestion(width, height, in, now);
            else
                BuildStickyQuestion(width, height, in, now);
        } else {
            BuildPanel(width, height, in, now);
            ApplyFade();
        }
        DrawBatch(width, height);
    }
    RestoreState(saved);
}

}  // namespace mxcam
