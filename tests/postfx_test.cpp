// postfx_test.cpp
//   End-to-end check of the shaders (postfx.cpp) and texture filtering (texfilter.cpp) on a real
//   GL context, through a built MXCam opengl32.dll. It draws a small scene the way MX Simulator
//   does - terrain patches from a program with u_heights / u_meshoffset / u_meshscale, a lit
//   object, then a scaled HUD element - and reads the frame back right after the HUD draw (the
//   effects run just before it).
//
//   The DLL is copied into a folder of its own (under %TEMP%) with an MXS_REMASTERED.ini written for the
//   mode, so the shipped settings are never touched:
//     off       Shaders=0: the picture must be exactly what was drawn
//     neutral   every effect at zero: the round trip through the pipeline must not change it
//     dirt      dirt detail only: the ground gets detail, the object and the sky don't
//     night     TimeOfDay=0: dark ground, a night sky, the HUD untouched, the sun turned
//     snow      WEATHER Snow=1, Snowfall=1: white ground (the object isn't), a grey sky, flakes falling
//     rain      WEATHER Rain=1 at 100%: a dark grey sky, drops falling, the ground darker (wet), puddles that
//               reflect, and the object's shader carrying the wet shine (rainperf / snowperf0: timing)
//     shadows   screen-space sun shadows only: the object shades the ground beside it
//     offscreen shadow maps: a tower behind the camera shades the ground in view
//     stadium   the same under a roof 120 m across: the roof is left out, the ground stays sunny
//     stadiumswap  the roof made in a deleted box's place (another track loaded): still left out
//     hilltree  the offscreen scene with a box 18 m up (a tree on a hill above you): no stadium, so it
//               still casts (stadium / stadiumswap: the same box is left out, as the stands' are);
//               and a see-through puff before the tower whose depth writes come back by glPopAttrib,
//               and after it a draw whose uniform makes the shader discard (a lobby's faded rider)
//     headlight TimeOfDay=0 and the headlight: a lit pool ahead, the rest night
//     headlightup / headlightdown  the same aimed 15 degrees up / 20 down: it reaches further / not as far
//     tires     your bike rolling past on spinning wheels: a rut behind the rear one, mud on the tire
//     tireshidden  the same at 5 m/s for ~3.5 s, its wheels no longer drawn after 1.4 s (as the game
//               doesn't draw them out of view in first person): they must not be lost (MXS_REMASTERED.log)
//     rival     another bike and rider 1 m beside yours (nearer the camera) at your speed, your rear wheel not drawn after 1.5 s: no marks of yours on its track
//     wheeldrift  both wheels in view, learnt; the rear drawn 6 cm low after 1 s: what's learnt stays put
//     backshadow  the bike standing still mid-view, rear wheel never drawn: the back of the bike's
//               stand-in shapes, to look at in frame.bmp
//     gatebox   standing still beside a box (like the start gate) that then isn't drawn: it never casts as yours
//     longshadow  shadow distance 150 m at 8192: a tall tower 30 m behind still shades the ground in view
//     longsharp   150 m at 2048: a thin shadow near the camera still dark and sharp (cascades)
//     oldshadow   an old ini (ShadowDistance=60, ShadowMapSize=4096): read as 18 m at 4096, as before
//     headshadow  first person, no helmet drawn: your head still casts a shadow (a stand-in ball)
//     rearhidden  first person from the start: the rear wheel never drawn, a helmet drawn for 1.4 s
//               only: both still cast shadows (the rear wheel with the front one's model)
//     defaults  the shipped defaults (no ini): no GL errors, GL state left alone
//   Every mode checks that the HUD is drawn after the effects (untouched) and that the GL state
//   the scene had is restored. A .bmp of the frame is saved next to the DLL copy.
//
//   Build from a Developer Command Prompt matching the DLL's bitness:
//     cl /nologo /EHsc /std:c++17 tests\postfx_test.cpp user32.lib gdi32.lib
//   Run:
//     postfx_test.exe bin\x64\Release\opengl32.dll off|neutral|dirt|night|defaults

#include <windows.h>
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

namespace {

#define GL_FUNCTIONS(X) \
    X(wglCreateContext) X(wglMakeCurrent) X(wglDeleteContext) X(wglGetProcAddress) X(glGetError) \
    X(glGetIntegerv) X(glIsEnabled) X(glEnable) X(glDisable) X(glViewport) X(glClearColor)       \
    X(glClear) X(glReadPixels) X(glPixelStorei) X(glFinish) X(glDrawElements) X(glBlendFunc)     \
    X(glDepthMask) X(glPushAttrib) X(glPopAttrib)                                                 \
    X(glGenTextures) X(glBindTexture) X(glTexImage2D) X(glTexParameteri) X(glGetTexParameterfv)  \
    X(glReadBuffer)

#define DECLARE(name) decltype(&::name) p_##name = nullptr;
GL_FUNCTIONS(DECLARE)
#undef DECLARE
BOOL(WINAPI* p_wglSwapBuffers)(HDC) = nullptr;

int g_failures = 0;
void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

using PFN_CreateShader       = GLuint(APIENTRY*)(GLenum);
using PFN_ShaderSource       = void(APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
using PFN_CompileShader      = void(APIENTRY*)(GLuint);
using PFN_CreateProgram      = GLuint(APIENTRY*)();
using PFN_AttachShader       = void(APIENTRY*)(GLuint, GLuint);
using PFN_LinkProgram        = void(APIENTRY*)(GLuint);
using PFN_GetProgramiv       = void(APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_UseProgram         = void(APIENTRY*)(GLuint);
using PFN_GetUniformLocation = GLint(APIENTRY*)(GLuint, const char*);
using PFN_UniformMatrix4fv   = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
using PFN_Uniform3fv         = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
using PFN_Uniform2fv         = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
using PFN_Uniform1f          = void(APIENTRY*)(GLint, GLfloat);
using PFN_Uniform1i          = void(APIENTRY*)(GLint, GLint);
using PFN_UniformMatrix3fv   = void(APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
using PFN_BindAttribLocation = void(APIENTRY*)(GLuint, GLuint, const char*);
using PFN_GenObjects         = void(APIENTRY*)(GLsizei, GLuint*);
using PFN_DeleteObjects      = void(APIENTRY*)(GLsizei, const GLuint*);
using PFN_BindObject         = void(APIENTRY*)(GLuint);
using PFN_BindBuffer         = void(APIENTRY*)(GLenum, GLuint);
using PFN_BufferData         = void(APIENTRY*)(GLenum, ptrdiff_t, const void*, GLenum);
using PFN_VertexAttribPointer = void(APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_ActiveTexture      = void(APIENTRY*)(GLenum);

struct Api {
    PFN_CreateShader CreateShader; PFN_ShaderSource ShaderSource; PFN_CompileShader CompileShader;
    PFN_CreateProgram CreateProgram; PFN_AttachShader AttachShader; PFN_LinkProgram LinkProgram;
    PFN_GetProgramiv GetProgramiv; PFN_UseProgram UseProgram; PFN_GetUniformLocation GetUniformLocation;
    PFN_UniformMatrix4fv UniformMatrix4fv; PFN_Uniform3fv Uniform3fv; PFN_Uniform2fv Uniform2fv;
    PFN_Uniform1f Uniform1f; PFN_BindAttribLocation BindAttribLocation; PFN_GenObjects GenVertexArrays;
    PFN_BindObject BindVertexArray; PFN_GenObjects GenBuffers; PFN_BindBuffer BindBuffer;
    PFN_BufferData BufferData; PFN_VertexAttribPointer VertexAttribPointer;
    PFN_BindObject EnableVertexAttribArray; PFN_ActiveTexture ActiveTexture;
    PFN_Uniform1i Uniform1i; PFN_UniformMatrix3fv UniformMatrix3fv;
    PFN_DeleteObjects DeleteBuffers; PFN_DeleteObjects DeleteVertexArrays;
};
Api gl{};

// MX Simulator's bike shaders, cut down: the lines the mod changes them by (tire dirt) as the game has
// them, so the test goes through the same change.
const char* const kBikeVertex =
    "#version 300 es\n"
    "in vec3 vattr_position;\n"
    "in vec2 vattr_texcoord;\n"
    "uniform int u_nbones;\n"
    "uniform vec3 u_bone_c[32];\n"
    "uniform vec3 u_bone_t[32];\n"
    "uniform mat3 u_bone_r[32];\n"
    "uniform mat4 u_projection;\n"
    "uniform mat4 u_modelview;\n"
    "out vec3 interp_normal, interp_tangent, interp_viewer;\n"
    "out vec2 interp_texcoord;\n"
    "void main()\n"
    "{\n"
    "\tinterp_texcoord = vattr_texcoord;\n"
    "\tinterp_normal = vec3(0.0, 0.0, 1.0);\n"
    "\tinterp_tangent = vec3(1.0, 0.0, 0.0);\n"
    "\tinterp_viewer = (u_modelview * vec4(vattr_position, 1.0)).xyz;\n"
    "\tgl_Position = u_projection * (u_modelview * vec4(vattr_position, 1.0)) + vec4(u_bone_t[0] * 0.0, 0.0) * float(u_nbones);\n"
    "}\n";
const char* const kBikeFragment =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 interp_normal, interp_tangent, interp_viewer;\n"
    "in vec2 interp_texcoord;\n"
    "uniform sampler2D u_tex, u_nmap, u_smap;\n"
    "uniform vec3 u_light_position;\n"
    "uniform vec4 u_ambient;\n"
    "uniform vec3 u_diffuse;\n"
    "uniform float u_shininess;\n"
    "uniform vec3 u_specular_color;\n"
    "out vec4 f_fragcolor;\n"
    "void main()\n"
    "{\n"
    "\tvec4 color, s;\n"
    "\tvec3 n, light;\n"
    "\tfloat ndl, z;\n"
    "\tlight = u_light_position;\n"
    "\tcolor = u_ambient;\n"
    "\tn = normalize(interp_normal);\n"
    "\tndl = dot(n, light);\n"
    "\tif (ndl < 0.0)\n"
    "\t\tndl = 0.0;\n"
    "\tcolor.rgb += u_diffuse.rgb * ndl;\n"
    "\tcolor *= texture(u_tex, interp_texcoord);\n"
    "\ts = texture(u_smap, interp_texcoord);\n"
    "\ts.rgb *= u_specular_color;\n"
    "\ts.a = s.a * 128.0 + 0.125;\n"
    "\tif (s.a >= 127.875)\n"
    "\t\ts.a = u_shininess;\n"
    "\tcolor.rgb += s.rgb * pow(max(dot(reflect(normalize(interp_viewer), n), light), 1e-4), max(s.a, 1.0));\n"
    "\tz = gl_FragCoord.z / gl_FragCoord.w;\n"
    "\tf_fragcolor = color;\n"
    "}\n";

GLuint Build(const char* vertex, const char* fragment, const char* attribute)
{
    const GLuint vs = gl.CreateShader(0x8B31), fs = gl.CreateShader(0x8B30);
    gl.ShaderSource(vs, 1, &vertex, nullptr);
    gl.ShaderSource(fs, 1, &fragment, nullptr);
    gl.CompileShader(vs);
    gl.CompileShader(fs);
    const GLuint program = gl.CreateProgram();
    gl.AttachShader(program, vs);
    gl.AttachShader(program, fs);
    gl.BindAttribLocation(program, 0, attribute);
    gl.LinkProgram(program);
    GLint linked = 0;
    gl.GetProgramiv(program, 0x8B82, &linked);
    return linked ? program : 0;
}

// Column-major helpers.
void Mul(const float a[16], const float b[16], float out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += double(a[k * 4 + r]) * b[c * 4 + k];
            out[c * 4 + r] = float(s);
        }
}

// Camera at eye, looking along -z turned by yawDeg (+ = left), pitched down by pitchDeg.
void MakeView(float eyeX, float eyeY, float eyeZ, float pitchDeg, float yawDeg, float out[16])
{
    const float p = pitchDeg * 3.14159265f / 180.0f, c = std::cos(p), s = std::sin(p);
    const float y = yawDeg * 3.14159265f / 180.0f, cy = std::cos(y), sy = std::sin(y);
    // World -> camera: Rx(+pitch) Ry(-yaw) (the camera itself is turned by Ry(yaw) Rx(-pitch):
    // looking down), after translating by -eye.
    const float r[16] = { 1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1 };
    const float ry[16] = { cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0, 0, 0, 0, 1 };
    const float t[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -eyeX, -eyeY, -eyeZ, 1 };
    float turned[16];
    Mul(r, ry, turned);
    Mul(turned, t, out);
}

struct Pixel { int r, g, b; };

bool WriteBmp(const std::wstring& path, const std::vector<unsigned char>& rgb, int w, int h)
{
    const int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> rows(static_cast<size_t>(stride) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k)
                rows[static_cast<size_t>(y) * stride + x * 3 + k] = rgb[(static_cast<size_t>(y) * w + x) * 3 + 2 - k];
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof info;
    info.biWidth = w;
    info.biHeight = h;
    info.biPlanes = 1;
    info.biBitCount = 24;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof file + sizeof info;
    file.bfSize = file.bfOffBits + static_cast<DWORD>(rows.size());
    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wb") != 0 || !out)
        return false;
    fwrite(&file, sizeof file, 1, out);
    fwrite(&info, sizeof info, 1, out);
    fwrite(rows.data(), 1, rows.size(), out);
    fclose(out);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: postfx_test <opengl32.dll> off|neutral|dirt|night|defaults\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string mode = argv[2];
    // tireslate: the tires scene with the tire dirt off, turned on while riding (frame 6): it must come.
    const bool tiresLate = mode == "tireslate";
    // stadium: the offscreen scene under a roof 120 m across, 3.7 m up (a stadium's stands and roof):
    // left out of the shadow map, the track stays sunny and the tower still casts.
    // stadiumswap: another track loaded mid-run. A small box is drawn first, then its arrays and
    // buffers are deleted and the roof made in their place (the names are given out again): the
    // roof must still be measured as the roof and left out.
    const bool stadiumSwap = mode == "stadiumswap";
    const bool stadium = mode == "stadium" || stadiumSwap;
    const bool hillTree = mode == "hilltree";

    // A folder of our own with the DLL and an ini for the mode.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + L"mxcam_postfx_test_" + std::wstring(mode.begin(), mode.end()) + L"\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t source[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, source, MAX_PATH);
    const std::wstring dll = dir + L"opengl32.dll";
    if (!CopyFileW(source, dll.c_str(), FALSE)) {
        printf("could not copy %s\n", argv[1]);
        return 1;
    }
    const std::wstring ini = dir + L"MXS_REMASTERED.ini";
    DeleteFileW(ini.c_str());
    const char* neutral =
        "[Graphics]\nShaders=1\nTimeOfDay=-1\nShadows=0\nAmbientOcclusion=0\nSunRays=0\nBloom=0\nExposure=0\n"
        "Contrast=0\nSaturation=0\nVignette=0\nAntiAliasing=0\nSharpen=0\nDirtDetail=0\nMotionBlur=0\nDepthOfField=0\n"
        "Headlight=0\nTireMarks=0\nTireDirt=0\n";
    std::string settings, base = neutral;
    if (mode == "off")
        settings = "[Graphics]\nShaders=0\n";
    else if (mode == "neutral")
        settings = base;
    else if (mode == "dirt" || mode == "dirtperf")
        settings = base.replace(base.find("DirtDetail=0"), 12, "DirtDetail=1");
    else if (mode == "dirtultra" || mode == "ultraperf")  // the same, with Ultra graphics
        settings = base.replace(base.find("DirtDetail=0"), 12, "DirtDetail=1\nUltraGraphics=1");
    else if (mode == "night")
        settings = base.replace(base.find("TimeOfDay=-1"), 12, "TimeOfDay=0");
    else if (mode == "snow" || mode == "snowperf")
        settings = base + "[Weather]\nSnow=1\nSnowfall=1\nWind=0.3\n";
    else if (mode == "rain" || mode == "rainperf")
        settings = base + "[Weather]\nRain=1\nRainfall=1\nRainWind=0.3\nWetness=0.8\nPuddles=" +
                   std::string(getenv("MXCAM_TEST_PUDDLES") ? getenv("MXCAM_TEST_PUDDLES") : "0.8") + "\nReflections=1\n";
    else if (mode == "snowperf0")  // timing: the same scene without the weather
        settings = base;
    else if (mode == "shadows")  // screen-space shadows alone
        settings = base.replace(base.find("Shadows=0"), 9, "Shadows=1\nShadowMap=0");
    else if (mode == "offscreen" || stadium || hillTree)  // shadow maps alone (the screen-space ones as short as they go)
        settings = base.replace(base.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1\nShadowLength=1");
    else if (mode == "headlight" || mode == "narrowbeam" || mode == "thirdperson" || mode == "headlightup" ||
             mode == "headlightdown" || mode == "headlightamber" || mode == "headlightrainbow") {
        base.replace(base.find("TimeOfDay=-1"), 12, "TimeOfDay=0");
        settings = base.replace(base.find("Headlight=0"), 11,
                                mode == "narrowbeam"      ? "Headlight=1\nHeadlightBeamDeg=8"
                                : mode == "headlightup"   ? "Headlight=1\nHeadlightBeamDeg=22\nHeadlightAimDeg=15"
                                : mode == "headlightdown" ? "Headlight=1\nHeadlightBeamDeg=22\nHeadlightAimDeg=-20"
                                : mode == "headlightamber" ? "Headlight=1\nHeadlightBeamDeg=22\nHeadlightColor=1"
                                : mode == "headlightrainbow" ? "Headlight=1\nHeadlightBeamDeg=22\nHeadlightColor=2"
                                                          : "Headlight=1\nHeadlightBeamDeg=22");
    }
    else if (mode == "shadowperf" || mode == "shadowperf0")  // timing: 800 objects cast shadow-map shadows
        settings = base.replace(base.find("Shadows=0"), 9, mode == "shadowperf" ? "Shadows=1\nShadowMap=1\nShadowBatch=1"
                                                                                : "Shadows=1\nShadowMap=1\nShadowBatch=0");
    else if (mode == "tires" || tiresLate || mode == "tiresfeet" || mode == "tiresair" || mode == "tireshidden" || mode == "rearhidden" ||
             mode == "gatebox" || mode == "backshadow" || mode == "wheeldrift" || mode == "rival")  // your bike on spinning wheels (metres, feet, in the air, wheels not drawn)
        // (tires: the dirt at 0.75, a player's setting: patchy mud, not a tire covered in it)
        settings = base.replace(base.find("TireMarks=0\nTireDirt=0"), 22, mode == "tires" ? "TireMarks=1\nTireDirt=0.75" : tiresLate ? "TireMarks=1\nTireDirt=0" : "TireMarks=1\nTireDirt=1");
    else if (mode == "longshadow")  // shadow distance far out (150 m, 8192): a tall tower far behind still casts
        settings = base.replace(base.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1\nShadowDistanceM=150\nShadowResolution=2");
    else if (mode == "longsharp")  // 150 m at the lowest resolution: near you the shadows must stay sharp (cascades)
        settings = base.replace(base.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1\nShadowDistanceM=150\nShadowResolution=0");
    else if (mode == "oldshadow")  // an ini from before: ShadowDistance in feet (said m), ShadowMapSize
        settings = base.replace(base.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1\nShadowDistance=60\nShadowMapSize=4096");
    else if (mode == "headshadow")  // first person: your helmet isn't drawn, a ball stands in for it in the shadow map
        settings = base.replace(base.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1");
    else if (mode == "hidehud")  // the HUD hidden while riding at once, shown with the game's menu (Esc)
        settings = base + "[Other]\nHideHud=1\n";
    else if (mode == "dof")
        settings = base.replace(base.find("DepthOfField=0"), 14, "DepthOfField=1\nDofFocus=60");
    else if (mode == "motion")  // the helmet cam off: its smoothing would take most of the turning out
        settings = "[General]\nEnabled=0\n" + base.replace(base.find("MotionBlur=0"), 12, "MotionBlur=1");
    else if (mode != "defaults") {
        printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    if (mode == "tireshidden") {  // and shadow maps: the wheels the game stops drawing still cast
        settings = settings.replace(settings.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1");
        // A rear wheel learnt at a bad moment (a real one: a metre off to the side): thrown away.
        settings += "[Learned]\nRearWheelOnBike=4.200,-1.950,-6.730\n";
    }
    if (mode == "rearhidden" || mode == "gatebox" || mode == "backshadow" || mode == "wheeldrift" || mode == "rival") {  // first person from the start: the wheels learnt in an earlier session
        if (mode != "rival")  // (rival: no shadows, they'd darken the other bike's track like ruts)
            settings = settings.replace(settings.find("Shadows=0"), 9, "Shadows=1\nShadowMap=1");
        settings += "[Learned]\nRearWheelOnBike=0.000,6.102,-0.820\nFrontWheelOnBike=0.000,6.102,-5.676\n"
                    "BikeRider=0.000,7.874,-2.789\n";  // where your rider sits on the test bike
    }
    // MXCAM_TEST_EXTRA_INI: more settings for any mode, to look at it another way (a scene with
    // "[Weather]|Snow=1": '|' for a new line). Not for the checks: they're the mode's own.
    if (const char* extra = getenv("MXCAM_TEST_EXTRA_INI")) {
        std::string more = extra;
        for (char& ch : more)
            if (ch == '|')
                ch = '\n';
        settings += "\n" + more + "\n";
    }
    if (!settings.empty()) {
        FILE* out = nullptr;
        if (_wfopen_s(&out, ini.c_str(), L"wb") == 0 && out) {
            fputs(settings.c_str(), out);
            fclose(out);
        }
    }

    // Whoever is at the desk moving the mouse mustn't count as using the game's menus.
    SetEnvironmentVariableA("MXCAM_TEST_MOUSE_IDLE", "1");
    if (tiresLate)
        SetEnvironmentVariableA("MXCAM_TEST_RELOAD_FRAME", "6");  // the ini read again then (changed at frame 3)
    HMODULE proxy = LoadLibraryW(dll.c_str());
    if (!proxy) {
        printf("could not load the DLL (error %lu)\n", GetLastError());
        return 1;
    }
#define LOAD(name) p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(proxy, #name)); \
    if (!p_##name) { printf("missing export %s\n", #name); return 1; }
    GL_FUNCTIONS(LOAD)
    LOAD(wglSwapBuffers)
#undef LOAD

    const int W = 960, H = 540;
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "MXCamPostfxTest";
    RegisterClassA(&wc);
    RECT frame = { 0, 0, W, H };
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowA(wc.lpszClassName, "MXCam postfx test", WS_OVERLAPPEDWINDOW, 0, 0, frame.right - frame.left,
                                frame.bottom - frame.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = p_wglCreateContext(dc);
    if (!rc || !p_wglMakeCurrent(dc, rc)) {
        printf("no GL context\n");
        return 1;
    }
    // Timing: not held to the screen's refresh (every frame took exactly 1 / refresh otherwise).
    if (mode == "shadowperf" || mode == "shadowperf0" || mode == "snowperf" || mode == "snowperf0" || mode == "dirtperf" ||
        mode == "ultraperf" || mode == "rainperf") {
        using PFN_SwapInterval = BOOL(WINAPI*)(int);
        if (const auto swapInterval = reinterpret_cast<PFN_SwapInterval>(p_wglGetProcAddress("wglSwapIntervalEXT")))
            swapInterval(0);
    }
#define PROC(field, name) gl.field = reinterpret_cast<decltype(gl.field)>(p_wglGetProcAddress(name)); \
    if (!gl.field) { printf("missing %s\n", name); return 1; }
    PROC(CreateShader, "glCreateShader") PROC(ShaderSource, "glShaderSource") PROC(CompileShader, "glCompileShader")
    PROC(CreateProgram, "glCreateProgram") PROC(AttachShader, "glAttachShader") PROC(LinkProgram, "glLinkProgram")
    PROC(GetProgramiv, "glGetProgramiv") PROC(UseProgram, "glUseProgram") PROC(GetUniformLocation, "glGetUniformLocation")
    PROC(UniformMatrix4fv, "glUniformMatrix4fv") PROC(Uniform3fv, "glUniform3fv") PROC(Uniform2fv, "glUniform2fv")
    PROC(Uniform1f, "glUniform1f") PROC(BindAttribLocation, "glBindAttribLocation")
    PROC(Uniform1i, "glUniform1i") PROC(UniformMatrix3fv, "glUniformMatrix3fv")
    PROC(GenVertexArrays, "glGenVertexArrays") PROC(BindVertexArray, "glBindVertexArray") PROC(GenBuffers, "glGenBuffers")
    PROC(BindBuffer, "glBindBuffer") PROC(BufferData, "glBufferData") PROC(VertexAttribPointer, "glVertexAttribPointer")
    PROC(EnableVertexAttribArray, "glEnableVertexAttribArray") PROC(ActiveTexture, "glActiveTexture")
    PROC(DeleteBuffers, "glDeleteBuffers") PROC(DeleteVertexArrays, "glDeleteVertexArrays")
#undef PROC

    printf("postfx test, mode '%s'\n", mode.c_str());
    // The terrain: like MX Simulator's, patches of one unit mesh placed by u_meshoffset / u_meshscale.
    const GLuint terrain = Build(
        "attribute vec2 a_corner;\n"
        "uniform mat4 u_modelview; uniform mat4 u_projection; uniform float u_heights;\n"
        "uniform vec2 u_meshoffset; uniform float u_meshscale;\n"
        "void main() {\n"
        "    vec2 p = u_meshoffset + a_corner * u_meshscale;\n"
        "    gl_Position = u_projection * u_modelview * vec4(p.x, u_heights, p.y, 1.0);\n"
        "}",
        "void main() { gl_FragColor = vec4(0.45, 0.36, 0.27, 1.0); }", "a_corner");
    // Objects (and the HUD): a flat colour, lit by u_light_position so the sun is known.
    const GLuint object = Build(
        "attribute vec3 a_pos;\n"
        "uniform mat4 u_modelview; uniform mat4 u_projection; uniform vec3 u_light_position;\n"
        "varying float v_light;\n"
        "void main() { v_light = u_light_position.x * 0.0001; gl_Position = u_projection * u_modelview * vec4(a_pos, 1.0); }",
        // (A negative colour throws the object away: how a shader can hide a faded rider - hilltree.)
        "uniform vec3 u_color; varying float v_light; void main() { if (u_color.r < 0.0) discard; "
        "gl_FragColor = vec4(u_color + v_light, 1.0); }",
        "a_pos");
    Check(terrain && object, "test shaders compile and link");
    if (!terrain || !object)
        return 1;
    const GLint tMv = gl.GetUniformLocation(terrain, "u_modelview");
    const GLint tProj = gl.GetUniformLocation(terrain, "u_projection");
    gl.GetUniformLocation(terrain, "u_heights");
    const GLint tOffset = gl.GetUniformLocation(terrain, "u_meshoffset");
    const GLint tScale = gl.GetUniformLocation(terrain, "u_meshscale");
    // Skinned meshes, like the game's riders and bikes: v = u_bone_r (p - u_bone_c) + u_bone_t.
    // Only their uniforms matter here (nothing is drawn on screen).
    const GLuint rider = Build(
        "attribute vec3 a_pos; uniform mat4 u_modelview; uniform mat4 u_projection; uniform int u_nbones; uniform float u_solid;\n"
        "uniform vec3 u_bone_c[10]; uniform vec3 u_bone_t[10]; uniform mat3 u_bone_r[10];\n"
        "void main() {\n"
        "  vec3 p = u_bone_r[9] * (a_pos - u_bone_c[9]) + u_bone_t[9] + u_bone_r[0] * u_bone_c[0] + u_bone_t[0];\n"
        "  gl_Position = u_nbones < 0 ? u_projection * u_modelview * vec4(p, 1.0)\n"
        "              : u_nbones == 0 && u_solid > 0.5 ? u_projection * u_modelview * vec4(a_pos, 1.0) : vec4(2.0, 2.0, 2.0, 1.0); }",
        "void main() { gl_FragColor = vec4(1.0); }", "a_pos");
    const GLint rMv = rider ? gl.GetUniformLocation(rider, "u_modelview") : -1;
    const GLint rProj = rider ? gl.GetUniformLocation(rider, "u_projection") : -1;
    const GLint rCount = rider ? gl.GetUniformLocation(rider, "u_nbones") : -1;
    const GLint rCentres = rider ? gl.GetUniformLocation(rider, "u_bone_c") : -1;
    const GLint rBones = rider ? gl.GetUniformLocation(rider, "u_bone_t") : -1;
    const GLint rRotations = rider ? gl.GetUniformLocation(rider, "u_bone_r") : -1;
    const GLint rSolid = rider ? gl.GetUniformLocation(rider, "u_solid") : -1;
    // Your bike's wheels (and the brake disc): drawn by the game's bike shader, which carries the tire dirt.
    const GLuint bike = Build(kBikeVertex, kBikeFragment, "vattr_position");
    Check(bike != 0, "the bike shader (with the tire dirt the mod puts in it) compiles and links");
    const GLint bMv = bike ? gl.GetUniformLocation(bike, "u_modelview") : -1;
    const GLint bProj = bike ? gl.GetUniformLocation(bike, "u_projection") : -1;
    const GLint bCount = bike ? gl.GetUniformLocation(bike, "u_nbones") : -1;
    if (bike) {  // (looked up like the game does: the mod knows its programs by them)
        gl.GetUniformLocation(bike, "u_bone_c");
        gl.GetUniformLocation(bike, "u_bone_t");
        gl.GetUniformLocation(bike, "u_bone_r");
    }
    const GLint bAmbient = bike ? gl.GetUniformLocation(bike, "u_ambient") : -1;
    const GLint bDiffuse = bike ? gl.GetUniformLocation(bike, "u_diffuse") : -1;
    const GLint bTex = bike ? gl.GetUniformLocation(bike, "u_tex") : -1;
    const GLint oMv = gl.GetUniformLocation(object, "u_modelview");
    const GLint oProj = gl.GetUniformLocation(object, "u_projection");
    const GLint oLight = gl.GetUniformLocation(object, "u_light_position");
    const GLint oColor = gl.GetUniformLocation(object, "u_color");

    // Meshes: the unit patch (indices in a buffer, like the game's) and a 1 m cube, and a quad.
    GLuint arrays[8] = {};
    gl.GenVertexArrays(8, arrays);
    const auto mesh = [&](GLuint array, const float* vertices, size_t vertexBytes, int components,
                          const unsigned short* indices, size_t indexBytes, GLuint* made = nullptr) {
        GLuint local[2] = {};
        GLuint* buffers = made ? made : local;
        gl.BindVertexArray(array);
        gl.GenBuffers(2, buffers);
        gl.BindBuffer(0x8892, buffers[0]);
        gl.BufferData(0x8892, static_cast<ptrdiff_t>(vertexBytes), vertices, 0x88E4);
        gl.BindBuffer(0x8893, buffers[1]);
        gl.BufferData(0x8893, static_cast<ptrdiff_t>(indexBytes), indices, 0x88E4);
        gl.VertexAttribPointer(0, components, GL_FLOAT, GL_FALSE, 0, nullptr);
        gl.EnableVertexAttribArray(0);
    };
    const float patch[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    const unsigned short patchIndices[6] = { 0, 1, 2, 2, 1, 3 };
    mesh(arrays[0], patch, sizeof patch, 2, patchIndices, sizeof patchIndices);
    const float cube[24] = { -0.5f, 0, -0.5f,  0.5f, 0, -0.5f,  -0.5f, 1, -0.5f,  0.5f, 1, -0.5f,
                             -0.5f, 0, 0.5f,   0.5f, 0, 0.5f,   -0.5f, 1, 0.5f,   0.5f, 1, 0.5f };
    const unsigned short cubeIndices[36] = { 0, 2, 1, 1, 2, 3,  4, 5, 6, 5, 7, 6,  0, 4, 2, 4, 6, 2,
                                             1, 3, 5, 5, 3, 7,  2, 6, 3, 3, 6, 7,  0, 1, 4, 4, 1, 5 };
    mesh(arrays[1], cube, sizeof cube, 3, cubeIndices, sizeof cubeIndices);
    // A roof (stadium): the cube made 400 units across in its own vertices (a big model, not a
    // scaled matrix: that's how the HUD is told apart).
    float roofVertices[24];
    for (int k = 0; k < 24; ++k)
        roofVertices[k] = (k % 3 == 1) ? cube[k] : cube[k] * 400.0f;
    GLuint roofBuffers[2] = {};
    if (stadiumSwap)  // (the small box first: the roof takes its place at frame 10)
        mesh(arrays[7], cube, sizeof cube, 3, cubeIndices, sizeof cubeIndices, roofBuffers);
    else
        mesh(arrays[7], roofVertices, sizeof roofVertices, 3, cubeIndices, sizeof cubeIndices, roofBuffers);
    const float quad[12] = { -1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0 };
    mesh(arrays[2], quad, sizeof quad, 3, patchIndices, sizeof patchIndices);
    // A wheel: a 2 m square across its axle (its own x), standing in for the rim and tire.
    const float wheelQuad[12] = { 0, -1, -1,  0, 1, -1,  0, -1, 1,  0, 1, 1 };
    mesh(arrays[3], wheelQuad, sizeof wheelQuad, 3, patchIndices, sizeof patchIndices);
    // The same square, 1.5 m off its model's origin (MX Simulator's wheel models have their origin
    // far from the axle: it circles the axle as the wheel turns).
    const float wheelQuadOff[12] = { 0, 0.5f, -1,  0, 2.5f, -1,  0, 0.5f, 1,  0, 2.5f, 1 };
    mesh(arrays[4], wheelQuadOff, sizeof wheelQuadOff, 3, patchIndices, sizeof patchIndices);
    // The same in feet (MX Simulator's unit): 3.28 times as big.
    const float ft = 3.2808f;
    const float wheelQuadFeet[12] = { 0, 0.5f * ft, -ft,  0, 2.5f * ft, -ft,  0, 0.5f * ft, ft,  0, 2.5f * ft, ft };
    mesh(arrays[5], wheelQuadFeet, sizeof wheelQuadFeet, 3, patchIndices, sizeof patchIndices);
    // A brake disc: a small square on the wheel's axle (12 cm each way), 8 cm to its side, its model
    // like the wheel's (origin 1.5 m off the axle).
    const float discQuad[12] = { 0.08f, 1.38f, -0.12f,  0.08f, 1.62f, -0.12f,  0.08f, 1.38f, 0.12f,  0.08f, 1.62f, 0.12f };
    mesh(arrays[6], discQuad, sizeof discQuad, 3, patchIndices, sizeof patchIndices);
    gl.BindVertexArray(0);

    // A texture the game would filter smoothly (for texture filtering), bound on unit 2 as
    // "game state" that the effects must leave alone.
    GLuint gameTexture = 0;
    p_glGenTextures(1, &gameTexture);
    p_glBindTexture(GL_TEXTURE_2D, gameTexture);
    const unsigned char texel[4] = { 255, 255, 255, 255 };
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    // The tire's dark rubber and the disc's steel (as the bike shader samples them).
    GLuint rubberTex = 0, steelTex = 0;
    for (GLuint* tex : { &rubberTex, &steelTex }) {
        p_glGenTextures(1, tex);
        p_glBindTexture(GL_TEXTURE_2D, *tex);
        const unsigned char rubber[4] = { 77, 77, 77, 255 }, steel[4] = { 200, 200, 206, 255 };
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex == &rubberTex ? rubber : steel);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    // 60 degrees vertical field of view, near 0.1, far 1000.
    const float n = 0.1f, f = 1000.0f, cot = 1.0f / std::tan(30.0f * 3.14159265f / 180.0f);
    const float projection[16] = { cot * H / W, 0, 0, 0, 0, cot, 0, 0, 0, 0, (f + n) / (n - f), -1, 0, 0, 2 * f * n / (n - f), 0 };
    float view[16];
    if (mode == "backshadow")  // looking down at the bike from above, to see its shadow
        MakeView(0, 26.0f, 0, 58.0f, 0.0f, view);
    else  // (MXCAM_TEST_CAM_X moves it sideways: the ground's patterns must stay where the world has them)
        MakeView(getenv("MXCAM_TEST_CAM_X") ? static_cast<float>(atof(getenv("MXCAM_TEST_CAM_X"))) : 0.0f, 1.6f,
                 getenv("MXCAM_TEST_CAM_Z") ? static_cast<float>(atof(getenv("MXCAM_TEST_CAM_Z"))) : 0.0f, 12.0f, 0.0f, view);
    // What the game sends (eye space, toward the sun). For the shadow check: from the right, so
    // the box's shadow falls on the ground to its left.
    // For the shadow map check: low, from behind, so a tower behind the camera (never on screen)
    // throws its shadow forward onto the visible ground.
    const bool headShadow = mode == "headshadow";
    const bool farTower = mode == "longshadow" || mode == "oldshadow" || mode == "longsharp";
    const float sunEye[3] = { mode == "shadows" ? 0.8f : mode == "backshadow" ? 0.25f : (mode == "offscreen" || stadium || hillTree) || farTower ? 0.0f : headShadow ? -0.5f : 0.3f,
                              mode == "shadows" ? 0.6f : mode == "backshadow" ? 0.95f : (mode == "offscreen" || stadium || hillTree) || farTower ? 0.15f : headShadow ? 0.17f : 0.8f,
                              mode == "shadows" ? 0.0f : (mode == "offscreen" || stadium || hillTree) || farTower ? 0.99f : headShadow ? 0.83f : 0.5f };
    const float hud[16] = { 0.05f, 0, 0, 0, 0, 0.05f, 0, 0, 0, 0, 0.05f, 0, 0, 0.3f, -1, 1 };  // scaled: the HUD
    const float flare[16] = { 0.05f, 0, 0, 0, 0, 0.05f, 0, 0, 0, 0, 0.05f, 0, -0.3f, 0.3f, -1, 1 };  // the game's lens flare
    const int flareX = static_cast<int>((1.0f - projection[0] * 0.3f) * 0.5f * W);

    std::vector<unsigned char> rgb(static_cast<size_t>(W) * H * 3);
    std::vector<unsigned char> rgbBefore(static_cast<size_t>(W) * H * 3);  // tires: the frame before, for flicker
    std::vector<unsigned char> rgbEach(static_cast<size_t>(W) * H * 3);    // tires: every frame, for the mud's coverage
    double tireCover[16] = {};                                                // tires: muddy share of the rear tire, frames 3-11
    GLint programAfter = 0, activeAfter = 0, unit2After = 0, drawFbAfter = -1, viewportAfter[4] = {};
    bool depthAfter = true, blendAfter = false, cullAfter = true;
    GLenum errorAfter = GL_NO_ERROR;
    // A skinned draw: n bones (centres, positions, rotations) drawn with modelview.
    // Ready to draw with the bike shader: no bones, lit flat (its colour is its texture's), texture on unit 0.
    const auto UseBike = [&](GLuint tex) {
        gl.UseProgram(bike);
        gl.UniformMatrix4fv(bProj, 1, GL_FALSE, projection);
        gl.Uniform1i(bCount, 0);
        const float ambient[4] = { 1, 1, 1, 1 };
        p_wglGetProcAddress ? (void)0 : (void)0;
        using PFN_Uniform4fv = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
        static const auto uniform4 = reinterpret_cast<PFN_Uniform4fv>(p_wglGetProcAddress("glUniform4fv"));
        if (uniform4)
            uniform4(bAmbient, 1, ambient);
        const float none[3] = { 0, 0, 0 };
        gl.Uniform3fv(bDiffuse, 1, none);
        gl.Uniform1i(bTex, 0);
        gl.ActiveTexture(0x84C0);
        p_glBindTexture(GL_TEXTURE_2D, tex);
    };
    int lastBones = -1;
    float lastBone0[3] = {};
    const auto Skinned = [&](const float* modelview, int n, const float* centres, const float* positions,
                             const float* rotations, int indexCount = 6) {
        lastBones = n;
        if (n > 0)
            std::copy(positions, positions + 3, lastBone0);
        gl.UseProgram(rider);
        gl.UniformMatrix4fv(rProj, 1, GL_FALSE, projection);
        gl.UniformMatrix4fv(rMv, 1, GL_FALSE, modelview);
        gl.Uniform1i(rCount, n);
        gl.Uniform3fv(rCentres, n, centres);
        gl.Uniform3fv(rBones, n, positions);
        gl.UniformMatrix3fv(rRotations, n, GL_FALSE, rotations);
        gl.BindVertexArray(arrays[2]);
        p_glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, nullptr);
    };
    // A rider with 10 bones (torso and hips on the midline, arms and legs out to the sides),
    // facing along rotation's -Z, the middle of its bones at (x, y, z). Drawn with the bare view:
    // its bones are world positions.
    const auto Rider = [&](const float rotation[9], float x, float y, float z, float scale = 1.0f, bool torsoPiece = false) {
        const float centres[30] = { 0, 1.3f, 0,  0, 0.95f, 0,  0.25f, 1.2f, -0.1f,  -0.25f, 1.2f, -0.1f,
                                    0.3f, 1.0f, -0.35f,  -0.3f, 1.0f, -0.35f,  0.15f, 0.8f, -0.25f,  -0.15f, 0.8f, -0.25f,
                                    0.18f, 0.45f, -0.3f,  -0.18f, 0.45f, -0.3f };
        float mean[3] = {};
        for (int b = 0; b < 10; ++b)
            for (int k = 0; k < 3; ++k)
                mean[k] += centres[b * 3 + k] / 10.0f;
        float positions[30], rotations[90];
        for (int b = 0; b < 10; ++b) {
            const float c[3] = { (centres[b * 3] - mean[0]) * scale, (centres[b * 3 + 1] - mean[1]) * scale,
                                 (centres[b * 3 + 2] - mean[2]) * scale };
            for (int k = 0; k < 3; ++k)  // rotation (column-major) * c + the middle
                positions[b * 3 + k] = rotation[k] * c[0] + rotation[3 + k] * c[1] + rotation[6 + k] * c[2] +
                                       (k == 0 ? x : k == 1 ? y : z);
            for (int k = 0; k < 9; ++k)
                rotations[b * 9 + k] = rotation[k];
        }
        Skinned(view, 10, centres, positions, rotations);
        // torsoPiece: a second piece of the rider (its torso), with bones of its own in its own order
        // (an arm, the torso, the other arm), drawn with a draw call of its own.
        if (torsoPiece) {
            const int pick[3] = { 2, 0, 3 };
            float c3[9], p3[9], r3[27];
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    c3[i * 3 + k] = centres[pick[i] * 3 + k];
                    p3[i * 3 + k] = positions[pick[i] * 3 + k];
                }
                for (int k = 0; k < 9; ++k)
                    r3[i * 9 + k] = rotations[pick[i] * 9 + k];
            }
            Skinned(view, 3, c3, p3, r3, 3);
        }
    };

    const bool snowTiming = mode == "snowperf" || mode == "snowperf0" ||  // timing: snow at 100% / off,
                            mode == "dirtperf" || mode == "ultraperf" ||  // the dirt detail, without and with Ultra,
                            mode == "rainperf";                           // rain at 100%
    const bool timing = mode == "shadowperf" || mode == "shadowperf0" || snowTiming;
    const bool hiddenWheels = mode == "tireshidden";
    // gatebox: the same bike, standing still for 3.4 s beside a box the game then stops drawing (like
    // the start gate): the box must not be taken for part of your bike.
    const bool gateBox = mode == "gatebox";
    // backshadow: the same bike standing still in the middle of the view, its rear wheel never drawn,
    // the sun from the side: the shapes standing in for the back of the bike, for a look at them.
    const bool backShadow = mode == "backshadow";
    const bool rearHidden = mode == "rearhidden" || gateBox || backShadow;
    // wheeldrift: both wheels in view, where they sit already learnt; after 1 s the rear one is drawn
    // 6 cm lower for good (a long bad moment). What's learnt must not creep after it.
    const bool wheelDrift = mode == "wheeldrift";
    // rival: your bike (where its wheels sit learnt) with another bike 1 m beside it at your speed,
    // its wheels spinning too; after 1.5 s your rear wheel isn't drawn (as in first person). Your
    // tire marks must stay yours: none in the other bike's track.
    const bool rival = mode == "rival";
    const bool hideHud = mode == "hidehud";
    const int frames = timing ? 160 : gateBox || wheelDrift || rival ? 260 : hiddenWheels || rearHidden ? 200 : hideHud ? 330 : headShadow ? 40 : stadiumSwap ? 40 : 12;
    // hidehud: the game's menu over the race, a draw of its own (another texture) right of the HUD.
    // Esc opens it (frames 100-199), Esc closes it; then it's opened and closed without Esc (frames
    // 230-299: a click in it). The HUD and the menu are sampled on the frames below.
    const int hudFrames[5] = { 3, 180, 220, 290, 325 };
    const bool hudShownAt[5] = { false, true, false, true, false };
    Pixel hudAt[5] = {}, menuAt[5] = {};
    GLuint menuTextures[3] = {};  // the menu: three draws of its own (a panel, its text, a button)
    if (hideHud)
        p_glGenTextures(3, menuTextures);
    const int menuX = static_cast<int>((1.0f + projection[0] * 0.3f) * 0.5f * W);
    LARGE_INTEGER frequency, frameStart, frameEnd, runStart;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&runStart);
    double timedMs = 0;
    int timedFrames = 0;
    for (int frameIndex = 0; frameIndex < frames; ++frameIndex) {
        if (stadiumSwap && frameIndex == 10) {  // another track: the box's arrays and buffers go, the roof comes
            const GLuint before[3] = { arrays[7], roofBuffers[0], roofBuffers[1] };
            gl.DeleteVertexArrays(1, &arrays[7]);
            gl.DeleteBuffers(2, roofBuffers);
            gl.GenVertexArrays(1, &arrays[7]);
            mesh(arrays[7], roofVertices, sizeof roofVertices, 3, cubeIndices, sizeof cubeIndices, roofBuffers);
            gl.BindVertexArray(0);
            printf("    the roof made in the box's place: arrays %u -> %u, buffers %u %u -> %u %u (%s)\n", before[0], arrays[7],
                   before[1], before[2], roofBuffers[0], roofBuffers[1],
                   before[0] == arrays[7] && before[1] == roofBuffers[0] ? "the same names" : "other names");
        }
        QueryPerformanceCounter(&frameStart);
        if (tiresLate && frameIndex == 3) {  // the tire dirt turned on in the menu (read again at frame 6)
            std::string on = settings;
            on.replace(on.find("TireDirt=0"), 10, "TireDirt=0.75");
            FILE* out = nullptr;
            if (_wfopen_s(&out, ini.c_str(), L"wb") == 0 && out) {
                fputs(on.c_str(), out);
                fclose(out);
            }
        }
        if (mode == "motion")  // the head turning quickly from side to side
            MakeView(0, 1.6f, 0, 12.0f, frameIndex % 2 ? 1.0f : -1.0f, view);
        // headshadow: for a while the game's camera sits 1.5 m behind your head (a replay, another
        // view), then comes back. Your head's shadow must stay on your shoulders.
        if (headShadow)
            MakeView(0, 1.6f, frameIndex >= 5 && frameIndex < 35 ? 1.5f : 0.0f, 12.0f, 0.0f, view);
        p_glViewport(0, 0, W, H);
        p_glClearColor(0.40f, 0.60f, 0.85f, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        p_glEnable(GL_DEPTH_TEST);
        p_glDisable(GL_BLEND);
        p_glDisable(GL_CULL_FACE);

        // Terrain: an 8 x 8 grid of 25 m patches around the camera. Its first u_modelview is the camera.
        gl.UseProgram(terrain);
        gl.UniformMatrix4fv(tMv, 1, GL_FALSE, view);
        gl.UniformMatrix4fv(tProj, 1, GL_FALSE, projection);
        gl.BindVertexArray(arrays[0]);
        for (int z = -4; z < 4; ++z) {
            for (int x = -4; x < 4; ++x) {
                const float offset[2] = { x * 25.0f, z * 25.0f };
                gl.Uniform2fv(tOffset, 1, offset);
                gl.Uniform1f(tScale, 25.0f);
                p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
            }
        }

        // First person: your rider with the camera in its head (its torso bone 0.65 m under the eye).
        // Its body isn't drawn on screen here (only its bones matter), and no helmet is drawn at all.
        if (headShadow && rider) {
            const float facing[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };
            Rider(facing, 0.0f, 0.565f, -0.2f);
        }

        // Third person: your rider a few metres ahead of the camera, riding away from it.
        if (mode == "thirdperson" && rider) {
            // Like a respawn at the start gate: it drops onto the gate, then sits still. The drop
            // mustn't aim the light at the ground. It faces 30 degrees right, like its bike.
            const float drop = frameIndex < 6 ? (6 - frameIndex) * 0.3f : 0.0f;
            const float turned[9] = { 0.866f, 0, 0.5f,  0, 1, 0,  -0.5f, 0, 0.866f };
            Rider(turned, 0.0f, 0.9f + drop, -2.8f);
        }

        // Third person: your bike under the rider, turned 30 degrees to the right of the camera
        // (its origin sits below the ground, like MX Simulator's bikes, so it isn't seen).
        if (mode == "thirdperson") {
            gl.UseProgram(object);
            gl.UniformMatrix4fv(oProj, 1, GL_FALSE, projection);
            const float bikeModel[16] = { 0.866f, 0, 0.5f, 0,  0, 1, 0, 0,  -0.5f, 0, 0.866f, 0,  -0.425f, -1.5f, -2.064f, 1 };
            float bikeView[16];
            Mul(view, bikeModel, bikeView);
            gl.UniformMatrix4fv(oMv, 1, GL_FALSE, bikeView);
            gl.BindVertexArray(arrays[1]);
            p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        }

        // Tires: your bike crossing the view from left to right, 4.5 m ahead, on spinning wheels,
        // with your rider on it. The bike model faces its own -Z (here: world +X), its origin
        // sits below the ground and its wheels are models of their own, like MX Simulator's.
        if ((mode == "tires" || tiresLate || mode == "tiresfeet" || mode == "tiresair" || hiddenWheels || rearHidden || wheelDrift || rival) && rider) {
            const float u = mode == "tiresfeet" || rearHidden || wheelDrift || rival ? 3.2808f : 1.0f;  // game units per metre
            const float lift = mode == "tiresair" ? 1.2f : 0.0f;    // in the air: no ruts
            // tireshidden: on the clock at 5 m/s; after 1.4 s the wheels aren't drawn any more (the
            // frame and your rider still are).
            LARGE_INTEGER nowCount;
            QueryPerformanceCounter(&nowCount);
            const float elapsed = static_cast<float>(double(nowCount.QuadPart - runStart.QuadPart) / double(frequency.QuadPart));
            const bool wheelsDrawn = !hiddenWheels || elapsed < 1.4f;
            const float travelled = hiddenWheels ? 5.0f * elapsed : gateBox ? 0.0f : backShadow ? 4.5f : rearHidden || wheelDrift || rival ? 3.0f * elapsed : frameIndex * 0.45f;
            // First, like a real track: lots of other skinned objects drawn further away (more than
            // the mod used to keep track of in a frame).
            for (int k = 0; k < 100; ++k) {
                const float place[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  -15.0f + k * 0.3f, 0, -30.0f * u, 1 };
                float placed[16];
                Mul(view, place, placed);
                const float none[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
                Skinned(placed, 0, none, none, none);
            }
            const float x0 = (-5.5f + travelled) * u;
            const float facingX[9] = { 0, 0, 1,  0, 1, 0,  -1, 0, 0 };  // its -Z along world +X
            // rearhidden: your rider moves about on the bike (as riding), and wears a helmet: a rigid
            // draw of the rider's program on its torso bone, drawn for the first 1.4 s only (in first
            // person the game doesn't draw it).
            const float bob = rearHidden ? 0.15f * u * std::sin(elapsed * 9.0f) : 0.0f;
            Rider(facingX, x0 + 0.85f * u, 0.9f * u + lift + bob, -4.5f * u, u, rearHidden && elapsed < 1.4f);
            if (gateBox && elapsed < 3.4f) {  // the box: 0.6 m beside the bike, standing on the ground
                const float boxModel[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x0, 0.0f, -4.5f * u + 0.6f * u, 1 };
                float boxView[16];
                Mul(view, boxModel, boxView);
                gl.UseProgram(rider);
                gl.Uniform1f(rSolid, 1.0f);
                gl.UniformMatrix4fv(rProj, 1, GL_FALSE, projection);
                gl.UniformMatrix4fv(rMv, 1, GL_FALSE, boxView);
                gl.Uniform1i(rCount, 0);
                gl.BindVertexArray(arrays[1]);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            if (rearHidden && !gateBox && !backShadow && elapsed < 1.4f) {
                // Someone alongside at your speed (off the start gate): a box riding next to your bike,
                // drawn by the riders' program. It must never be taken for part of your bike.
                const float alongside[16] = { 0, 0, 1, 0,  0, 1, 0, 0,  -1, 0, 0, 0,  x0 + 0.3f * u, 0.3f * u, -4.5f * u + 0.55f * u, 1 };
                float alongsideView[16];
                Mul(view, alongside, alongsideView);
                gl.UseProgram(rider);
                gl.Uniform1f(rSolid, 1.0f);
                gl.UniformMatrix4fv(rProj, 1, GL_FALSE, projection);
                gl.UniformMatrix4fv(rMv, 1, GL_FALSE, alongsideView);
                gl.Uniform1i(rCount, 0);
                gl.BindVertexArray(arrays[1]);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            if (rearHidden && elapsed < 1.4f) {
                // The torso bone (the rider's first): 0.37 m above the middle of its bones, turned with it.
                const float torsoX = x0 + 0.85f * u, torsoY = 0.9f * u + bob + 0.37f * u, torsoZ = -4.5f * u;
                const float helmet[16] = { 0, 0, 1, 0,  0, 1, 0, 0,  -1, 0, 0, 0,  // a unit cube (about 30 cm in feet)
                                           torsoX, torsoY + 0.45f * u, torsoZ, 1 };
                float helmetView[16];
                Mul(view, helmet, helmetView);
                gl.UseProgram(rider);
                gl.Uniform1f(rSolid, 1.0f);
                gl.UniformMatrix4fv(rProj, 1, GL_FALSE, projection);
                gl.UniformMatrix4fv(rMv, 1, GL_FALSE, helmetView);
                gl.Uniform1i(rCount, 0);
                gl.BindVertexArray(arrays[1]);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            const float angle = -travelled / 0.35f;
            const float c = std::cos(angle), sn = std::sin(angle);
            const float bikeModel[16] = { 0, 0, 1, 0,  0, 1, 0, 0,  -1, 0, 0, 0,  x0, -1.5f * u + lift, -4.5f * u, 1 };
            // The bike: a skinned mesh in a model of its own, its bones in it: the frame and the
            // two wheels (at the axles), which turn as it rolls.
            {
                float bikeView[16];
                Mul(view, bikeModel, bikeView);
                // The wheel bones' own centres sit 1.5 m below their axles and circle them.
                const float bikeCentres[9] = { 0, 1.9f * u, -1.0f * u,  0, 0.36f * u, -0.25f * u,  0, 0.36f * u, -1.73f * u };
                const float bikeBones[9] = { 0, 1.9f * u, -1.0f * u,  0, (1.86f - 1.5f * c) * u, (-0.25f - 1.5f * sn) * u,
                                             0, (1.86f - 1.5f * c) * u, (-1.73f - 1.5f * sn) * u };
                const float bikeRotations[27] = { 1, 0, 0, 0, 1, 0, 0, 0, 1,
                                                  1, 0, 0, 0, c, sn, 0, -sn, c,
                                                  1, 0, 0, 0, c, sn, 0, -sn, c };
                // tireshidden: only the frame is skinned; the wheels are rigid draws of their own, like the game's.
                Skinned(bikeView, wheelsDrawn && !hiddenWheels && !rearHidden && !wheelDrift && !rival ? 3 : 1, bikeCentres, bikeBones, bikeRotations);
            }
            gl.UseProgram(object);
            gl.UniformMatrix4fv(oProj, 1, GL_FALSE, projection);
            float viewModel[16];
            // wheeldrift: another upright part of the bike, its origin 1.45 units along it (like the one
            // the bike was once measured from by mistake: every wheel then 44 cm off), and for three
            // frames the bike's own body not drawn (the moment that let it latch on).
            if (wheelDrift) {
                float partModel[16];
                std::copy(bikeModel, bikeModel + 16, partModel);
                partModel[12] += 1.45f;  // the bike faces world +X
                Mul(view, partModel, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                gl.BindVertexArray(arrays[1]);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            const bool bodyGone = wheelDrift && frameIndex >= 60 && frameIndex < 63;
            Mul(view, bikeModel, viewModel);
            if (!bodyGone)
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
            gl.BindVertexArray(arrays[1]);
            if (!backShadow && !bodyGone)  // (backshadow: only its place is sent, so the ground under it stays in view)
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            // Something that creeps round now and then, off the centreline between the wheels (like
            // the chain or a brake disc): it must not be taken for a wheel. (Feet only: in metres
            // the test cube would be the size of the bike.)
            if (u > 1.5f) {
                const float creep = frameIndex * 0.03f, cc = std::cos(creep), cs = std::sin(creep);
                const float onBike[16] = { 1, 0, 0, 0,  0, cc, cs, 0,  0, -cs, cc, 0,  0.25f * u, 1.9f * u, -1.0f * u, 1 };
                float partModel[16];
                Mul(bikeModel, onBike, partModel);
                Mul(view, partModel, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            const float grey[3] = { 0.3f, 0.3f, 0.3f };
            if ((mode == "tires" || tiresLate) && wheelsDrawn) {
                // tires: the rear wheel's brake disc, drawn first (so it's the part followed as the
                // wheel): the dirt must still go on the tire, not just on the disc.
                UseBike(steelTex);
                // (A model of its own, turned another way round the same axle: not the wheel's transform.)
                const float discC = std::cos(angle + 0.9f), discS = std::sin(angle + 0.9f);
                const float onBike[16] = { 1, 0, 0, 0,  0, discC, discS, 0,  0, -discS, discC, 0,  0, 1.86f - 1.5f * discC, -0.25f - 1.5f * discS, 1 };
                float discModel[16];
                Mul(bikeModel, onBike, discModel);
                Mul(view, discModel, viewModel);
                gl.UniformMatrix4fv(bMv, 1, GL_FALSE, viewModel);
                gl.BindVertexArray(arrays[6]);
                p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
                gl.UseProgram(object);
            }
            gl.Uniform3fv(oColor, 1, grey);
            // rearhidden: from 1.6 s, two other parts spinning on the bike 0.95 units apart (what once
            // passed for your wheels and made every size five times too small).
            if (rearHidden && !gateBox && !backShadow && elapsed > 1.6f) {
                for (int k = 0; k < 2; ++k) {
                    const float axleZ = -2.0f * u - k * 0.95f;
                    const float onBike[16] = { 1, 0, 0, 0,  0, c, sn, 0,  0, -sn, c, 0,  0, 1.86f * u - 0.3f * c, axleZ - 0.3f * sn, 1 };
                    float partModel[16];
                    Mul(bikeModel, onBike, partModel);
                    Mul(view, partModel, viewModel);
                    gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                    gl.BindVertexArray(arrays[1]);
                    p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
                }
            }
            UseBike(rubberTex);
            for (int w = rearHidden || (rival && elapsed > 1.5f) ? 1 : 0; w < 2 && wheelsDrawn && !backShadow && !(rival && elapsed > 1.5f); ++w) {
                // In the bike's space, like MX Simulator's: the rider between the wheels.
                const float axleZ = (w == 0 ? -0.25f : -1.73f) * u;
                // bike * translate(axle) * rotateX(angle) * translate(0, -1.5 m, 0): its origin circles the axle
                const float drop = wheelDrift && w == 0 && elapsed > 1.0f ? 0.06f * u : 0.0f;
                const float onBike[16] = { 1, 0, 0, 0,  0, c, sn, 0,  0, -sn, c, 0,  0, (1.86f - 1.5f * c) * u - drop, axleZ - 1.5f * sn * u, 1 };
                float wheelModel[16];
                Mul(bikeModel, onBike, wheelModel);
                Mul(view, wheelModel, viewModel);
                gl.UniformMatrix4fv(bMv, 1, GL_FALSE, viewModel);
                gl.BindVertexArray(arrays[u > 1.5f ? 5 : 4]);
                p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
            }
            gl.UseProgram(object);
            if (rival) {  // the other bike: 1 m nearer the camera than yours (its rider the nearest to the
                          // camera: in third person that once made it "your" rider), the same wheels, spinning
                float rivalModel[16];
                std::copy(bikeModel, bikeModel + 16, rivalModel);
                rivalModel[14] += 1.0f * u;
                Rider(facingX, x0 + 0.85f * u, 0.9f * u, -3.5f * u, u);
                UseBike(rubberTex);
                for (int w = 0; w < 2; ++w) {
                    const float axleZ = (w == 0 ? -0.25f : -1.73f) * u;
                    const float onBike[16] = { 1, 0, 0, 0,  0, c, sn, 0,  0, -sn, c, 0,  0, (1.86f - 1.5f * c) * u, axleZ - 1.5f * sn * u, 1 };
                    float wheelModel[16];
                    Mul(rivalModel, onBike, wheelModel);
                    Mul(view, wheelModel, viewModel);
                    gl.UniformMatrix4fv(bMv, 1, GL_FALSE, viewModel);
                    gl.BindVertexArray(arrays[5]);
                    p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
                }
                gl.UseProgram(object);
            }
        }

        // Timing: a field of 800 small boxes around the camera, every one a shadow caster.
        if (timing && !snowTiming) {
            gl.UseProgram(object);
            gl.UniformMatrix4fv(oProj, 1, GL_FALSE, projection);
            gl.Uniform3fv(oLight, 1, sunEye);
            gl.BindVertexArray(arrays[1]);
            for (int k = 0; k < 800; ++k) {
                const float bx = -30.0f + (k % 40) * 1.5f, bz = -3.0f - (k / 40) * 1.5f;
                const float box[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  bx, 0, bz, 1 };
                float boxView[16];
                Mul(view, box, boxView);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, boxView);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
        }

        // An object 6 m ahead, 1 m cube, red.
        gl.UseProgram(object);
        gl.UniformMatrix4fv(oProj, 1, GL_FALSE, projection);
        gl.Uniform3fv(oLight, 1, sunEye);
        const float model[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 0, -6.0f, 1 };
        float viewModel[16];
        Mul(view, model, viewModel);
        gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
        const float red[3] = { 0.8f, 0.1f, 0.1f };
        gl.Uniform3fv(oColor, 1, red);
        gl.BindVertexArray(arrays[1]);
        p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        if (farTower) {  // a 40-box tower 100 units behind the camera, 3 right: its shadow reaches far into view
            for (int level = 0; level < 40; ++level) {
                const float place[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3.0f, static_cast<float>(level), 100.0f, 1 };
                Mul(view, place, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
        }
        if (stadium) {  // the roof: one object 400 units across, 12 up
            const float roof[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 12.0f, 0.0f, 1 };
            Mul(view, roof, viewModel);
            gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
            gl.BindVertexArray(arrays[7]);
            // (stadiumswap: the box drawn as another draw call, 30 indices: the roof's is one never measured)
            p_glDrawElements(GL_TRIANGLES, stadiumSwap && frameIndex < 10 ? 30 : 36, GL_UNSIGNED_SHORT, nullptr);
            gl.BindVertexArray(arrays[1]);
        }
        if (stadium || hillTree) {  // a small box 60 units (18 m) up, 20 ahead: all of it well above the camera
            const float high[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 60.0f, -20.0f, 1 };
            Mul(view, high, viewModel);
            gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
            p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        }
        if (mode == "offscreen" || stadium || hillTree) {  // a 2 m tower of two boxes, 0.8 m behind the camera and 1 m right
            if (hillTree) {
                // First a see-through puff whose depth writes are put back by glPopAttrib, not
                // glDepthMask: the tower after it is solid and must still cast.
                p_glPushAttrib(GL_DEPTH_BUFFER_BIT);
                p_glDepthMask(GL_FALSE);
                const float puff[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1.0f, 0.0f, 0.8f, 1 };
                Mul(view, puff, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
                p_glPopAttrib();
            }
            // Drawn with blending on, as MX Simulator draws its solid objects: it must still cast.
            p_glEnable(GL_BLEND);
            p_glBlendFunc(GL_ONE, GL_ZERO);
            for (int level = 0; level < 2; ++level) {
                const float behind[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1.0f, static_cast<float>(level), 0.8f, 1 };
                Mul(view, behind, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            // Kicked-up dirt: a see-through cloud (blended, not writing depth) just as tall, on the
            // other side. It must not cast a solid shadow onto the sunny ground.
            p_glEnable(GL_BLEND);
            p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            p_glDepthMask(GL_FALSE);
            for (int level = 0; level < 2; ++level) {
                const float dust[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1.0f, static_cast<float>(level), 0.8f, 1 };
                Mul(view, dust, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
            p_glDepthMask(GL_TRUE);
            p_glDisable(GL_BLEND);
            if (hillTree) {
                // Last before the HUD, a lobby's faded rider: the same program, a uniform its shader
                // throws everything away with. The tower's held-back cast must still be drawn with
                // the uniforms it was drawn with, not these.
                const float ghostColor[3] = { -1.0f, 0.0f, 0.0f };
                gl.Uniform3fv(oColor, 1, ghostColor);
                const float ghost[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3.0f, 0.0f, -8.0f, 1 };
                Mul(view, ghost, viewModel);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, viewModel);
                p_glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
            }
        }

        // Game state the effects must not disturb.
        gl.ActiveTexture(0x84C0 + 2);
        p_glBindTexture(GL_TEXTURE_2D, gameTexture);
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        p_glDisable(GL_DEPTH_TEST);

        // The HUD: a scaled u_modelview. The effects run right before this draw.
        gl.UniformMatrix4fv(oMv, 1, GL_FALSE, hud);
        const float white[3] = { 1, 1, 1 };
        if (!hillTree)  // (hilltree: the faded rider's uniform still set as the scene ends - its HUD isn't drawn)
            gl.Uniform3fv(oColor, 1, white);
        gl.BindVertexArray(arrays[2]);
        p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        // The game's lens flare: a sprite added onto the picture, left of the HUD.
        p_glBlendFunc(GL_ONE, GL_ONE);
        gl.UniformMatrix4fv(oMv, 1, GL_FALSE, flare);
        const float purple[3] = { 0.35f, 0.2f, 0.5f };
        gl.Uniform3fv(oColor, 1, purple);
        p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (hideHud) {
            if (frameIndex == 100 || frameIndex == 200)
                SendMessageA(window, WM_KEYDOWN, VK_ESCAPE, 1);
            // A HUD text that changed while the menu was open (a timer, a position: a new draw then),
            // drawn again during the race after: it's learnt with the menu's page, but alone it must
            // not count as the menu being open (that once kept the HUD and cursor up all the time).
            if ((frameIndex >= 100 && frameIndex < 230) || frameIndex >= 300) {
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, hud);
                gl.Uniform3fv(oColor, 1, white);
                p_glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr);
            }
            if ((frameIndex >= 100 && frameIndex < 200) || (frameIndex >= 230 && frameIndex < 300)) {
                const float menuMv[16] = { 0.05f, 0, 0, 0, 0, 0.05f, 0, 0, 0, 0, 0.05f, 0, 0.3f, 0.3f, -1, 1 };
                const float green[3] = { 0.1f, 0.9f, 0.1f };
                GLint wasBound = 0;
                gl.ActiveTexture(0x84C0);
                p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &wasBound);
                gl.UniformMatrix4fv(oMv, 1, GL_FALSE, menuMv);
                gl.Uniform3fv(oColor, 1, green);
                for (GLuint t : menuTextures) {
                    p_glBindTexture(GL_TEXTURE_2D, t);
                    p_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
                }
                p_glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(wasBound));
                gl.ActiveTexture(0x84C0 + 2);
            }
            for (int k = 0; k < 5; ++k) {
                if (frameIndex != hudFrames[k])
                    continue;
                unsigned char px[3];
                p_glFinish();
                p_glReadBuffer(GL_BACK);
                p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
                p_glReadPixels(W / 2, H - 1 - 130, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
                hudAt[k] = Pixel{ px[0], px[1], px[2] };
                p_glReadPixels(menuX, H - 1 - 130, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
                menuAt[k] = Pixel{ px[0], px[1], px[2] };
            }
        }

        p_glGetIntegerv(0x8B8D, &programAfter);
        p_glGetIntegerv(0x84E0, &activeAfter);
        p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit2After);
        p_glGetIntegerv(0x8CA6, &drawFbAfter);
        p_glGetIntegerv(GL_VIEWPORT, viewportAfter);
        depthAfter = p_glIsEnabled(GL_DEPTH_TEST) != GL_FALSE;
        blendAfter = p_glIsEnabled(GL_BLEND) != GL_FALSE;
        cullAfter = p_glIsEnabled(GL_CULL_FACE) != GL_FALSE;
        errorAfter = p_glGetError();
        if (errorAfter != GL_NO_ERROR)
            printf("    GL error 0x%X on frame %d\n", errorAfter, frameIndex);

        if ((mode == "tires" || tiresLate) && frameIndex >= 3 && frameIndex <= 11) {
            // How much of the rear tire is muddy this frame (48 places round it, 29 cm from the axle).
            p_glFinish();
            p_glReadBuffer(GL_BACK);
            p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            p_glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, rgbEach.data());
            int muddy = 0, places = 0;
            for (int k = 0; k < 48; ++k) {
                const float a = k * (6.2831853f / 48.0f), axleX = -5.5f + frameIndex * 0.45f + 0.25f;
                const float world[4] = { axleX + 0.29f * std::cos(a), 0.36f + 0.29f * std::sin(a), -4.5f, 1 };
                float eye[4] = {}, clip[4] = {};
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < 4; ++j)
                        eye[r] += view[j * 4 + r] * world[j];
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < 4; ++j)
                        clip[r] += projection[j * 4 + r] * eye[j];
                const int x = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
                const int y = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
                if (x < 0 || x >= W || y < 0 || y >= H)
                    continue;
                const unsigned char* px = &rgbEach[(static_cast<size_t>(H - 1 - y) * W + x) * 3];
                muddy += px[0] > px[2] + 20 ? 1 : 0;
                ++places;
            }
            tireCover[frameIndex] = places ? double(muddy) / places : 0.0;
        }
        if (mode == "tires" && frameIndex == 10) {
            p_glFinish();
            p_glReadBuffer(GL_BACK);
            p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            p_glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, rgbBefore.data());
        }
        if (frameIndex == (hideHud || headShadow || rival || stadiumSwap || (rearHidden && !gateBox) ? frames - 1 : 11)) {
            p_glFinish();
            p_glReadBuffer(GL_BACK);
            p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
            p_glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        }
        gl.ActiveTexture(0x84C0);
        p_glDisable(GL_BLEND);
        p_wglSwapBuffers(dc);
        if (timing) {
            p_glFinish();
            QueryPerformanceCounter(&frameEnd);
            if (frameIndex >= 40) {  // after warming up
                timedMs += 1000.0 * double(frameEnd.QuadPart - frameStart.QuadPart) / double(frequency.QuadPart);
                ++timedFrames;
            }
        } else {
            Sleep(15);
        }
    }

    const auto At = [&](int x, int y) {  // y from the top
        const unsigned char* p = &rgb[(static_cast<size_t>(H - 1 - y) * W + x) * 3];
        return Pixel{ p[0], p[1], p[2] };
    };
    const auto Near = [](Pixel a, Pixel b, int tolerance) {
        return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance;
    };
    const Pixel sky = At(W / 2, 20), ground = At(W / 2, H - 30), box = At(W / 2, 256), hudPixel = At(W / 2, 130);
    // The variation over a block of near ground: flat colour unless the dirt detail is on.
    double sum = 0, sumSq = 0;
    int count = 0;
    for (int y = H - 80; y < H - 20; ++y)
        for (int x = W / 2 - 200; x < W / 2 + 200; ++x) {
            const Pixel p = At(x, y);
            sum += p.r;
            sumSq += double(p.r) * p.r;
            ++count;
        }
    const double mean = sum / count, spread = std::sqrt(std::fmax(sumSq / count - mean * mean, 0.0));
    // And over ground further out (a band a little below the horizon): what Ultra graphics reaches.
    double farSum = 0, farSumSq = 0;
    int farCount = 0;
    for (int y = 188; y < 212; ++y)
        for (int x = 20; x < W / 2 - 80; ++x) {  // (left of the box)
            const Pixel q = At(x, y);
            farSum += q.r;
            farSumSq += double(q.r) * q.r;
            ++farCount;
        }
    const double farMean = farSum / farCount, farSpread = std::sqrt(std::fmax(farSumSq / farCount - farMean * farMean, 0.0));
    printf("    far ground red %.1f +- %.2f\n", farMean, farSpread);
    printf("    sky %d %d %d, ground %d %d %d, box %d %d %d, HUD %d %d %d; near ground red %.1f +- %.2f\n", sky.r, sky.g,
           sky.b, ground.r, ground.g, ground.b, box.r, box.g, box.b, hudPixel.r, hudPixel.g, hudPixel.b, mean, spread);

    const Pixel drawnSky{ 102, 153, 217 }, drawnGround{ 115, 92, 69 }, drawnBox{ 204, 26, 26 };
    if (hideHud) {
        Check(Near(hudPixel, drawnSky, 12), "the HUD is hidden while riding (the sky shows where it was)");
        static const char* const kWhen[5] = { "from the start", "with the game's menu open (Esc)", "once Esc closed the menu",
                                              "with the menu open again (no Esc)", "once the menu closed (no Esc)" };
        for (int k = 0; k < 5; ++k) {
            printf("    frame %d: HUD %d %d %d, menu %d %d %d\n", hudFrames[k], hudAt[k].r, hudAt[k].g, hudAt[k].b, menuAt[k].r,
                   menuAt[k].g, menuAt[k].b);
            const bool white = hudAt[k].r == 255 && hudAt[k].g == 255 && hudAt[k].b == 255;
            Check(hudShownAt[k] ? white : Near(hudAt[k], drawnSky, 12),
                  (std::string("the HUD is ") + (hudShownAt[k] ? "shown " : "hidden ") + kWhen[k]).c_str());
            if (hudShownAt[k])
                Check(menuAt[k].g > 200 && menuAt[k].r < 60, (std::string("the game's menu is drawn ") + kWhen[k]).c_str());
        }
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        Check(log.find("hud: hiding the game's HUD while riding") != std::string::npos, "the log says the HUD is hidden");
    } else if (!hillTree) {  // (hilltree's HUD square is thrown away by the faded rider's uniform)
        Check(hudPixel.r == 255 && hudPixel.g == 255 && hudPixel.b == 255, "the HUD is drawn after the effects, untouched");
    }
    Check(static_cast<GLuint>(programAfter) == object, "the current program is the game's again");
    Check(activeAfter == 0x84C0 + 2 && static_cast<GLuint>(unit2After) == gameTexture, "texture unit and its texture restored");
    Check(drawFbAfter == 0 && viewportAfter[2] == W && viewportAfter[3] == H, "framebuffer and viewport restored");
    Check(!depthAfter && blendAfter && !cullAfter, "depth test / blend / culling restored");
    Check(errorAfter == GL_NO_ERROR, "no GL errors");
    {
        const Pixel flarePixel = At(flareX, 130), beside = At(flareX - 45, 130);
        const bool timeOfDay = mode == "night" || mode == "headlight" || mode == "narrowbeam" || mode == "thirdperson" ||
                               mode == "headlightup" || mode == "headlightdown" || mode == "headlightamber" ||
                               mode == "headlightrainbow";
        printf("    lens flare %d %d %d, beside it %d %d %d\n", flarePixel.r, flarePixel.g, flarePixel.b, beside.r, beside.g, beside.b);
        if (timeOfDay)
            Check(Near(flarePixel, beside, 6), "the game's lens flare is hidden under a time of day of our own");
        else
            Check(flarePixel.r > beside.r + 40 || flarePixel.r == 255, "the game's lens flare is drawn as usual");
    }

    if (mode == "off" || mode == "neutral") {
        const int tolerance = mode == "off" ? 1 : 2;
        Check(Near(sky, drawnSky, tolerance) && Near(ground, drawnGround, tolerance) && Near(box, drawnBox, tolerance) &&
              Near(At(20, 20), drawnSky, tolerance) && Near(At(W - 20, 20), drawnSky, tolerance) &&
              Near(At(20, H - 20), drawnGround, tolerance) && Near(At(W - 20, H - 20), drawnGround, tolerance),
              mode == "off" ? "shaders off: the picture is exactly what was drawn"
                            : "every effect at zero: the pipeline gives the picture back unchanged");
    } else if (mode == "dirt" || mode == "dirtultra") {
        Check(spread > 2.0, "the near ground has detail");
        if (mode == "dirtultra")
            Check(farSpread > 3.0, "Ultra graphics: the ground further out has detail too");
        Check(Near(box, drawnBox, 2), "the object is left alone (the ground mask works)");
        Check(Near(sky, drawnSky, 2), "the sky is left alone");
    } else if (mode == "snow") {
        printf("    ground %d %d %d (drawn %d %d %d), sky %d %d %d, box %d %d %d\n", ground.r, ground.g, ground.b,
               drawnGround.r, drawnGround.g, drawnGround.b, sky.r, sky.g, sky.b, box.r, box.g, box.b);
        Check(ground.r > drawnGround.r + 50 && std::abs(ground.r - ground.b) < 30, "the ground is white with snow");
        Check(box.r > box.g + 60, "the object isn't (the snow stays on the ground)");
        Check(std::abs(sky.b - sky.r) < std::abs(drawnSky.b - drawnSky.r) / 2, "the sky is overcast grey");
        // Flakes: across the sky, pixels clearly lighter than the sky around them.
        int flakes = 0;
        for (int y = 10; y < 110; ++y)
            for (int x = 0; x < W; x += 1) {
                const Pixel q = At(x, y), r = At(x, y > 60 ? y - 8 : y + 8);
                flakes += q.r > r.r + 18 && q.b > r.b + 18 ? 1 : 0;
            }
        printf("    %d snowflake pixels in the sky\n", flakes);
        Check(flakes > 30, "snow is falling (flakes against the sky)");
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        Check(log.find("postfx: snowfall ready") != std::string::npos && log.find("snowfall couldn't be built") == std::string::npos,
              "the snowfall's shaders compile");
        Check(log.find("shader failed") == std::string::npos, "every effect's shader compiles with the snow in it");
    } else if (mode == "rain") {
        printf("    ground %d %d %d (drawn %d %d %d), sky %d %d %d, box %d %d %d\n", ground.r, ground.g, ground.b,
               drawnGround.r, drawnGround.g, drawnGround.b, sky.r, sky.g, sky.b, box.r, box.g, box.b);
        Check(std::abs(sky.b - sky.r) < std::abs(drawnSky.b - drawnSky.r) / 2, "the sky is overcast grey");
        Check(sky.r + sky.g + sky.b < drawnSky.r + drawnSky.g + drawnSky.b, "and darker than a clear one");
        // Drops: across the sky, pixels lighter than the sky a little above or below them.
        int drops = 0;
        for (int y = 10; y < 110; ++y)
            for (int x = 0; x < W; x += 1) {
                const Pixel q = At(x, y), r = At(x, y > 60 ? y - 12 : y + 12);
                drops += q.r > r.r + 6 && q.b > r.b + 6 ? 1 : 0;
            }
        printf("    %d raindrop pixels in the sky\n", drops);
        Check(drops > 30, "rain is falling (streaks against the sky)");
        // The ground: darker where it's soaked, and over the near ground a spread of lighter
        // (reflecting) puddles and darker wet dirt.
        int darker = 0, lighter = 0, count = 0;
        for (int y = 300; y < H - 10; y += 2)
            for (int x = 10; x < W - 10; x += 2) {
                if (x > W / 2 - 120 && x < W / 2 + 120 && y < 360)
                    continue;  // (the box)
                const Pixel q = At(x, y);
                ++count;
                darker += q.r + q.g + q.b < drawnGround.r + drawnGround.g + drawnGround.b - 40 ? 1 : 0;
                lighter += q.r + q.g + q.b > drawnGround.r + drawnGround.g + drawnGround.b - 10 ? 1 : 0;
            }
        printf("    near ground: %d%% darker (wet), %d%% about as light or lighter (puddles reflecting)\n",
               darker * 100 / count, lighter * 100 / count);
        Check(darker * 100 / count > 30, "the ground is darker: wet");
        Check(lighter > 50, "puddles reflect the sky");
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        Check(log.find("postfx: rainfall ready") != std::string::npos && log.find("rainfall couldn't be built") == std::string::npos,
              "the rainfall's shaders compile");
        Check(log.find("carries the wet shine") != std::string::npos, "the bike shader carries the wet shine");
        Check(log.find("shader failed") == std::string::npos, "every effect's shader compiles with the rain in it");
    } else if (mode == "night") {
        // Corners on both sides of the screen's diagonals: every pass must cover the whole window.
        const Pixel skyLeft = At(20, 20), skyRight = At(W - 20, 20), groundLeft = At(20, H - 20), groundRight = At(W - 20, H - 20);
        Check(ground.r < 45 && ground.r < drawnGround.r / 2, "the ground is dark");
        Check(sky.b > sky.r && sky.r < 40 && sky.b < 80, "the sky is a dark night blue");
        Check(skyLeft.r < 40 && skyRight.r < 40 && groundLeft.r < 45 && groundRight.r < 45 && box.r < 100,
              "the whole picture is graded, to every corner");
        // The game's sun was turned: what reached the shader isn't what the game sent.
        using PFN_GetUniformfv = void(APIENTRY*)(GLuint, GLint, GLfloat*);
        const auto getUniform = reinterpret_cast<PFN_GetUniformfv>(p_wglGetProcAddress("glGetUniformfv"));
        GLfloat light[4] = {};
        if (getUniform)
            getUniform(object, oLight, light);
        printf("    light sent (%.2f %.2f %.2f), the shader got (%.2f %.2f %.2f)\n", sunEye[0], sunEye[1], sunEye[2],
               light[0], light[1], light[2]);
        const float length = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
        Check(oLight >= 0 && length > 0.5f &&
                  std::fabs(light[0] - sunEye[0]) + std::fabs(light[1] - sunEye[1]) + std::fabs(light[2] - sunEye[2]) > 0.1f,
              "the time of day moved the game's sun");
    } else if (mode == "dof" || mode == "motion") {
        // Across the middle of the box: a sharp edge goes from ground to red in a pixel or two.
        int partial = 0;
        for (int x = W / 2 - 120; x < W / 2 + 120; ++x) {
            const Pixel p = At(x, 256);
            partial += p.r > 130 && p.r < 190 ? 1 : 0;
        }
        printf("    %d partly red pixels across the box's edges\n", partial);
        Check(partial >= 8, mode == "dof" ? "the box (out of focus) has soft edges" : "the box is smeared by the turning camera");
        Check(hudPixel.r == 255, "the HUD stays sharp");
    } else if (farTower) {
        // The tower's shadow on the ground ahead (3 right), against the same place on the left.
        int px[2] = {}, py[2] = {};
        for (int k = 0; k < 2; ++k) {
            const float world[4] = { k ? -3.0f : 3.0f, 0.0f, -5.0f, 1 };
            float eye[4] = {}, clip[4] = {};
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j)
                    eye[r] += view[j * 4 + r] * world[j];
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j)
                    clip[r] += projection[j * 4 + r] * eye[j];
            px[k] = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
            py[k] = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
        }
        const Pixel shaded = At(px[0], py[0]), sunny = At(px[1], py[1]);
        printf("    far tower's shadow: ground right %d %d %d, left %d %d %d\n", shaded.r, shaded.g, shaded.b, sunny.r, sunny.g, sunny.b);
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        const size_t at = log.find("objects cast shadow-map shadows (");
        printf("    log: %s\n", at == std::string::npos ? "-" : log.substr(at, log.find('\n', at) - at).c_str());
        if (mode == "longshadow") {
            Check(log.find("(150 m around the camera, 8192x8192)") != std::string::npos, "shadow distance 150 m at 8192");
            Check(shaded.r + 25 < sunny.r, "a tall tower far behind you throws its shadow into view");
        } else if (mode == "longsharp") {
            // Across the tower's 1-unit-wide shadow, 5 units ahead: dark in the middle and a narrow
            // edge (one map stretched over 300 m would smear it to a faint grey band).
            int darkest = 255, soft = 0;
            for (int k = 0; k <= 60; ++k) {
                const float world[4] = { 1.5f + k * 0.05f, 0.0f, -5.0f, 1 };
                float eye[4] = {}, clip[4] = {};
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < 4; ++j)
                        eye[r] += view[j * 4 + r] * world[j];
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < 4; ++j)
                        clip[r] += projection[j * 4 + r] * eye[j];
                const int x = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
                const int y = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
                const int red = At(x, y).r;
                darkest = red < darkest ? red : darkest;
                soft += red > 82 && red < 108 ? 1 : 0;  // neither in the shadow nor out of it
            }
            printf("    across the tower's shadow: darkest %d, %d of 61 places on its soft edge\n", darkest, soft);
            Check(log.find("(150 m around the camera, 2048x2048)") != std::string::npos, "shadow distance 150 m at 2048");
            Check(darkest <= 82, "a thin shadow near you is fully dark at 150 m (not smeared out)");
            Check(soft <= 12, "and its edge is sharp");
        } else {
            Check(log.find("(18 m around the camera, 4096x4096)") != std::string::npos,
                  "an old ini's ShadowDistance=60 (feet) and ShadowMapSize=4096 read as before: 18 m at 4096");
        }
    } else if (mode == "offscreen" || stadium || hillTree) {
        std::string log;
        if (FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO)) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        const bool overheadOut = log.find("left out an object all overhead") != std::string::npos;
        if (hillTree) {
            Check(!overheadOut, "no stadium: something all above the camera (a tree up a hill) still casts");
            Check(log.find("has a stadium") == std::string::npos, "and the track isn't taken for a stadium");
        } else if (stadium) {
            Check(log.find("has a stadium") != std::string::npos, "the roof makes the track a stadium");
            Check(overheadOut, "where things all overhead are left out (the stands, the lights)");
        }
        // Ground 3.5 m ahead and 1 m right (in the tower's shadow) against 1 m left (in the sun).
        const Pixel shaded = At(604, 374), sunny = At(356, 374);
        printf("    ground right %d %d %d (tower's shadow), left %d %d %d\n", shaded.r, shaded.g, shaded.b, sunny.r, sunny.g, sunny.b);
        Check(shaded.r + 25 < sunny.r, "something off screen casts a shadow (shadow map)");
        Check(Near(sunny, drawnGround, 3), "ground in the sun stays as it was (see-through dust casts no block)");
        Check(Near(box, drawnBox, 3), "the object in front isn't shadowed by itself");
    } else if (mode == "headlightup" || mode == "headlightdown") {
        // Up the middle of the picture, from the bottom: how far out the beam lights the ground. (Level,
        // the default: the ground at y 210 is ~28, at y 360 ~157, at y 480 ~120.)
        printf("    beam up the middle (aim %s):", mode == "headlightup" ? "+15" : "-20");
        for (int y = H - 10; y >= 200; y -= 10)
            printf(" %d", At(W / 2, y).r);
        printf("\n");
        if (mode == "headlightup") {
            Check(At(W / 2, 210).r > 60, "aimed up, the headlight lights the ground further ahead");
        } else {
            Check(At(W / 2, 360).r < 60, "aimed down, it doesn't reach as far");
            Check(At(W / 2, 480).r > 90, "and still lights the ground right in front of the bike");
        }
    } else if (mode == "headlight" || mode == "headlightamber" || mode == "headlightrainbow") {
        const Pixel pool = At(W / 2, 330), side = At(30, 330), corner = At(W - 20, H - 20);
        printf("    headlight pool %d %d %d, beside it %d %d %d, corner %d %d %d\n", pool.r, pool.g, pool.b, side.r, side.g,
               side.b, corner.r, corner.g, corner.b);
        // (The rainbow's colour depends on the moment: its brightest channel is what lights.)
        const int brightest = pool.r > pool.g ? (pool.r > pool.b ? pool.r : pool.b) : (pool.g > pool.b ? pool.g : pool.b);
        Check((mode == "headlightrainbow" ? brightest : pool.r) > 90 && pool.r + pool.g + pool.b > (side.r + side.g + side.b) * 2,
              "the headlight lights the ground ahead");
        Check(side.r < 45 && corner.r < 45, "outside the beam it stays night");
        Check(sky.r < 40, "the headlight doesn't light the sky");
        // (The ground's own colour is brownish: lit white it reads about 151 119 88.)
        if (mode == "headlightamber")
            Check(pool.b * 10 < pool.r * 3 && pool.g * 10 < pool.r * 7, "an amber headlight lights it amber (blue and green well under red)");
        else if (mode == "headlight")
            Check(pool.b * 2 > pool.r && pool.g * 10 > pool.r * 7, "a white one lights it as it is (not amber)");
    } else if (mode == "narrowbeam") {
        // A narrow beam is skinnier, but still lights the ground from the plate out.
        const Pixel ahead = At(W / 2, 330), closeBy = At(W / 2, H - 40), aside = At(W / 2 + 110, 330);
        printf("    narrow beam: ahead %d, near the bike %d, beside it %d\n", ahead.r, closeBy.r, aside.r);
        Check(ahead.r > 90 && closeBy.r > 90, "a narrow beam still lights the ground from the plate out");
        const Pixel underPlate = At(W / 2, H - 8);
        printf("    right in front of the plate %d\n", underPlate.r);
        Check(underPlate.r > 60, "the light starts right in front of the plate");
        Check(aside.r < 45, "a narrow beam is skinnier");
    } else if (snowTiming) {
        printf("    %s: %.3f ms per frame (%d frames)\n",
               mode == "snowperf" ? "snow at 100%, snowfall 100%" : mode == "snowperf0" ? "snow off"
               : mode == "dirtperf" ? "dirt detail" : mode == "rainperf" ? "rain at 100%, puddles 80%"
               : "dirt detail with Ultra graphics",
               timedMs / timedFrames, timedFrames);
    } else if (timing) {
        printf("    %s: %.3f ms per frame with 800 shadow casters (%d frames)\n",
               mode == "shadowperf" ? "drawn together" : "one at a time", timedMs / timedFrames, timedFrames);
    } else if (mode == "tires" || mode == "tiresfeet" || mode == "tiresair") {
        const float u = mode == "tiresfeet" ? 3.2808f : 1.0f;
        // Where world points land on the screen (y from the top).
        const auto ToPixel = [&](float x, float y, float z, int& px, int& py) {
            const float world[4] = { x, y, z, 1 };
            float eye[4] = {}, clip[4] = {};
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k)
                    eye[r] += view[k * 4 + r] * world[k];
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k)
                    clip[r] += projection[k * 4 + r] * eye[k];
            px = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
            py = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
        };
        int mx = 0, my = 0, bx = 0, by = 0, tx = 0, ty = 0, ox = 0, oy = 0;
        ToPixel(-1.5f * u, 0.0f, -4.5f * u, mx, my);   // on the rear wheel's track, behind the bike
        ToPixel(-1.5f * u, 0.0f, -3.9f * u, bx, by);   // the same, 60 cm closer: untouched ground
        const float rearX = (-5.5f + 11 * 0.45f + 0.25f) * u;
        ToPixel(rearX - 0.27f * u, 0.26f * u, -4.5f * u, tx, ty);  // the lower back of the rear tire
        ToPixel(rearX, (0.36f + 0.62f) * u, -4.5f * u, ox, oy);   // the wheel's square, well above the tire
        const Pixel mark = At(mx, my), beside = At(bx, by), tire = At(tx, ty), above = At(ox, oy);
        printf("    tire track %d %d %d, beside it %d %d %d; tire %d %d %d, above it %d %d %d\n", mark.r, mark.g, mark.b,
               beside.r, beside.g, beside.b, tire.r, tire.g, tire.b, above.r, above.g, above.b);
        if (mode == "tiresair") {  // the same ride 1.2 m up: the wheels turn, nothing presses into the ground
            Check(Near(mark, drawnGround, 3), "no ruts while the bike is in the air");
        } else {
            Check(mark.r + 20 < beside.r, "the rear tire leaves a groove behind it");
            Check(Near(beside, drawnGround, 3), "the ground beside the track is untouched");
            Check(tire.r > tire.b + 25, "the tire is caked with mud");
            // (The test wheel is a 2 m square: higher up it is still the wheel's own draw, which the
            // tire dirt now goes on. On a real bike the wheel model is the wheel.)
            (void)above;
            if (mode == "tires") {
                // The ground right next to the rear tire's contact (12 cm ahead, 6 cm towards the
                // camera) against the same place 1 m further ahead (as far from the front tire's rut,
                // away from the rear tire): ground both, not mud painted around the tire.
                int gx = 0, gy = 0, fx = 0, fy = 0;
                ToPixel(rearX + 0.12f, 0.0f, -4.5f + 0.06f, gx, gy);
                ToPixel(rearX + 1.12f, 0.0f, -4.5f + 0.06f, fx, fy);
                const Pixel next = At(gx, gy), ahead = At(fx, fy);
                printf("    ground next to the rear tire %d %d %d, the same 1 m ahead %d %d %d\n", next.r, next.g, next.b,
                       ahead.r, ahead.g, ahead.b);
                Check(Near(next, ahead, 5), "no square of mud on the ground under the tire");
            }
        }
        if (mode == "tires") {
            // Flicker: the mud on the rear tire at the same places on the wheel, frame 10 and 11 (at
            // 27 m/s it turns ~73 degrees a frame). A pattern that jumps changes there; blurred, not.
            double before = 0, after = 0;
            int samples = 0;
            for (int k = 0; k < 48; ++k) {
                const float a = k * (6.2831853f / 48.0f);
                int x[2], y[2];
                for (int fr = 0; fr < 2; ++fr) {
                    const float axleX = -5.5f + (10 + fr) * 0.45f + 0.25f;
                    ToPixel(axleX + 0.29f * std::cos(a), 0.36f + 0.29f * std::sin(a), -4.5f, x[fr], y[fr]);
                }
                if (x[0] < 0 || x[0] >= W || y[0] < 0 || y[0] >= H || x[1] < 0 || x[1] >= W || y[1] < 0 || y[1] >= H)
                    continue;
                before += rgbBefore[(static_cast<size_t>(H - 1 - y[0]) * W + x[0]) * 3];
                after += At(x[1], y[1]).r;
                ++samples;
            }
            // The mud turns with the tire now (it's on it, like its tread): how much of it there is
            // round the tire must hold steady from one frame to the next.
            const double diff = samples > 0 ? std::fabs(before - after) / samples : 99.0;
            printf("    tire dirt round the tire a frame apart: average red differs by %.1f (%d places)\n", diff, samples);
            Check(samples > 30 && diff < 4.0, "the mud on a fast-turning tire doesn't flicker from frame to frame");
            double low = 1.0, high = 0.0;
            printf("    muddy share of the rear tire, frames 4-11 (from when it's found):");
            for (int f3 = 4; f3 <= 11; ++f3) {
                printf(" %.2f", tireCover[f3]);
                low = std::fmin(low, tireCover[f3]);
                high = std::fmax(high, tireCover[f3]);
            }
            printf("\n");
            Check(low > 0.2 && high - low < 0.3, "the mud stays on the tire every frame (it doesn't come and go)");
        }
    } else if (tiresLate) {
        printf("    muddy share of the rear tire, frames 3-11 (turned on at frame 6):");
        for (int f3 = 3; f3 <= 11; ++f3)
            printf(" %.2f", tireCover[f3]);
        printf("\n");
        Check(tireCover[3] < 0.05 && tireCover[5] < 0.05, "with the tire dirt off, no mud on the tire");
        Check(std::fmin(tireCover[9], std::fmin(tireCover[10], tireCover[11])) > 0.2,
              "turned on while riding, the mud comes on the tire");
    } else if (hiddenWheels) {
        // What the log says about the wheels: found while drawn, and never lost after.
        std::string log;
        // The DLL still has it open for writing: share it.
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        size_t at = 0;
        while ((at = log.find("tire marks:", at)) != std::string::npos) {
            const size_t end = log.find('\n', at);
            printf("    log: %s\n", log.substr(at, end == std::string::npos ? std::string::npos : end - at).c_str());
            at = end;
        }
        for (size_t from = 0; (from = log.find("shadow map:", from)) != std::string::npos;) {
            const size_t end = log.find('\n', from);
            printf("    log: %s\n", log.substr(from, end == std::string::npos ? std::string::npos : end - from).c_str());
            from = end;
        }
        Check(log.find("can't be right; learning it again") != std::string::npos,
              "a wheel learnt off the bike's centreline is thrown away, not used");
        Check(log.find("tire marks: your wheels found") != std::string::npos, "the wheels are found while they're drawn");
        Check(log.find("tire marks: your wheels lost") == std::string::npos,
              "wheels the game stops drawing stay on your bike (not lost)");
        for (const char* wheel : { "rear", "front" }) {
            const std::string kept = std::string("shadow map: kept your ") + wheel + " wheel's draw call";
            const std::string cast = std::string("shadow map: your ") + wheel + " wheel isn't drawn by the game here: a tire and hub";
            Check(log.find(kept) != std::string::npos, (std::string("the ") + wheel + " wheel's draw call is kept while it's drawn").c_str());
            Check(log.find(cast) != std::string::npos, (std::string("the ") + wheel + " wheel still casts a shadow once it isn't drawn").c_str());
        }
    } else if (headShadow) {
        // Where the head's shadow lands: from the middle of the head (about the eye) along the sun
        // down to the ground, onto the screen; and the same spot mirrored to the left (in the sun).
        float sunWorld[3];
        for (int r = 0; r < 3; ++r)  // eye space -> world: the view's rotation, transposed
            sunWorld[r] = view[r * 4] * sunEye[0] + view[r * 4 + 1] * sunEye[1] + view[r * 4 + 2] * sunEye[2];
        const float t = 1.6f / sunWorld[1];
        const float spot[2][3] = { { -sunWorld[0] * t, 0.0f, -sunWorld[2] * t }, { sunWorld[0] * t, 0.0f, -sunWorld[2] * t } };
        int px[2] = {}, py[2] = {};
        for (int k = 0; k < 2; ++k) {
            const float world[4] = { spot[k][0], spot[k][1], spot[k][2], 1 };
            float eye[4] = {}, clip[4] = {};
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j)
                    eye[r] += view[j * 4 + r] * world[j];
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j)
                    clip[r] += projection[j * 4 + r] * eye[j];
            px[k] = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
            py[k] = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
        }
        const Pixel shade = At(px[0], py[0]), sunny = At(px[1], py[1]);
        printf("    head's shadow at (%.1f, %.1f) m: pixel %d,%d is %d %d %d; mirrored %d,%d is %d %d %d\n", spot[0][0],
               spot[0][2], px[0], py[0], shade.r, shade.g, shade.b, px[1], py[1], sunny.r, sunny.g, sunny.b);
        Check(shade.r + 20 < sunny.r, "your head casts a shadow in first person, with no helmet drawn");
        {
            // The torso's: from its upper part (1.05 m up, under the eye; lower down it falls off the
            // bottom of the picture), the same way.
            const float tt = 1.05f / sunWorld[1];
            const float torsoSpot[4] = { -sunWorld[0] * tt, 0.0f, -sunWorld[2] * tt, 1 };
            const float mirrored[4] = { sunWorld[0] * tt, 0.0f, -sunWorld[2] * tt, 1 };
            int tx[2] = {}, ty[2] = {};
            for (int k = 0; k < 2; ++k) {
                const float* world = k == 0 ? torsoSpot : mirrored;
                float eye[4] = {}, clip[4] = {};
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < 4; ++j)
                        eye[r] += view[j * 4 + r] * world[j];
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < 4; ++j)
                        clip[r] += projection[j * 4 + r] * eye[j];
                tx[k] = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
                ty[k] = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
            }
            const bool onScreen = tx[0] >= 0 && tx[0] < W && ty[0] >= 0 && ty[0] < H && tx[1] >= 0 && tx[1] < W && ty[1] >= 0 && ty[1] < H;
            const Pixel torsoShade = onScreen ? At(tx[0], ty[0]) : Pixel{}, torsoSunny = onScreen ? At(tx[1], ty[1]) : Pixel{};
            printf("    torso's shadow at (%.1f, %.1f) m: pixel %d,%d is %d; mirrored %d,%d is %d\n", torsoSpot[0], torsoSpot[2],
                   tx[0], ty[0], torsoShade.r, tx[1], ty[1], torsoSunny.r);
            Check(onScreen && torsoShade.r + 20 < torsoSunny.r, "your torso casts a shadow in first person");
            // From the head down to the torso, every 5 cm: shaded all the way (the head isn't apart).
            int gaps = 0, lightest = 0;
            for (float h = 1.6f; h >= 1.05f; h -= 0.05f) {
                const float th = h / sunWorld[1];
                const float world[4] = { -sunWorld[0] * th, 0.0f, -sunWorld[2] * th, 1 };
                float eye[4] = {}, clip[4] = {};
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < 4; ++j)
                        eye[r] += view[j * 4 + r] * world[j];
                for (int r = 0; r < 4; ++r)
                    for (int j = 0; j < 4; ++j)
                        clip[r] += projection[j * 4 + r] * eye[j];
                const int x = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
                const int y = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
                if (x < 0 || x >= W || y < 0 || y >= H)
                    continue;
                const int r = At(x, y).r;
                lightest = r > lightest ? r : lightest;
                gaps += r + 20 < drawnGround.r ? 0 : 1;
            }
            printf("    head to torso: lightest %d (sunny ground %d), %d gap(s)\n", lightest, drawnGround.r, gaps);
            Check(gaps == 0, "the head's shadow joins the torso's (no gap at the neck)");
        }
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        Check(log.find("a helmet shape stands in for it") != std::string::npos, "the log says a helmet shape stands in");
        Check(log.find("a torso shape on your rider's shoulders and hips stands in for it") != std::string::npos,
              "the log says a torso shape on your rider's own shoulders and hips stands in for your torso");
        // Where the game's arms start, the shoulder joints (the rider's upper arm bones: 25 cm either
        // side, 0.85 m up, 10 cm ahead of the camera here): both in the shadow, so the arms join it.
        for (int k = 0; k < 2; ++k) {
            const float jx = k ? 0.25f : -0.25f, jy = 0.85f, jz = -0.1f;
            const float th = jy / sunWorld[1];
            const float world[4] = { jx - sunWorld[0] * th, 0.0f, jz - sunWorld[2] * th, 1 };
            float eye[4] = {}, clip[4] = {};
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j)
                    eye[r] += view[j * 4 + r] * world[j];
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j)
                    clip[r] += projection[j * 4 + r] * eye[j];
            const int x = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
            const int y = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
            const bool on = x >= 0 && x < W && y >= 0 && y < H;
            const int r = on ? At(x, y).r : -1;
            printf("    %s shoulder's shadow at pixel %d,%d: %d\n", k ? "right" : "left", x, y, r);
            Check(on && r + 20 < drawnGround.r, k ? "the right shoulder (where the arm starts) is in the shadow"
                                                   : "the left shoulder (where the arm starts) is in the shadow");
        }
    } else if (rival) {
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        for (const char* topic : { "tire marks:", "wheel on its own", "found your bike" })
            for (size_t from = 0; (from = log.find(topic, from)) != std::string::npos;) {
                const size_t end = log.find('\n', from);
                printf("    log: %s\n", log.substr(from, end == std::string::npos ? std::string::npos : end - from).c_str());
                from = end;
            }
        // The other bike's track (1 m further away) behind where it is at the end, against untouched
        // ground well off both tracks: no ruts of yours in it.
        const float u = 3.2808f;
        const auto Project = [&](float x, float z, int& px, int& py) {
            const float world[4] = { x, 0.0f, z, 1 };
            float eye[4] = {}, clip[4] = {};
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j)
                    eye[r] += view[j * 4 + r] * world[j];
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < 4; ++j)
                    clip[r] += projection[j * 4 + r] * eye[j];
            px = static_cast<int>((clip[0] / clip[3] * 0.5f + 0.5f) * W);
            py = H - 1 - static_cast<int>((clip[1] / clip[3] * 0.5f + 0.5f) * H);
        };
        // (At the end, both bikes gone past: their tracks in view.)
        int dark[2] = {}, places[2] = {};
        for (int track = 0; track < 2; ++track)
            for (int k = 0; k < 12; ++k) {
                int px = 0, py = 0;
                Project((0.2f + k * 0.3f) * u, (track ? -3.5f : -4.5f) * u, px, py);  // ridden after 1.5 s
                if (px < 0 || px >= W || py < 0 || py >= H)
                    continue;
                const Pixel g = At(px, py);
                dark[track] += g.r + 20 < drawnGround.r ? 1 : 0;
                ++places[track];
            }
        printf("    your track: %d of %d places pressed in; the other bike's: %d of %d\n", dark[0], places[0], dark[1], places[1]);
        Check(places[0] >= 6 && dark[0] >= places[0] / 2, "your tire marks are in your own track");
        Check(places[1] >= 6 && dark[1] == 0, "your tire marks stay yours: none in the other bike's track");
    } else if (wheelDrift) {
        std::string log, saved;
        for (int k = 0; k < 2; ++k) {
            FILE* in = _wfsopen((dir + (k ? L"MXS_REMASTERED.ini" : L"MXS_REMASTERED.log")).c_str(), L"rb", _SH_DENYNO);
            if (!in)
                continue;
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                (k ? saved : log).append(buffer, got);
            fclose(in);
        }
        for (const char* topic : { "tire marks:", "learnt where your", "wheel has been somewhere else" })
            for (size_t from = 0; (from = log.find(topic, from)) != std::string::npos;) {
                const size_t end = log.find('\n', from);
                printf("    log: %s\n", log.substr(from, end == std::string::npos ? std::string::npos : end - from).c_str());
                from = end;
            }
        const size_t at = saved.find("RearWheelOnBike=");
        double v[3] = { 0, 0, 0 };
        if (at != std::string::npos)
            sscanf_s(saved.c_str() + at + 16, "%lf,%lf,%lf", &v[0], &v[1], &v[2]);
        printf("    ini: RearWheelOnBike=%.3f,%.3f,%.3f (learnt: 0.000,6.102,-0.820)\n", v[0], v[1], v[2]);
        Check(log.find("tire marks: your wheels found") != std::string::npos, "the wheels are found");
        Check(std::fabs(v[1] - 6.102) < 0.02 && log.find("learnt where your rear wheel sits") == std::string::npos,
              "where the rear wheel sits doesn't creep after a long bad moment");
        Check(log.find("let go of a part taken for your") == std::string::npos,
              "your bike isn't measured from another part of it (your wheels stay where they sit)");
    } else if (backShadow) {
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        Check(log.find("a swingarm shape stands in for the back of your bike") != std::string::npos,
              "shapes stand in for the back of your bike (see frame.bmp)");
    } else if (gateBox) {
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        for (size_t from = 0; (from = log.find("shadow map:", from)) != std::string::npos;) {
            const size_t end = log.find('\n', from);
            printf("    log: %s\n", log.substr(from, end == std::string::npos ? std::string::npos : end - from).c_str());
            from = end;
        }
        Check(log.find("shadow map: a part of your bike") == std::string::npos &&
                  log.find("shadow map: a part of your rider") == std::string::npos,
              "a box standing beside you while you wait isn't taken for part of your bike");
    } else if (rearHidden) {
        std::string log;
        FILE* in = _wfsopen((dir + L"MXS_REMASTERED.log").c_str(), L"rb", _SH_DENYNO);
        if (in) {
            char buffer[4096];
            size_t got;
            while ((got = fread(buffer, 1, sizeof buffer, in)) > 0)
                log.append(buffer, got);
            fclose(in);
        }
        for (const char* topic : { "shadow map:", "wheel on its own", "tire marks:" })
            for (size_t from = 0; (from = log.find(topic, from)) != std::string::npos;) {
                const size_t end = log.find('\n', from);
                printf("    log: %s\n", log.substr(from, end == std::string::npos ? std::string::npos : end - from).c_str());
                from = end;
            }
        Check(log.find("found your bike's front wheel on its own") != std::string::npos,
              "the front wheel is found on its own (the rear one is never drawn)");
        Check(log.find("shadow map: kept your front wheel's draw call") == std::string::npos,
              "a wheel found on its own doesn't have its draw call kept (it could be the sprocket or a brake disc)");
        Check(log.find("your rear wheel isn't drawn by the game here: a tire and hub stand in for it") != std::string::npos,
              "the rear wheel, never drawn, has a tire and hub standing in for it in the shadow map");
        // (Kept parts on your torso aren't cast any more: in a lobby other bikes' wheels were learnt as
        // your helmet. Your head's shape stands in for it in first person - see headshadow.)
        Check(log.find("shadow map: a part of your rider (on the torso: the helmet)") == std::string::npos,
              "no part kept on your torso is cast (the head's shape stands in for the helmet)");
        Check(log.find("shadow map: a part of your bike") == std::string::npos,
              "something riding alongside at your speed isn't cast as part of your bike");
        Check(log.find("a swingarm shape stands in for the back of your bike") != std::string::npos,
              "a swingarm shape stands in for the back of your bike");
        // (The camera here is 4.5 m from the rider, not at its eyes: the game only leaves your body out
        // in first person. Counted by pieces alone, some riders' gear - drawn in two pieces in every
        // view - read as left out with the camera close behind, and copies of the body were cast into
        // the real one: jagged patches on the hoodie.)
        Check(log.find("a piece of your rider the game doesn't draw here casts its shadow anyway") == std::string::npos,
              "with the camera away from the rider's eyes, no kept body piece is cast in place of the real one");
        Check(log.find("uses bones the game doesn't send here") == std::string::npos, "its bones are all found");
        bool unitOk = true;
        for (size_t from = 0; (from = log.find("units to the metre", from)) != std::string::npos; from += 18) {
            const size_t colon = log.rfind(':', from);
            const double unit = colon != std::string::npos ? atof(log.c_str() + colon + 1) : 0.0;
            printf("    log: ...%.2f units to the metre\n", unit);
            unitOk = unitOk && unit > 2.5;
        }
        Check(unitOk, "two other parts spinning close together aren't taken for your wheels (sizes stay right)");
        using PFN_GetUniformiv = void(APIENTRY*)(GLuint, GLint, GLint*);
        using PFN_GetUniformfv = void(APIENTRY*)(GLuint, GLint, GLfloat*);
        const auto getInt = reinterpret_cast<PFN_GetUniformiv>(p_wglGetProcAddress("glGetUniformiv"));
        const auto getFloat = reinterpret_cast<PFN_GetUniformfv>(p_wglGetProcAddress("glGetUniformfv"));
        GLint bonesNow = -2;
        GLfloat bone0[3] = {};
        if (getInt && getFloat) {
            getInt(rider, rCount, &bonesNow);
            getFloat(rider, rBones, bone0);
        }
        printf("    rider program: %d bones, bone 0 at %.2f %.2f %.2f (the game last sent %d, %.2f %.2f %.2f)\n", bonesNow,
               bone0[0], bone0[1], bone0[2], lastBones, lastBone0[0], lastBone0[1], lastBone0[2]);
        Check(bonesNow == lastBones && std::fabs(bone0[0] - lastBone0[0]) + std::fabs(bone0[1] - lastBone0[1]) +
                  std::fabs(bone0[2] - lastBone0[2]) < 1e-4f,
              "the rider program's bones are the game's again");
    } else if (mode == "thirdperson") {
        // The light starts at the rider's bike, not at the camera behind it.
        // (The bike is turned 30 degrees to the right, so its beam is right of the middle.)
        const Pixel underCamera = At(W / 2, H - 8), pool = At(W / 2 + 150, 330);
        printf("    third person: ground in front of the camera %d, ahead of the rider %d\n", underCamera.r, pool.r);
        Check(underCamera.r < 45, "third person: the camera itself doesn't carry the light");
        Check(pool.r > 60, "third person: the light shines ahead of your bike");
        const Pixel right = At(W / 2 + 150, 330), left = At(W / 2 - 150, 330);
        printf("    third person: right of the middle %d, left %d\n", right.r, left.r);
        Check(right.r > left.r + 30, "third person: the light turns with the bike, not the rider");
    } else if (mode == "shadows") {
        // Ground just left of the box's base (in its shadow) against the same distance to its right.
        int darker = 0;
        for (int dy = 0; dy < 8; ++dy) {
            const Pixel left = At(W / 2 - 60, 296 + dy), right = At(W / 2 + 60, 296 + dy);
            darker += left.r + 10 < right.r ? 1 : 0;
        }
        printf("    beside the box: left %d, right %d\n", At(W / 2 - 60, 300).r, At(W / 2 + 60, 300).r);
        Check(darker >= 6, "the box casts a shadow on the ground, away from the sun");
        Check(Near(At(W / 2 + 200, 400), drawnGround, 3), "open ground in the sun stays as it was");
    } else if (mode == "defaults") {
        Check(Near(box, drawnBox, 40), "the defaults keep the picture recognisable");
    }

    // Texture filtering: the game's bilinear mipmapped texture became trilinear (and anisotropic).
    if (mode == "defaults" || mode == "off") {
        p_glBindTexture(GL_TEXTURE_2D, gameTexture);
        GLfloat filter = 0, anisotropy = 0;
        p_glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
        p_glGetTexParameterfv(GL_TEXTURE_2D, 0x84FE, &anisotropy);
        p_glGetError();
        printf("    game texture: min filter 0x%X, anisotropy %.0f\n", static_cast<int>(filter), anisotropy);
        if (mode == "defaults")
            Check(filter == GL_LINEAR_MIPMAP_LINEAR && anisotropy >= 2.0f, "texture filtering: trilinear and anisotropic");
        else
            Check(filter == GL_LINEAR_MIPMAP_NEAREST && anisotropy <= 1.0f, "shaders off: the game's own filtering");
    }

    WriteBmp(dir + L"frame.bmp", rgb, W, H);
    wprintf(L"    frame saved to %sframe.bmp\n", dir.c_str());
    p_wglMakeCurrent(nullptr, nullptr);
    p_wglDeleteContext(rc);
    printf(g_failures ? "%d check(s) FAILED\n" : "All checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
