// proc_hooks.cpp
//   Wrappers for OpenGL 2.0+ functions. Games don't import these from opengl32.dll; they fetch
//   them at startup through wglGetProcAddress, which hooks.cpp routes through WrapProc().
//
//   This is the path MX Simulator actually renders through. Every object is drawn with a shader
//   whose 'u_modelview' uniform (Config::modelviewUniform) receives View * Model, computed on the
//   CPU and uploaded with glUniformMatrix4fv. Per frame:
//     - the terrain shader (the program that has Config::cameraProgramUniform, 'u_heights') is
//       drawn with Model = identity, so its first u_modelview upload is the camera view matrix;
//       it feeds camera::Update(), which yields the eye-space correction C;
//     - every rigid u_modelview upload is replaced with C * M - this covers the world and the
//       rider's own bike;
//     - the sky is drawn before the terrain with a rotation-only view. That rotation is enough
//       to preview this frame's view orientation (camera::PreviewRotation), and the sky gets
//       only the rotation part of C, so eye offsets never move it. Other rotation-only draws
//       can come first (tens of degrees off the camera), so the one closest to the last camera
//       rotation is taken as the sky; a preview never changes the filter state;
//     - uploads that aren't rigid (HUD elements are scaled down or flattened) or are exactly
//       identity (2D screens) are left alone.
//     - lighting is computed in eye space, so the light direction ('u_light_position',
//       Config::lightUniform) arrives already turned by the game's view. It gets the rotation
//       part of C too, or turning the view would turn the lighting with it;
//     - reflections look up an environment map through 'u_envmat' (eye -> world rotation, the
//       transpose of the view's), which becomes u_envmat * C^T so reflections stay put.
//   - replays are recognised by replay.cpp and skip all of the above: they are passed through
//     untouched, or corrected to its chase or helmet camera (the rider is found through
//     'u_bone_t', its head through the whole skeleton). The game culls the terrain patches
//     behind its own replay camera, so with those cameras the missing ones are drawn when the
//     game is done with the terrain program (terrain.h).
//   - the shaders (postfx.h) are told this frame's camera, the projection, the game's sun and
//     when the terrain pass ends, all independently of the helmet cam being on; and where the
//     HUD starts: the first draw after the 3D scene whose u_modelview is scaled (HUD elements)
//     or that draws without depth testing (2D), so the effects run on the scene alone;
//   - the sun can be turned by the time of day, before the eye-space correction above;
//   - texture filtering (texfilter.h) also covers sampler objects.
//   TraceKey (F11) records a per-frame trace (MXS_REMASTERED_trace.csv) for diagnosing jerks.
//   Uniform names are logged as the game looks them up, and every upload goes into the F10 dump.

#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include <share.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bike.h"
#include "camera.h"
#include "config.h"
#include "brake.h"
#include "dab.h"
#include "gamepad.h"
#include "hooks.h"
#include "lobby_wheels.h"
#include "log.h"
#include "overlay.h"
#include "postfx.h"
#include "replay.h"
#include "terrain.h"
#include "texfilter.h"

namespace mxcam {
namespace {

using GLchar = char;
using PFN_UseProgram              = void (APIENTRY*)(GLuint program);
using PFN_GetUniformLocation      = GLint (APIENTRY*)(GLuint program, const GLchar* name);
using PFN_UniformMatrix4fv        = void (APIENTRY*)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
using PFN_ProgramUniformMatrix4fv = void (APIENTRY*)(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
using PFN_Uniform4fv              = void (APIENTRY*)(GLint location, GLsizei count, const GLfloat* value);
using PFN_Uniform3fv              = PFN_Uniform4fv;
using PFN_UniformMatrix3fv        = PFN_UniformMatrix4fv;
using PFN_Uniform2fv              = PFN_Uniform4fv;
using PFN_Uniform1f               = void (APIENTRY*)(GLint location, GLfloat v0);
using PFN_Uniform1i               = void (APIENTRY*)(GLint location, GLint v0);
using PFN_DrawRangeElements       = void (APIENTRY*)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices);
using PFN_BindVertexArray         = void (APIENTRY*)(GLuint array);
using PFN_BindBuffer              = void (APIENTRY*)(GLenum target, GLuint buffer);
using PFN_DrawElements            = void (APIENTRY*)(GLenum mode, GLsizei count, GLenum type, const void* indices);
using PFN_DrawArrays              = void (APIENTRY*)(GLenum mode, GLint first, GLsizei count);
using PFN_SamplerParameteri       = void (APIENTRY*)(GLuint sampler, GLenum pname, GLint param);
using PFN_SamplerParameterf       = void (APIENTRY*)(GLuint sampler, GLenum pname, GLfloat param);
using PFN_DeleteSamplers          = void (APIENTRY*)(GLsizei count, const GLuint* samplers);
using PFN_ShaderSource            = void (APIENTRY*)(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths);
using PFN_IsEnabled               = GLboolean (APIENTRY*)(GLenum capability);

// Real driver functions, filled in by WrapProc() when the game first asks for them.
// The ARB variants have the same signatures (GLhandleARB is an unsigned int on Windows).
PFN_UseProgram              real_glUseProgram              = nullptr;
PFN_UseProgram              real_glUseProgramObjectARB     = nullptr;
PFN_GetUniformLocation      real_glGetUniformLocation      = nullptr;
PFN_GetUniformLocation      real_glGetUniformLocationARB   = nullptr;
PFN_UniformMatrix4fv        real_glUniformMatrix4fv        = nullptr;
PFN_UniformMatrix4fv        real_glUniformMatrix4fvARB     = nullptr;
PFN_ProgramUniformMatrix4fv real_glProgramUniformMatrix4fv = nullptr;
PFN_Uniform4fv              real_glUniform4fv              = nullptr;
PFN_Uniform4fv              real_glUniform4fvARB           = nullptr;
PFN_Uniform3fv              real_glUniform3fv              = nullptr;
PFN_Uniform3fv              real_glUniform3fvARB           = nullptr;
PFN_UniformMatrix3fv        real_glUniformMatrix3fv        = nullptr;
PFN_UniformMatrix3fv        real_glUniformMatrix3fvARB     = nullptr;
PFN_Uniform2fv              real_glUniform2fv              = nullptr;
PFN_Uniform1f               real_glUniform1f               = nullptr;
PFN_Uniform1i               real_glUniform1i               = nullptr;
PFN_DrawRangeElements       real_glDrawRangeElements       = nullptr;
PFN_BindVertexArray         real_glBindVertexArray         = nullptr;
PFN_BindBuffer              real_glBindBuffer              = nullptr;
PFN_DrawElements            real_glDrawElements            = nullptr;  // OpenGL 1.1 exports, for replaying
PFN_DrawArrays              real_glDrawArrays              = nullptr;  // the terrain's draw calls
PFN_SamplerParameteri       real_glSamplerParameteri       = nullptr;
PFN_SamplerParameterf       real_glSamplerParameterf       = nullptr;
PFN_DeleteSamplers          real_glDeleteSamplers          = nullptr;
PFN_ShaderSource            real_glShaderSource            = nullptr;
PFN_IsEnabled               real_glIsEnabled               = nullptr;  // OpenGL 1.1 export, for the HUD check
using PFN_ActiveTexture         = void (APIENTRY*)(GLenum texture);
using PFN_BufferData            = void (APIENTRY*)(GLenum target, ptrdiff_t size, const void* data, GLenum usage);
using PFN_BufferSubData         = void (APIENTRY*)(GLenum target, ptrdiff_t offset, ptrdiff_t size, const void* data);
using PFN_VertexAttribPointer   = void (APIENTRY*)(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                                   GLsizei stride, const void* pointer);
using PFN_EnableDisable         = void (APIENTRY*)(GLenum capability);
using PFN_BindTexture           = void (APIENTRY*)(GLenum target, GLuint texture);
PFN_ActiveTexture           real_glActiveTexture           = nullptr;
PFN_BufferData              real_glBufferData              = nullptr;
PFN_BufferSubData           real_glBufferSubData           = nullptr;
PFN_VertexAttribPointer     real_glVertexAttribPointer     = nullptr;
using PFN_VertexAttrib4f        = void (APIENTRY*)(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
PFN_VertexAttrib4f          real_glVertexAttrib4f          = nullptr;
// The game's constant vertex attributes (glVertexAttrib4f: the value an attribute holds while its array
// is off). They're not part of a vertex array: whatever the game set last holds for every draw after.
// A draw repeated later into the shadow map (held back, FlushShadowCasters) is given the ones it was
// drawn with: repeated with whatever was set last, every bike's parts and every rider's kept pieces
// came out nowhere - the shadows that vanished in a lobby as soon as you rode off.
constexpr int kConstAttribs = 16;
GLfloat  g_constAttrib[kConstAttribs][4] = {};
unsigned g_constAttribSet = 0;  // bit i: attribute i was given a constant value
using PFN_DeleteObjects         = void (APIENTRY*)(GLsizei count, const GLuint* names);
PFN_DeleteObjects           real_glDeleteBuffers           = nullptr;
PFN_DeleteObjects           real_glDeleteVertexArrays      = nullptr;
using PFN_GetIntegerv           = void (APIENTRY*)(GLenum pname, GLint* data);
using PFN_GetFloatv             = void (APIENTRY*)(GLenum pname, GLfloat* data);
PFN_GetIntegerv             real_glGetIntegerv             = nullptr;  // OpenGL 1.1 export, for the lens flare check

// What each program's u_modelview last held this frame, to tell the 3D scene from the HUD.
enum class ModelviewKind { Scene, Identity, Scaled };
struct ProgramModelview {
    unsigned      frame = 0;
    ModelviewKind kind  = ModelviewKind::Scene;
    GLint         location = -1;     // its u_modelview, and what was last sent to the driver for it
    GLfloat       uploaded[16] = {};
    GLfloat       raw[16] = {};      // what the game sent (before any correction of ours)
    bool          transposed = false;
};
std::unordered_map<GLuint, ProgramModelview> g_programModelview;  // render thread only
// Each program's projection uniform (whatever its name: the one perspective matrices go to) and
// its value, for drawing objects again from the sun (shadow maps) and putting it back.
struct ProgramProjection {
    GLint   location = -1;
    GLfloat value[16] = {};
};
std::unordered_map<GLuint, ProgramProjection> g_programProjection;  // render thread only
// Flat projections (the HUD's), which the perspective-only record above leaves out: where the
// HUD's draws land on screen, for finding the game's cursor (Config::hideCursor).
std::unordered_map<GLuint, ProgramProjection> g_flatProjection;     // render thread only
unsigned g_frameNumber   = 1;
bool     g_shaderViewSet = false;  // postfx has this frame's camera

constexpr int kMaxLookupsLogged = 500;

// The rider's skinned mesh: world-space bone positions (vec3 array). Replays find the rider by it.
constexpr char kBoneUniform[] = "u_bone_t";

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_set<std::string> g_fetched;                       // names already logged
std::unordered_map<unsigned long long, std::string> g_uniforms;  // (program << 32 | location) -> name
std::unordered_set<unsigned long long> g_modelviewUniforms;      // (program, location) of each u_modelview
std::unordered_set<unsigned long long> g_lightUniforms;          // (program, location) of each light direction
std::unordered_set<unsigned long long> g_envUniforms;            // (program, location) of each environment-map matrix
std::unordered_set<unsigned long long> g_boneUniforms;           // (program, location) of each skinned mesh's bone positions
std::unordered_set<GLuint> g_cameraPrograms;                     // programs that draw the terrain
int     g_lookupsLogged = 0;
GLuint  g_program = 0;                                           // program bound by glUseProgram

// Hiding your own torso and head (Config::hideOwnBody). Riders are skinned meshes: per bone,
// v = u_bone_r * (position - u_bone_c) + u_bone_t, blended by weight. The rider has 9 bones
// (forearms, upper arms, shins, thighs) and one on the body's midline that carries the torso,
// the head and the helmet. That bone is shrunk to a speck at the eye: what it alone moves ends up
// behind the near plane, and a vertex it shares with an arm or a leg lands on the line from the
// eye to where the arm or leg puts it, so on screen it stays exactly there and nothing stretches.
// Only your own rider: the one whose torso has the game's eye in its head. Other riders, and
// anything drawn from another viewpoint (3rd person views), keep their bodies.
constexpr char    kBoneCentreUniform[]   = "u_bone_c";
constexpr char    kBoneRotationUniform[] = "u_bone_r";
constexpr char    kBoneCountUniform[]    = "u_nbones";
constexpr int     kMaxBones = 32;
constexpr GLfloat kHiddenBoneScale = 1e-3f;

struct SkinnedProgram {
    GLint   centreLoc = -1, positionLoc = -1, rotationLoc = -1, countLoc = -1, modelviewLoc = -1;
    int     bones = 0;                              // the last u_nbones (0: a rigid draw)
    GLfloat centre[kMaxBones * 3]   = {};           // the game's uploads, as sent
    GLfloat position[kMaxBones * 3] = {};
    GLfloat rotation[kMaxBones * 9] = {};
    int     centres = 0, positions = 0, rotations = 0;
    bool    transposed = false;
    bool    positionArb = false, rotationArb = false;
    GLfloat rawView[16]  = {};                      // the game's last u_modelview
    GLfloat usedView[16] = {};                      // what was uploaded for it (ours, corrected)
    bool    haveView = false;
    bool    positionEdited = false, rotationEdited = false;  // the program holds our arrays, not the game's
    GLfloat hiddenPosition[kMaxBones * 3] = {};     // our arrays (the torso and head hidden), while edited
    GLfloat hiddenRotation[kMaxBones * 9] = {};
};
std::unordered_map<GLuint, SkinnedProgram> g_skinned;

// ---- What the shadow casters' batch needs to know without asking the driver -----------------
constexpr GLenum kArrayBufferTarget = 0x8892;
constexpr GLenum kTexture0Unit      = 0x84C0;
constexpr int    kTrackedUnits      = 4;     // texture units a caster's textures are kept for
GLuint g_arrayBuffer = 0;                    // GL_ARRAY_BUFFER binding
int    g_activeUnit = 0;                     // glActiveTexture - GL_TEXTURE0
GLuint g_boundTextures[16] = {};             // GL_TEXTURE_2D binding per unit
bool   g_depthWrites = true;                 // glDepthMask, as the game left it
bool   g_blending = false;                   // GL_BLEND, as the game left it
std::unordered_map<GLuint, std::vector<GLuint>> g_vaoArrayBuffers;  // vertex arrays -> their buffers
struct BufferUpdates { unsigned lastFrame = 0; int frames = 0; };
struct AttribPointer { GLuint buffer = 0; GLint size = 0; GLenum type = 0; GLsizei stride = 0; uintptr_t offset = 0; };
std::unordered_map<unsigned long long, AttribPointer> g_vaoAttribs;  // (vertex array << 4 | index) -> its pointer
int g_loggedBigCasters = 0;  // objects left out of the shadow map logged (this track)
bool g_stadiumTrack = false;  // this track has drawn something far bigger than a track's props
                              // (a stadium's roof or stands): things all overhead are left out too
std::unordered_map<GLuint, BufferUpdates> g_bufferUpdates;          // buffers rewritten while running

struct DeferredCaster {
    GLuint   program = 0, vao = 0, elementBuffer = 0;
    DrawCall draw;
    GLuint   textures[kTrackedUnits] = {};
    GLint    modelviewLoc = -1, projectionLoc = -1;
    GLint    boneCountLoc = -1;  // a skinned program's u_nbones, set to 0 for this rigid draw
    float    lightModelview[16] = {};
    unsigned cascades = 0;       // which of the shadow map's cascades it reaches (bit i: cascade i)
    int      bones = -1;         // >= 0: a skinned draw, its bones in g_casterBones[bones] (else rigid)
    unsigned vaoGen = 0;         // its vertex array's g_vaoGeneration when kept (for the trace)
    unsigned attribSet = 0;      // the constant vertex attributes it was drawn with (see g_constAttrib)
    GLfloat  attrib[kConstAttribs][4] = {};
    int      uniforms = -1;      // its program's other uniforms as it was drawn: g_uniformSnaps[uniforms]
};
std::vector<DeferredCaster> g_deferredCasters;

// ---- The uniforms a held-back caster was drawn with ------------------------------------------
// A held-back caster is drawn again at the end of the scene, with its program as the game last
// left it. Its matrices, textures, bones and constant attributes were put back; its program's other
// uniforms weren't. In a lobby the last draw before then left one the shader throws everything
// away with: not one of ~900 held-back casters wrote a depth sample (single player: 4.9 million),
// and every shadow but those cast as drawn (within 4 m of you) was gone. So each program's
// single-value uniforms are followed as the game uploads them, each caster keeps a copy of its
// program's, and the replay uploads the ones that differ (the game's are put back after).
struct UniformSlot {
    unsigned char kind = 0;  // 0 none, 1 glUniform1i, 2 glUniform1f, 3 2fv, 4 3fv, 5 4fv
    GLfloat       v[4] = {};
    GLint         i = 0;
    bool          listed = false;  // (in ProgramUniforms::used; not part of the value)
    bool operator==(const UniformSlot& o) const
    {
        return kind == o.kind && i == o.i && v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2] && v[3] == o.v[3];
    }
    bool operator!=(const UniformSlot& o) const { return !(*this == o); }
};
struct ProgramUniforms {
    std::vector<UniformSlot> now;   // as the game uploaded them, by location
    std::vector<GLint>       used;  // locations that hold one
    unsigned version = 1;           // bumped on every upload
    unsigned snapGeneration = 0, snapVersion = 0;
    int      snapIndex = -1;
};
constexpr GLint kMaxTrackedLocation = 1024;
std::unordered_map<GLuint, ProgramUniforms> g_programUniforms;
ProgramUniforms* g_currentUniforms = nullptr;  // g_program's
struct UniformSnap {
    GLuint program = 0;
    std::vector<std::pair<GLint, UniformSlot>> slots;
};
std::vector<UniformSnap> g_uniformSnaps;  // this frame's (DeferredCaster::uniforms)
unsigned g_snapGeneration = 1;             // bumped whenever they're let go

void ClearUniformSnaps()
{
    g_uniformSnaps.clear();
    ++g_snapGeneration;
}

void NoteUniformProgram(GLuint program)
{
    g_currentUniforms = program ? &g_programUniforms[program] : nullptr;
}

// An upload of the game's (the value as it went to GL). Arrays aren't followed: a location given
// several at once (bones, lights) is forgotten, so it's never put back from a single value.
void NoteUniformValue(GLint location, GLsizei count, int kind, const GLfloat* f, GLint i)
{
    if (!g_currentUniforms || location < 0 || location >= kMaxTrackedLocation)
        return;
    ProgramUniforms& u = *g_currentUniforms;
    if (u.now.size() <= static_cast<size_t>(location))
        u.now.resize(static_cast<size_t>(location) + 1);
    UniformSlot& slot = u.now[static_cast<size_t>(location)];
    if (!slot.listed) {
        slot.listed = true;
        u.used.push_back(location);
    }
    if (count != 1 || (!f && kind != 1)) {
        slot.kind = 0;
    } else {
        slot.kind = static_cast<unsigned char>(kind);
        slot.i = i;
        const int n = kind == 3 ? 2 : kind == 4 ? 3 : kind == 5 ? 4 : kind == 2 ? 1 : 0;
        for (int k = 0; k < 4; ++k)
            slot.v[k] = k < n ? f[k] : 0.0f;
    }
    ++u.version;
}

// g_program's uniforms as they stand: the index of a copy (shared by every caster drawn while they
// stay the same), or -1.
int SnapshotUniforms()
{
    if (!g_currentUniforms)
        return -1;
    ProgramUniforms& u = *g_currentUniforms;
    if (u.snapGeneration == g_snapGeneration && u.snapVersion == u.version && u.snapIndex >= 0)
        return u.snapIndex;
    UniformSnap snap;
    snap.program = g_program;
    snap.slots.reserve(u.used.size());
    for (GLint location : u.used)
        if (u.now[static_cast<size_t>(location)].kind != 0)
            snap.slots.emplace_back(location, u.now[static_cast<size_t>(location)]);
    g_uniformSnaps.push_back(std::move(snap));
    u.snapGeneration = g_snapGeneration;
    u.snapVersion = u.version;
    u.snapIndex = static_cast<int>(g_uniformSnaps.size()) - 1;
    return u.snapIndex;
}
struct CasterBones {
    int     count = 0;
    bool    transposed = false;
    GLfloat centre[kMaxBones * 3] = {}, position[kMaxBones * 3] = {}, rotation[kMaxBones * 9] = {};
};
std::vector<CasterBones> g_casterBones;  // this frame's, for the skinned held-back casters

// Your rider in the shadow map. In first person the game draws fewer pieces of your rider (a capture:
// 3 skinned draws against 8 in third person; the torso among those left out). Each skinned draw of
// your rider is kept while the game makes it; one it leaves out is cast with your rider's bones this
// frame, found by their rest positions (u_bone_c: the same bone has the same one in every piece).
struct RiderDraw {
    DeferredCaster caster;
    CasterBones    bones;
    GLfloat        raw[16];  // its u_modelview as the game sent it
};
std::vector<RiderDraw> g_riderDraws;  // this frame's skinned draws near the camera
constexpr size_t kMaxRiderDraws = 64;
struct KeptRiderPiece {
    bool                used = false, loggedCast = false, loggedUnmatched = false;
    DeferredCaster      caster;
    std::vector<GLuint> buffers;
    int                 count = 0;                  // its bones' rest positions, in its own order
    GLfloat             centre[kMaxBones * 3] = {};
    unsigned            drawnFrame = 0;
    bool                leftOut = false;            // the game left it out last frame (DrawRealBodyAlong)
};
std::vector<KeptRiderPiece> g_riderPieces;
constexpr size_t kMaxRiderPieces = 24;
// Your bike's wheels in the shadow map (postfx::OwnWheelCast): this frame's rigid draws near the
// camera (with the game's own u_modelview, to find a wheel's among them), and each wheel's draw call
// kept from when the game last drew it, cast in its place on your bike when the game doesn't.
struct NearDraw {
    DeferredCaster caster;
    GLfloat        raw[16];
    GLfloat        uploaded[16];    // its u_modelview and projection as drawn (the tire dirt's mask
    GLfloat        projection[16];  // draws it again exactly so)
};
std::vector<NearDraw> g_nearDraws;
double g_lobbyView[16] = {};   // this frame's raw camera view, for the lobby wheels (lobby_wheels.h)
bool   g_haveLobbyView = false;
constexpr size_t kMaxNearDraws = 512;
constexpr float  kNearDrawRange = 60.0f;  // game units from the camera (~18 m in feet; the chase camera sits ~12 m back)
struct KeptWheel {
    bool                valid = false;
    DeferredCaster      caster;
    std::vector<GLuint> buffers;            // its vertex array's buffers (dropped if one is deleted)
    bool                loggedKept = false, loggedCast = false;
};
KeptWheel g_keptWheels[2];  // 0 rear, 1 front
struct RiddenPart {
    bool                used = false, learnt = false, loggedCast = false;
    DeferredCaster      caster;                  // its draw call, as last drawn
    std::vector<GLuint> buffers;                 // its vertex array's buffers (dropped if one is deleted)
    double              rel[2][16] = {};         // on your bike (0) / on your torso (1), as last drawn
    double              mean[2][16] = {};        // its average place there (over about half a second)
    double              wobble[2] = { 0, 0 };    // how far it strays from that (mean square, units^2)
    bool                haveRel[2] = { false, false };
    double              seen = 0;                // seconds drawn near your rider
    double              travel = 0;              // how far it has moved through the world meanwhile (units)
    double              lastWorld[3] = {};
    int                 anchor = -1;
    unsigned            drawnFrame = 0;
};
constexpr size_t kMaxRiddenParts = 48;
std::vector<RiddenPart> g_riddenParts;
unsigned g_riddenFrame = 0;
double   g_riddenTime = -1.0;
int      g_riddenLogs = 0;
int  g_castAtOnce = 0;          // this frame's shadow casters drawn straight away (skinned, streamed)
bool g_loggedShadowBatch = false;  // by program; nodes stay put, so pointers last
bool g_hidingLogged = false;

struct ShaderCamera {
    bool   haveCorrection    = false;  // the camera was found (this frame, or last frame until it is)
    double correction[16]    = {};     // full eye-space correction, for everything with a position
    double rotationOnly[16]  = {};     // its rotation alone, for the sky (this frame's preview once there is one)
    bool   capturedThisFrame = false;
    bool   havePreview       = false;  // rotationOnly is this frame's preview
    double skyAngle          = 0;      // how far the chosen sky is from the last camera rotation
    int    previews          = 0;      // rotation-only draws taken as the sky this frame
    int    otherRotations    = 0;      // rotation-only draws that were not the camera
    int    correctedThisFrame = 0;
};
ShaderCamera g_camera;

// Rotation-only draws further than this from the last camera rotation are not the sky.
constexpr double kMaxSkyAngleDeg = 45.0;

// The terrain, drawn in patches (see terrain.h). In 3rd person replays the cells the game culled
// are drawn after its own patches, with the same draw calls and the patch uniforms changed.
constexpr char  kMeshOffsetUniform[]   = "u_meshoffset";
constexpr char  kMeshScaleUniform[]    = "u_meshscale";
constexpr char  kTexOffsetUniform[]    = "u_texoffset";
constexpr char  kTexScaleUniform[]     = "u_texscale";
constexpr char  kSpacingUniform[]      = "u_spacing";          // set when a track's heightmap loads
constexpr char  kHeightsDimUniform[]   = "u_heights_dim_inv";
constexpr GLenum kElementArrayBuffer   = 0x8893;               // GL_ELEMENT_ARRAY_BUFFER
constexpr int   kMaxPatchDraws         = 4;                    // draw calls per patch that can be replayed
constexpr int   kMaxFilledPatches      = 1024;

struct TerrainUniforms {
    GLint offset = -1, scale = -1, texOffset = -1, texScale = -1, spacing = -1, heightsDim = -1;
};
struct TerrainUniformName {
    const char* name;
    GLint TerrainUniforms::*field;
};
const TerrainUniformName kTerrainUniformNames[] = {
    { kMeshOffsetUniform, &TerrainUniforms::offset },  { kMeshScaleUniform, &TerrainUniforms::scale },
    { kTexOffsetUniform, &TerrainUniforms::texOffset }, { kTexScaleUniform, &TerrainUniforms::texScale },
    { kSpacingUniform, &TerrainUniforms::spacing },     { kHeightsDimUniform, &TerrainUniforms::heightsDim },
};
std::unordered_map<GLuint, TerrainUniforms> g_terrainUniforms;  // program -> its patch uniforms

struct GamePatch {
    terrain::Patch patch;
    DrawCall       draws[kMaxPatchDraws];
    int            drawCount = 0;
    GLuint         vao = 0;                // the vertex array it was drawn with
    bool           replayable = true;      // indices from a buffer, few enough draw calls
};

struct TerrainPass {
    TerrainUniforms uniforms;              // the bound program's patch uniforms (-1: it has none)
    bool   isTerrain     = false;          // the bound program draws the terrain in patches
    terrain::Patch current;                // the patch uniforms as last uploaded
    std::vector<GamePatch> patches;        // the game's patches so far this pass
    bool   haveEye       = false;          // the corrected camera this frame, for the fill
    bool   lookingAway   = false;          // helmet cam turned far from where the game looks
    double eye[3]        = {};
    double forward[3]    = {};
};
TerrainPass g_terrain;

// What the ground passes did this frame, for the trace and the log.
struct GroundStats {
    double      awayDeg  = -1;  // helmet cam vs the game camera, horizontally (-1: not measured)
    int         passes   = 0;   // terrain passes finished
    int         game     = 0;   // patches the game drew
    int         filled   = 0;   // cells the fill drew
    int         open     = 0;   // patches of a pass still open at the end of the frame (not filled)
    const char* note     = "";  // how the last pass's fill went, or why it didn't run
};
GroundStats g_ground;
bool        g_groundLogged = false;  // this look away has been logged
GLuint g_mapProgram = 0;                                 // the terrain program the map was learnt from
float  g_heightmapUpload[2][2] = {};                     // u_spacing, u_heights_dim_inv as last uploaded
GLuint g_vao = 0;                                        // bound by glBindVertexArray
std::unordered_map<GLuint, GLuint> g_elementBuffers;     // vertex array -> its element buffer
std::unordered_map<GLuint, unsigned> g_vaoChangedFrame;   // vertex array -> the last frame it was re-pointed (or given other indices)
terrain::Patch g_filled[kMaxFilledPatches];
// The game's draw calls per vertex array variant (see terrain.h), kept until the next heightmap.
std::unordered_map<GLuint, GamePatch> g_variants;

// Per-frame trace (TraceKey). Every frame goes into a ring buffer, so the key saves the seconds
// *before* it was pressed (the moment something looked wrong) and then keeps recording a while.
constexpr int    kTraceRing    = 4096;  // frames; covers kTraceHistory at up to ~270 fps
constexpr double kTraceHistory = 15.0;  // seconds kept before the key press
struct TraceLine {
    double time = 0;
    char   text[1024];
};
TraceLine* g_traceRing   = nullptr;     // allocated on first use
int    g_traceHead       = 0;
int    g_traceCount      = 0;
FILE*  g_trace           = nullptr;
int    g_traceFrames     = 0;
double g_traceStart      = -1;          // content time of the key press (-1: at the next frame)
double g_traceEnd        = 0;

unsigned long long UniformKey(GLuint program, GLint location)
{
    return (static_cast<unsigned long long>(program) << 32) | static_cast<unsigned int>(location);
}

// Whether the bound program draws the terrain in patches, and through which uniforms.
void RefreshTerrainProgram()
{
    AcquireSRWLockShared(&g_lock);
    const auto found = g_terrainUniforms.find(g_program);
    g_terrain.uniforms = found != g_terrainUniforms.end() ? found->second : TerrainUniforms{};
    const bool camera = g_cameraPrograms.count(g_program) != 0;
    ReleaseSRWLockShared(&g_lock);
    g_terrain.isTerrain = camera && g_terrain.uniforms.offset >= 0 && g_terrain.uniforms.scale >= 0;
}

void FinishTerrainPass();

void NoteUseProgram(GLuint program)
{
    if (program != g_program)
        FinishTerrainPass();  // before the switch: the fill draws with the terrain program
    g_program = program;
    RefreshTerrainProgram();
}

void NoteUniformLocation(GLuint program, const GLchar* name, GLint location)
{
    if (!name || location < 0)
        return;
    const Config& cfg = GetConfig();
    const unsigned long long key = UniformKey(program, location);

    AcquireSRWLockExclusive(&g_lock);
    const bool added = g_uniforms.emplace(key, name).second;
    if (strcmp(name, cfg.modelviewUniform) == 0)
        g_modelviewUniforms.insert(key);
    if (strcmp(name, cfg.lightUniform) == 0)
        g_lightUniforms.insert(key);
    if (strcmp(name, cfg.envUniform) == 0)
        g_envUniforms.insert(key);
    if (strcmp(name, kBoneUniform) == 0)
        g_boneUniforms.insert(key);
    if (strcmp(name, kBoneCentreUniform) == 0)
        g_skinned[program].centreLoc = location;
    if (strcmp(name, kBoneUniform) == 0)
        g_skinned[program].positionLoc = location;
    if (strcmp(name, kBoneRotationUniform) == 0)
        g_skinned[program].rotationLoc = location;
    if (strcmp(name, kBoneCountUniform) == 0)
        g_skinned[program].countLoc = location;
    if (strcmp(name, cfg.modelviewUniform) == 0)
        g_skinned[program].modelviewLoc = location;
    if (strcmp(name, cfg.cameraProgramUniform) == 0)
        g_cameraPrograms.insert(program);
    for (const TerrainUniformName& terrainUniform : kTerrainUniformNames) {
        if (strcmp(name, terrainUniform.name) == 0)
            g_terrainUniforms[program].*terrainUniform.field = location;
    }
    const bool log = added && g_lookupsLogged < kMaxLookupsLogged;
    if (log)
        ++g_lookupsLogged;
    ReleaseSRWLockExclusive(&g_lock);
    if (program == g_program)
        RefreshTerrainProgram();

    if (log)
        Log("uniform: program %u '%s' = location %d", program, name, location);
}

std::string UniformName(GLuint program, GLint location)
{
    AcquireSRWLockShared(&g_lock);
    const auto found = g_uniforms.find(UniformKey(program, location));
    std::string name = found != g_uniforms.end() ? found->second : "?";
    ReleaseSRWLockShared(&g_lock);
    return name;
}

// Logs the first few vec4s / the first matrix of an upload, as stored in memory
// (column-major unless transpose is set).
void DumpUniform(const char* call, GLuint program, GLint location, GLsizei count, int transpose,
                 const GLfloat* value, int floatsPerElement)
{
    if (!IsDumping() || !value || count <= 0)
        return;
    const int floats = count * floatsPerElement < 16 ? count * floatsPerElement : 16;
    char numbers[256] = "";
    int length = 0;
    for (int i = 0; i < floats && length < static_cast<int>(sizeof numbers) - 16; ++i)
        length += snprintf(numbers + length, sizeof numbers - length, "%s% .3f", i && i % (floatsPerElement % 3 == 0 ? 3 : 4) == 0 ? " |" : "", value[i]);
    DumpCall("%s prog=%u loc=%d '%s' count=%d%s [%s]", call, program, location,
             UniformName(program, location).c_str(), count, transpose ? " TRANSPOSED" : "", numbers);
}

// ---- Camera correction on u_modelview uploads -----------------------------------------------

// A rotation + translation with unit scale: world objects, the bike, the sky. HUD elements are
// scaled to fractions of a unit (or flattened), so they fail this test and stay screen-fixed.
bool IsRigid(const GLfloat* m)
{
    if (std::fabs(m[3]) > 1e-4f || std::fabs(m[7]) > 1e-4f || std::fabs(m[11]) > 1e-4f || std::fabs(m[15] - 1.0f) > 1e-4f)
        return false;
    for (int a = 0; a < 3; ++a) {
        for (int b = a; b < 3; ++b) {
            const float dot = m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2];
            if (std::fabs(dot - (a == b ? 1.0f : 0.0f)) > 0.02f)
                return false;
        }
    }
    return true;
}

bool IsIdentity(const GLfloat* m)
{
    for (int i = 0; i < 16; ++i) {
        if (m[i] != ((i % 5 == 0) ? 1.0f : 0.0f))
            return false;
    }
    return true;
}

bool HasTranslation(const GLfloat* m)
{
    return std::fabs(m[12]) + std::fabs(m[13]) + std::fabs(m[14]) > 1e-3f;
}

// The first person mod off with Controller look on: the look keys still turn the game's own
// camera (camera::LookOnlyUpdate), and nothing else of the mod touches it.
bool LookOnly(const Config& cfg) { return !cfg.enabled && cfg.controllerLook; }
// Whether the frame's view may be corrected at all (the mod, or the look keys alone).
bool Correcting(const Config& cfg) { return cfg.enabled || LookOnly(cfg); }

// A rotation-only view drawn before the camera is known this frame: if it is the closest yet to
// the last camera rotation (the sky), preview this frame's orientation from it. Anything further
// off - clouds, a sun billboard, a reflection pass - takes whatever correction is current.
void ConsiderSkyRotation(const double view[16])
{
    if (g_camera.capturedThisFrame || !g_camera.haveCorrection)
        return;
    const bool lookOnly = LookOnly(GetConfig());
    const bool replay = !lookOnly && replay::Corrects(replay::Current());
    const double angle = replay ? replay::AngleToCameraDeg(view)
                       : lookOnly ? camera::LookOnlyAngleToLastDeg(view) : camera::AngleToLastCameraDeg(view);
    if (g_camera.havePreview && std::fabs(angle - g_camera.skyAngle) < 0.01)
        return;  // the same rotation again (u_envmat, then the sky): already previewed
    if (angle < 0 || angle > kMaxSkyAngleDeg || (g_camera.havePreview && angle >= g_camera.skyAngle)) {
        ++g_camera.otherRotations;
        DumpCall("^ rotation only, %.1f deg from the camera: not taken as the sky", angle);
        return;
    }
    const bool previewed = replay ? replay::PreviewRotation(view, g_camera.rotationOnly)
                         : lookOnly ? camera::LookOnlyPreview(view, g_camera.rotationOnly)
                         : camera::PreviewRotation(view, g_camera.rotationOnly);
    if (previewed) {
        g_camera.havePreview = true;
        g_camera.skyAngle = angle;
        ++g_camera.previews;
        DumpCall("^ SKY (%.2f deg from last frame's camera): orientation previewed for this frame", angle);
    }
}

// Where the corrected camera is this frame, for the terrain fill: its view is C * the raw view.
void NoteCorrectedCamera(const double view[16])
{
    double m[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += g_camera.correction[k * 4 + r] * view[c * 4 + k];
            m[c * 4 + r] = sum;
        }
    }
    for (int c = 0; c < 3; ++c) {
        g_terrain.eye[c] = -(m[c * 4] * m[12] + m[c * 4 + 1] * m[13] + m[c * 4 + 2] * m[14]);  // -R^T t
        g_terrain.forward[c] = -m[c * 4 + 2];                                                  // -(row 2)
    }
    g_terrain.haveEye = true;
}

// The game culls the ground behind its own camera, so a helmet cam turned well away from it
// (the d-pad looks) needs the terrain fill, as a replay does. A few degrees of whip lock or
// air looseness stay well inside what the game draws.
constexpr double kLookAwayFillDeg = 30.0;
// The horizontal angle between the corrected camera and the game's (-1 when either looks
// straight down).
double LookAwayDeg(const double view[16])
{
    const double rawX = -view[2], rawZ = -view[10];  // the game camera's forward, horizontal
    const double ourX = g_terrain.forward[0], ourZ = g_terrain.forward[2];
    const double rawLength = std::sqrt(rawX * rawX + rawZ * rawZ), ourLength = std::sqrt(ourX * ourX + ourZ * ourZ);
    if (rawLength < 1e-3 || ourLength < 1e-3)
        return -1;
    const double cosine = (rawX * ourX + rawZ * ourZ) / (rawLength * ourLength);
    return std::acos(std::fmax(-1.0, std::fmin(1.0, cosine))) * 180.0 / 3.14159265358979;
}

// Third person: the look keys swing the camera round your rider (camera::OrbitLook), so you stay
// in view: looking back, the camera is out in front of you, facing you.
void OrbitOwnRider(const double view[16], double correction[16])
{
    double rider[3];
    if (camera::LookTurn() != 0 && postfx::OwnRiderForLook(rider))
        camera::OrbitLook(view, rider, correction);
}

// Returns the matrix to actually upload: value itself, or scratch holding C * value.
const GLfloat* CorrectModelview(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                                const GLfloat* value, GLfloat* scratch)
{
    const Config& cfg = GetConfig();
    if (!Correcting(cfg) || !value || count != 1 || transpose)
        return value;
    const bool lookOnly = LookOnly(cfg);
    // The mod off and the look keys not turning the view: once the camera has been seen this frame
    // there's nothing more to do (the sky only matters before it, the bike tracker is off) - skipped
    // before the lookups, as this runs for every draw.
    if (lookOnly && g_camera.capturedThisFrame && !g_camera.haveCorrection)
        return value;

    AcquireSRWLockShared(&g_lock);
    const bool isModelview = g_modelviewUniforms.count(UniformKey(program, location)) != 0;
    const bool knownCamera = !g_cameraPrograms.empty();
    const bool isCamera    = g_cameraPrograms.count(program) != 0;
    ReleaseSRWLockShared(&g_lock);

    if (!isModelview || !IsRigid(value) || IsIdentity(value))
        return value;

    // The terrain program's first upload is the bare view matrix. If the configured terrain
    // uniform was never seen, fall back to the first rigid upload that has a translation.
    if (!g_camera.capturedThisFrame && (knownCamera ? isCamera : HasTranslation(value))) {
        double view[16];
        for (int i = 0; i < 16; ++i)
            view[i] = value[i];
        g_camera.capturedThisFrame = true;
        if (lookOnly) {
            // The mod off: the look keys alone turn the game's camera (never a replay's).
            g_camera.haveCorrection = !replay::LooksLikeReplay(view, camera::ContentTime()) &&
                                      camera::LookOnlyUpdate(view, g_camera.correction);
            if (g_camera.haveCorrection) {
                std::copy(g_camera.correction, g_camera.correction + 16, g_camera.rotationOnly);
                OrbitOwnRider(view, g_camera.correction);  // (third person; the rotation-only copy stays put)
                NoteCorrectedCamera(view);
                g_ground.awayDeg = LookAwayDeg(view);
                g_terrain.lookingAway = g_ground.awayDeg > kLookAwayFillDeg;
                DumpCall("^ CAMERA  look only (first person mod off): %.0f deg from the game camera", g_ground.awayDeg);
            } else {
                DumpCall("^ CAMERA  look only (first person mod off): looking ahead, untouched");
            }
        } else if (const replay::Mode before = replay::Current(),
                   mode = replay::Update(view, camera::ContentTime(), g_camera.correction, g_camera.rotationOnly);
                   mode != replay::Mode::Off) {
            // A replay: the helmet cam and the bike tracker sit it out.
            if (before == replay::Mode::Off)
                bike::Reset();
            g_camera.haveCorrection = replay::Corrects(mode);
            if (g_camera.haveCorrection)
                NoteCorrectedCamera(view);
            char text[256];
            replay::Describe(text, sizeof text);
            DumpCall("^ CAMERA  %s", text);
        } else {
            bike::BeginFrame(view);
            g_camera.haveCorrection = camera::Update(view, g_camera.correction) &&
                                      camera::RotationCorrection(g_camera.rotationOnly);
            if (g_camera.haveCorrection) {
                OrbitOwnRider(view, g_camera.correction);  // (third person)
                NoteCorrectedCamera(view);
                g_ground.awayDeg = LookAwayDeg(view);
                g_terrain.lookingAway = g_ground.awayDeg > kLookAwayFillDeg;
                const camera::Telemetry& t = camera::LastTelemetry();
                DumpCall("^ CAMERA  dt=%.1fms lag=%.2fdeg roll=%.2fdeg rollCorr=%.2fdeg pitchExcess=%.1fdeg "
                         "viewPitch=%.1fdeg lookYaw=%.0fdeg airYaw=%.1fdeg air=%.2f bike=%s %s accelUp=%.2fg "
                         "neck=%.1fmm eyeHeight%+.1fmm",
                         t.dtMs, t.lagDeg, t.rawRollDeg, t.rollCorrectionDeg, t.pitchExcessDeg, t.viewPitchDeg,
                         t.lookYawDeg, t.airYawDeg, t.airborne, t.bikeTracked ? "yes" : "no",
                         t.customActive ? "CUSTOM" : "GAME", t.verticalAccelG, t.neckOffset * 1000.0,
                         t.verticalOffset * 1000.0);
            } else {
                DumpCall("^ CAMERA rejected (not a rigid view matrix)");
            }
        }
    } else if (!HasTranslation(value)) {
        // The sky: a rotation-only view, drawn before the terrain. Its rotation is this frame's
        // raw camera rotation, which is all the orientation half of the camera pipeline needs,
        // so the sky turns in step with this frame's world instead of last frame's. Only the
        // rotation is applied: the sky is infinitely far away, so eye offsets (neck, head
        // position) must not move it.
        double view[16];
        for (int i = 0; i < 16; ++i)
            view[i] = value[i];
        ConsiderSkyRotation(view);
    } else if (g_camera.capturedThisFrame && !lookOnly && replay::Current() == replay::Mode::Off) {
        bike::Observe(program, value);  // raw View * Model: one of the candidates for the bike
    }
    if (!g_camera.haveCorrection)
        return value;

    // scratch = C * value, accumulated in double precision.
    const double* correction = HasTranslation(value) ? g_camera.correction : g_camera.rotationOnly;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += correction[k * 4 + r] * value[c * 4 + k];
            scratch[c * 4 + r] = static_cast<GLfloat>(sum);
        }
    }
    ++g_camera.correctedThisFrame;
    return scratch;
}

// ---- What the shaders need to know (postfx.h) ----------------------------------------------
bool IsPerspectiveMatrix(const GLfloat* m)
{
    return std::fabs(m[15]) < 1e-6f && std::fabs(m[11]) > 1e-6f && std::fabs(m[3]) < 1e-6f && std::fabs(m[7]) < 1e-6f;
}

// Every mat4 upload: perspective projections, and per program whether its u_modelview holds a
// 3D object, an identity (2D screens) or something scaled (HUD elements). The terrain's first
// upload of the frame is the camera; uploaded is what actually went to the driver for it.
void NoteSceneMatrix(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value,
                     const GLfloat* uploaded)
{
    if (!value || count != 1 || location < 0)
        return;
    GLfloat transposed[16];
    const GLfloat* m = value;
    if (transpose) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                transposed[c * 4 + r] = value[r * 4 + c];
        m = transposed;
    }
    AcquireSRWLockShared(&g_lock);
    const bool isModelview = g_modelviewUniforms.count(UniformKey(program, location)) != 0;
    const bool knownCamera = !g_cameraPrograms.empty();
    const bool isCamera    = g_cameraPrograms.count(program) != 0;
    ReleaseSRWLockShared(&g_lock);

    if (IsPerspectiveMatrix(m)) {
        postfx::NoteProjection(m, isCamera);
        ProgramProjection& projection = g_programProjection[program];
        projection.location = location;
        std::copy(m, m + 16, projection.value);
        return;
    }
    if (!isModelview) {
        // A flat projection (the HUD's): where its draws land on screen (LeaveOutGameOverlay).
        if (GetConfig().hideCursor) {
            AcquireSRWLockShared(&g_lock);
            const auto name = g_uniforms.find(UniformKey(program, location));
            const bool projection = name != g_uniforms.end() && name->second.find("proj") != std::string::npos;
            ReleaseSRWLockShared(&g_lock);
            if (projection) {
                ProgramProjection& flat = g_flatProjection[program];
                flat.location = location;
                std::copy(m, m + 16, flat.value);
            }
        }
        return;
    }
    const bool identity = IsIdentity(m);
    const bool scene = !identity && IsRigid(m);
    ProgramModelview& kind = g_programModelview[program];
    kind.frame = g_frameNumber;
    kind.kind = scene ? ModelviewKind::Scene : identity ? ModelviewKind::Identity : ModelviewKind::Scaled;
    kind.location = location;
    kind.transposed = transpose != GL_FALSE;
    std::copy(uploaded, uploaded + 16, kind.uploaded);
    std::copy(m, m + 16, kind.raw);
    if (!identity && g_shaderViewSet && HasTranslation(m))
        postfx::NoteObject(program, m);  // one of them is your bike, others its wheels  // one of them is your bike (the headlight rides on it)
    if (!g_shaderViewSet && scene && (knownCamera ? isCamera : HasTranslation(m))) {
        g_shaderViewSet = true;
        double raw[16], rendered[16];
        for (int i = 0; i < 16; ++i) {
            raw[i] = m[i];
            rendered[i] = transpose ? m[i] : uploaded[i];  // transposed uploads are never corrected
        }
        std::copy(raw, raw + 16, g_lobbyView);  // (lobby wheels: postfx's is gone by the frame's end)
        g_haveLobbyView = true;
        postfx::NoteView(raw, rendered);
    }
}

// Whether the bound program's next draw is flat, drawn over the 3D scene (the HUD, the game's
// menus): why, or null for part of the 3D scene.
const char* FlatDrawReason()
{
    const auto found = g_programModelview.find(g_program);
    const bool fresh = found != g_programModelview.end() && found->second.frame == g_frameNumber;
    if (fresh && found->second.kind == ModelviewKind::Scene)
        return nullptr;
    if (fresh && found->second.kind == ModelviewKind::Scaled)
        return "a scaled u_modelview";
    if (!real_glIsEnabled)
        real_glIsEnabled = reinterpret_cast<PFN_IsEnabled>(GetRealGLFunction("glIsEnabled"));
    if (real_glIsEnabled && !real_glIsEnabled(GL_DEPTH_TEST))
        return fresh ? "an identity u_modelview without depth testing" : "drawn without depth testing";
    return nullptr;
}

// Before a draw call, while the shaders wait for the HUD: is this draw the start of it?
void CheckSceneEnd()
{
    if (const char* why = FlatDrawReason())
        postfx::SceneEnd(g_program, why);
}

// The game's lens flare: its sprites are added onto the picture (additive blending) after the
// 3D scene, as 2D draws (a scaled or identity u_modelview, or none), and its sun is a billboard
// that turns with the camera but doesn't move with it. With a time of day set they would still
// sit at the game's own sun (at night: a glare in the dark sky and ghosts on the ground).
std::unordered_set<GLuint> g_loggedFlare;  // render thread only

bool IsGameFlare()
{
    if (!real_glIsEnabled)
        real_glIsEnabled = reinterpret_cast<PFN_IsEnabled>(GetRealGLFunction("glIsEnabled"));
    if (!real_glGetIntegerv)
        real_glGetIntegerv = reinterpret_cast<PFN_GetIntegerv>(GetRealGLFunction("glGetIntegerv"));
    if (!real_glIsEnabled || !real_glGetIntegerv || !real_glIsEnabled(GL_BLEND))
        return false;
    constexpr GLenum kBlendDstRgb = 0x80C8;
    GLint dst = 0;
    real_glGetIntegerv(kBlendDstRgb, &dst);
    if (dst != GL_ONE)
        return false;  // not added onto the picture
    const auto found = g_programModelview.find(g_program);
    const bool fresh = found != g_programModelview.end() && found->second.frame == g_frameNumber;
    if (!fresh && real_glIsEnabled(GL_DEPTH_TEST))
        return false;  // no u_modelview of its own this frame, and depth tested: part of the world
    const bool flat = !fresh || found->second.kind != ModelviewKind::Scene;
    if (!flat && HasTranslation(found->second.uploaded))
        return false;  // something in the world (dust, lights): keep it
    if (g_loggedFlare.insert(g_program).second)
        Log("postfx: hiding the game's lens flare while a time of day is set (program %u, %s)", g_program,
            flat ? "added onto the picture in 2D" : "a billboard in the sky");
    return true;
}

// ---- The game's HUD and cursor while riding (Config::hideHud / hideCursor) -----------------
// The HUD is what's drawn flat over this frame's 3D scene (FlatDrawReason), on screen, up to the
// swap; frames with no 3D scene are the game's menus and are never touched. The game's cursor,
// if it draws one of its own, is the flat draw whose place on screen follows the mouse: learnt
// while the mouse moves (in the menus too), by its program, texture, size and vertex count. When
// and for how long they're hidden is overlay.cpp's (HideGameOverlayNow).
std::atomic<bool> g_sceneOnScreen{ false };
int  g_hudSkipped = 0, g_cursorSkipped = 0;  // this frame
bool g_loggedHudHidden = false;

struct CursorTrack {
    unsigned long long signature = 0;
    unsigned frame = 0, prevFrame = 0;     // the frames it was last drawn in
    float    x = 0, y = 0, prevX = 0, prevY = 0;  // where on screen (client pixels from the top left)
    bool     twice = false;                // drawn more than once this frame: which one moved?
    int      hits = 0;                     // frames in a row it moved with the mouse
};
std::vector<CursorTrack> g_cursorTracks;
constexpr size_t kMaxCursorTracks = 128;
constexpr int    kCursorHits = 4;
unsigned long long g_cursorSignature = 0;  // the game's cursor (0: not found)
float g_mouseX = 0, g_mouseY = 0;
float g_mouseMoveX[2] = {}, g_mouseMoveY[2] = {};  // the mouse's move over this frame and the one before
bool  g_haveMouse = false;
GLint g_viewport[4] = {};
unsigned g_viewportFrame = 0;

// Blended additively onto the picture (the lens flare): not the HUD.
bool AddedOntoPicture()
{
    if (!real_glIsEnabled)
        real_glIsEnabled = reinterpret_cast<PFN_IsEnabled>(GetRealGLFunction("glIsEnabled"));
    if (!real_glGetIntegerv)
        real_glGetIntegerv = reinterpret_cast<PFN_GetIntegerv>(GetRealGLFunction("glGetIntegerv"));
    if (!real_glIsEnabled || !real_glGetIntegerv || !real_glIsEnabled(GL_BLEND))
        return false;
    constexpr GLenum kBlendDstRgb = 0x80C8;
    GLint dst = 0;
    real_glGetIntegerv(kBlendDstRgb, &dst);
    return dst == GL_ONE;
}

bool DrawingToScreen()
{
    if (!real_glGetIntegerv)
        real_glGetIntegerv = reinterpret_cast<PFN_GetIntegerv>(GetRealGLFunction("glGetIntegerv"));
    if (!real_glGetIntegerv)
        return false;
    constexpr GLenum kDrawFramebufferBinding = 0x8CA6;
    GLint fb = 0;
    real_glGetIntegerv(kDrawFramebufferBinding, &fb);
    return fb == 0;
}

// A flat draw with a modelview of its own: where its origin lands on screen, and what it is.
bool FlatDrawPlace(const DrawCall& draw, float& x, float& y, unsigned long long& signature)
{
    const auto mv = g_programModelview.find(g_program);
    if (mv == g_programModelview.end() || mv->second.frame != g_frameNumber || mv->second.kind != ModelviewKind::Scaled ||
        mv->second.transposed)
        return false;
    const GLfloat* m = mv->second.raw;
    float clip[4] = { m[12], m[13], m[14], 1.0f };
    const auto flat = g_flatProjection.find(g_program);
    if (flat != g_flatProjection.end()) {
        const GLfloat* p = flat->second.value;
        for (int i = 0; i < 4; ++i)
            clip[i] = p[i] * m[12] + p[4 + i] * m[13] + p[8 + i] * m[14] + p[12 + i];
    }
    if (std::fabs(clip[3]) < 1e-6f)
        return false;
    if (g_viewportFrame != g_frameNumber) {
        g_viewportFrame = g_frameNumber;
        if (real_glGetIntegerv)
            real_glGetIntegerv(GL_VIEWPORT, g_viewport);
    }
    if (g_viewport[2] <= 0 || g_viewport[3] <= 0)
        return false;
    x = (clip[0] / clip[3] * 0.5f + 0.5f) * g_viewport[2] + g_viewport[0];
    y = (0.5f - clip[1] / clip[3] * 0.5f) * g_viewport[3];
    const unsigned long long scaleX = static_cast<unsigned long long>(std::lround(std::fabs(m[0]) * 4096.0f));
    const unsigned long long scaleY = static_cast<unsigned long long>(std::lround(std::fabs(m[5]) * 4096.0f));
    signature = (static_cast<unsigned long long>(g_program) * 0x9E3779B97F4A7C15ull) ^
                (static_cast<unsigned long long>(g_boundTextures[0]) * 0xC2B2AE3D27D4EB4Full) ^
                (static_cast<unsigned long long>(draw.count) * 0x165667B19E3779F9ull) ^ (scaleX << 20) ^ (scaleY << 40);
    return signature != 0;
}

// Notes where a flat draw is this frame, for finding the cursor among them.
void TrackFlatDraw(unsigned long long signature, float x, float y)
{
    CursorTrack* track = nullptr;
    for (CursorTrack& t : g_cursorTracks)
        if (t.signature == signature) {
            track = &t;
            break;
        }
    if (!track) {
        if (g_cursorTracks.size() >= kMaxCursorTracks)
            return;
        g_cursorTracks.push_back(CursorTrack{});
        track = &g_cursorTracks.back();
        track->signature = signature;
    }
    if (track->frame == g_frameNumber) {
        track->twice = true;
        return;
    }
    track->prevFrame = track->frame;
    track->prevX = track->x;
    track->prevY = track->y;
    track->frame = g_frameNumber;
    track->x = x;
    track->y = y;
    track->twice = false;
}

// At the end of a frame: the flat draw that moved with the mouse, a few frames running, is the
// game's cursor. (The game may draw it where the mouse was a frame ago: either move counts.)
void LearnCursor()
{
    float mx = 0, my = 0;
    const bool have = GameCursorPos(mx, my);
    g_mouseMoveX[1] = g_mouseMoveX[0];
    g_mouseMoveY[1] = g_mouseMoveY[0];
    g_mouseMoveX[0] = have && g_haveMouse ? mx - g_mouseX : 0.0f;
    g_mouseMoveY[0] = have && g_haveMouse ? my - g_mouseY : 0.0f;
    g_haveMouse = have;
    g_mouseX = mx;
    g_mouseY = my;
    if (!have)
        return;
    const bool mouseMoved = std::fabs(g_mouseMoveX[0]) + std::fabs(g_mouseMoveY[0]) >= 4.0f ||
                            std::fabs(g_mouseMoveX[1]) + std::fabs(g_mouseMoveY[1]) >= 4.0f;
    for (size_t i = 0; i < g_cursorTracks.size();) {
        CursorTrack& t = g_cursorTracks[i];
        if (g_frameNumber - t.frame > 600) {  // not drawn for a while
            g_cursorTracks[i] = g_cursorTracks.back();
            g_cursorTracks.pop_back();
            continue;
        }
        ++i;
        if (t.frame != g_frameNumber || t.prevFrame + 1 != g_frameNumber || t.twice)
            continue;
        const float dx = t.x - t.prevX, dy = t.y - t.prevY;
        const bool itMoved = std::fabs(dx) + std::fabs(dy) > 0.5f;
        if (!mouseMoved && !itMoved)
            continue;
        bool withMouse = false;
        for (int k = 0; k < 2 && !withMouse; ++k) {
            const float mdx = g_mouseMoveX[k], mdy = g_mouseMoveY[k];
            const float size = std::fabs(mdx) + std::fabs(mdy);
            withMouse = size >= 4.0f && std::fabs(dx - mdx) + std::fabs(dy - mdy) < 3.0f + 0.05f * size;
        }
        withMouse = withMouse && std::fabs(t.x - mx) + std::fabs(t.y - my) < 160.0f;
        t.hits = withMouse ? t.hits + 1 : 0;
        if (t.hits >= kCursorHits && g_cursorSignature != t.signature) {
            g_cursorSignature = t.signature;
            Log("hud: found the game's own mouse cursor (it follows the mouse on screen); hidden while riding");
        }
    }
}

// The game's own menu over the race (Esc): it's drawn flat over the 3D scene like the HUD, so it's
// told apart by what it draws that the HUD doesn't. Esc opens it; while it's open, the flat draws
// the HUD hasn't drawn lately that keep being drawn are the menu's, learnt for the session. From
// then on the menu is open while those are drawn (however it was opened or closed), and Esc
// toggles it in between. Each page of it is learnt as a whole, and it's open only while most of one
// page is drawn: a page learnt as any two draws once had a couple of HUD texts that changed while it
// was open (a timer, a position) taken for it, and the HUD and cursor then never went.
struct GameMenuWatch {
    std::unordered_map<unsigned long long, unsigned> hudSeen;  // flat draw -> last frame drawn with the menu closed
    std::unordered_map<unsigned long long, int> candidates;     // while open: frames each one was drawn
    std::vector<std::unordered_set<unsigned long long>> pages;  // learnt: the menu's pages, each its draws
    std::vector<unsigned long long> frameDraws;                 // this frame's flat draws
    int  logs = 0;          // (its opening and closing logged, the first few times)
    bool open = false;
    bool sawMenu = false;   // its draws seen since it opened
    int  window = 0;        // frames learnt over so far (in windows of kMenuLearnFrames)
    int  absent = 0;        // frames in a row its draws weren't seen, after they were
    int  unseenOpen = 0;    // frames open without its draws seen (once they're known)
    bool loggedOpen = false, loggedLearnt = false;
};
GameMenuWatch g_gameMenu;
constexpr int      kMenuLearnFrames = 45;
constexpr unsigned kHudRecentFrames = 600;  // ~10 s: a HUD draw that recent isn't the menu's

unsigned long long FlatDrawKey(const DrawCall& draw)
{
    return (static_cast<unsigned long long>(g_program) * 0x9E3779B97F4A7C15ull) ^
           (static_cast<unsigned long long>(g_boundTextures[0]) * 0xC2B2AE3D27D4EB4Full) ^
           (static_cast<unsigned long long>(static_cast<unsigned>(draw.count)) * 0x165667B19E3779F9ull) ^
           (static_cast<unsigned long long>(draw.mode) << 56) ^ (static_cast<unsigned long long>(draw.kind) << 60);
}

// Whether most of one of the menu's pages is drawn this frame (60% of it, and two draws at least).
bool MenuPageDrawn()
{
    const std::unordered_set<unsigned long long> drawn(g_gameMenu.frameDraws.begin(), g_gameMenu.frameDraws.end());
    for (const auto& page : g_gameMenu.pages) {
        size_t present = 0;
        for (unsigned long long key : page)
            present += drawn.count(key);
        if (present >= 2 && present * 10 >= page.size() * 6)
            return true;
    }
    return false;
}

void LogGameMenu(const char* what)
{
    if (g_gameMenu.logs < 30) {
        ++g_gameMenu.logs;
        Log("hud: the game's menu %s", what);
    }
}

// At the end of a frame with the HUD or cursor hidden: is the game's menu open?
void UpdateGameMenu()
{
    GameMenuWatch& m = g_gameMenu;
    const int escapes = TakeGameEscapes();
    if (!g_shaderViewSet) {  // no race on screen: the game's own menu screens, left alone anyway
        m.open = m.sawMenu = false;
        m.candidates.clear();
        m.window = m.absent = 0;
        m.frameDraws.clear();
        return;
    }
    for (int k = 0; k < escapes; ++k) {
        m.open = !m.open;
        m.sawMenu = false;
        m.absent = m.unseenOpen = 0;
        m.candidates.clear();
        m.window = 0;
        LogGameMenu(m.open ? "opened (Esc): the HUD and the menu are shown while it's open" : "closed (Esc): the HUD hidden again");
        if (m.open && !m.loggedOpen)
            m.loggedOpen = true;
    }
    const bool learnt = !m.pages.empty();
    if (learnt && MenuPageDrawn()) {
        if (!m.open)
            LogGameMenu("is open (a page of it is drawn)");
        m.open = true;
        m.sawMenu = true;
        m.absent = m.unseenOpen = 0;
    } else if (m.open && m.sawMenu && ++m.absent >= 6) {
        m.open = m.sawMenu = false;  // closed without Esc (a click in it)
        m.candidates.clear();
        m.window = 0;
        LogGameMenu("closed (its pages aren't drawn any more): the HUD hidden again");
    } else if (m.open && !m.sawMenu && learnt && ++m.unseenOpen >= 2 * kMenuLearnFrames) {
        // Esc, but none of the menu's pages (nor one learnt meanwhile, a page of it not seen yet): it
        // was for something else (the chat) and the menu isn't open.
        m.open = false;
        m.candidates.clear();
        m.window = m.unseenOpen = 0;
        LogGameMenu("isn't open after all (that Esc was for something else): the HUD hidden again");
    }
    if (m.open) {
        // Learning: the draws the HUD hasn't drawn lately, kept for most of a window of frames: a page
        // (unless it's mostly one already known).
        std::unordered_set<unsigned long long> counted;
        for (unsigned long long key : m.frameDraws) {
            const auto hud = m.hudSeen.find(key);
            if ((hud != m.hudSeen.end() && g_frameNumber - hud->second < kHudRecentFrames) || !counted.insert(key).second)
                continue;
            ++m.candidates[key];
        }
        if (++m.window >= kMenuLearnFrames) {
            std::unordered_set<unsigned long long> page;
            for (const auto& c : m.candidates)
                if (c.second * 10 >= m.window * 8)
                    page.insert(c.first);
            bool known = page.size() < 2;
            for (const auto& old : m.pages) {
                size_t shared = 0;
                for (unsigned long long key : page)
                    shared += old.count(key);
                known = known || shared * 10 >= page.size() * 8;
            }
            if (!known && m.pages.size() < 32) {
                m.pages.push_back(page);
                if (!m.loggedLearnt || m.logs < 30) {
                    m.loggedLearnt = true;
                    Log("hud: learnt a page of the game's menu (%zu draws of its own, %zu page(s) known): shown whenever it's open",
                        page.size(), m.pages.size());
                }
            }
            m.candidates.clear();
            m.window = 0;
        }
    } else {
        for (unsigned long long key : m.frameDraws)
            m.hudSeen[key] = g_frameNumber;
        if (m.hudSeen.size() > 4096)
            for (auto it = m.hudSeen.begin(); it != m.hudSeen.end();)
                it = g_frameNumber - it->second > kHudRecentFrames ? m.hudSeen.erase(it) : std::next(it);
    }
    m.frameDraws.clear();
}

// Whether to leave this draw out: the HUD or the game's cursor, while riding with them hidden.
bool LeaveOutGameOverlay(const DrawCall& draw)
{
    const Config& cfg = GetConfig();
    if (!cfg.hideHud && !cfg.hideCursor)
        return false;
    const char* flat = FlatDrawReason();
    if (!flat)
        return false;
    float x = 0, y = 0;
    unsigned long long signature = 0;
    const bool placed = cfg.hideCursor && FlatDrawPlace(draw, x, y, signature);
    if (placed)
        TrackFlatDraw(signature, x, y);
    if (!g_shaderViewSet || !DrawingToScreen() || AddedOntoPicture())
        return false;  // the game's menus (no 3D scene), into a texture of its own, or the lens flare
    if (g_gameMenu.frameDraws.size() < 4096)
        g_gameMenu.frameDraws.push_back(FlatDrawKey(draw));
    if (cfg.hideHud && HideGameOverlayNow(false)) {
        ++g_hudSkipped;
        if (!g_loggedHudHidden) {
            g_loggedHudHidden = true;
            Log("hud: hiding the game's HUD while riding (first HUD draw: program %u, %s)", g_program, flat);
        }
        DumpCall("^ SKIPPED  the game's HUD (%s)", flat);
        return true;
    }
    if (placed && g_cursorSignature && signature == g_cursorSignature && HideGameOverlayNow(true)) {
        ++g_cursorSkipped;
        DumpCall("^ SKIPPED  the game's mouse cursor");
        return true;
    }
    return false;
}

// ---- Your own torso and head (see SkinnedProgram) -----------------------------------------
SkinnedProgram* FindSkinned(GLuint program)
{
    AcquireSRWLockShared(&g_lock);
    const auto found = g_skinned.find(program);
    SkinnedProgram* skin = found != g_skinned.end() ? &found->second : nullptr;
    ReleaseSRWLockShared(&g_lock);
    return skin;
}

// Returns what to upload: uploaded, or scratch holding the helmet camera's own helmet.
const GLfloat* NoteSkinnedView(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                               const GLfloat* value, const GLfloat* uploaded, GLfloat* scratch)
{
    SkinnedProgram* skin = value ? FindSkinned(program) : nullptr;
    if (!skin || location != skin->modelviewLoc || location < 0 || count != 1 || transpose)
        return uploaded;
    if (replay::Current() != replay::Mode::Off && replay::ObserveModelview(value, scratch)) {
        uploaded = scratch;
        DumpCall("^ HELMET  the followed rider's head, held still under the helmet camera");
    }
    std::copy(value, value + 16, skin->rawView);
    std::copy(uploaded, uploaded + 16, skin->usedView);
    skin->haveView = true;
    return uploaded;
}

void NoteSkinnedCount(GLint location, GLint value)
{
    SkinnedProgram* skin = FindSkinned(g_program);
    if (skin && location == skin->countLoc && location >= 0)
        skin->bones = value;
}

// Bone centres and positions (vec3 arrays).
void NoteSkinnedVectors(GLint location, GLsizei count, const GLfloat* value, bool arb)
{
    SkinnedProgram* skin = value ? FindSkinned(g_program) : nullptr;
    if (!skin || location < 0 || count <= 0)
        return;
    const int n = std::min<int>(count, kMaxBones);
    if (location == skin->centreLoc) {
        std::copy(value, value + n * 3, skin->centre);
        skin->centres = n;
    } else if (location == skin->positionLoc) {
        std::copy(value, value + n * 3, skin->position);
        skin->positions = n;
        skin->positionArb = arb;
        skin->positionEdited = false;
    }
}

void NoteSkinnedRotations(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value, bool arb)
{
    SkinnedProgram* skin = value ? FindSkinned(g_program) : nullptr;
    if (!skin || location != skin->rotationLoc || location < 0 || count <= 0)
        return;
    const int n = std::min<int>(count, kMaxBones);
    std::copy(value, value + n * 9, skin->rotation);
    skin->rotations = n;
    skin->transposed = transpose != GL_FALSE;
    skin->rotationArb = arb;
    skin->rotationEdited = false;
}

// The eye of a rigid modelview, in the space it maps from (column-major: -R^T t).
void EyeOf(const GLfloat* m, double eye[3])
{
    for (int i = 0; i < 3; ++i)
        eye[i] = -(double(m[i * 4]) * m[12] + double(m[i * 4 + 1]) * m[13] + double(m[i * 4 + 2]) * m[14]);
}

// Picks the bones on the body's midline (the torso and head) and says whether this is your own
// rider: the game's eye, taken back into the torso bone's rest pose, has to sit in its head
// (a capture had it at +0.04 +2.19 -0.82, half-span 1.46: centred, above the shoulders, forward).
// Sizes are in units of the skeleton's half-span (the outermost arm bone), so scale doesn't matter.
bool OwnTorso(const SkinnedProgram& skin, int bones, bool hidden[kMaxBones], double rest[3], double* size)
{
    double halfSpan = 0;
    for (int b = 0; b < bones; ++b)
        halfSpan = std::max(halfSpan, std::fabs(double(skin.centre[b * 3])));
    *size = halfSpan;
    if (halfSpan < 1e-3)
        return false;
    int torso = -1, midline = 0;
    for (int b = 0; b < bones; ++b) {
        hidden[b] = std::fabs(skin.centre[b * 3]) <= 0.05 * halfSpan;
        if (!hidden[b])
            continue;
        ++midline;
        if (torso < 0 || skin.centre[b * 3 + 1] > skin.centre[torso * 3 + 1])
            torso = b;  // the highest one carries the head
    }
    if (torso < 0 || midline == bones)
        return false;

    double eye[3];
    EyeOf(skin.rawView, eye);
    const GLfloat* r = skin.rotation + torso * 9;
    double offset[3];
    for (int i = 0; i < 3; ++i)
        offset[i] = eye[i] - skin.position[torso * 3 + i];
    for (int i = 0; i < 3; ++i) {  // R^T (eye - t): the eye relative to the bone's centre, at rest
        double sum = 0;
        for (int k = 0; k < 3; ++k)
            sum += (skin.transposed ? r[k * 3 + i] : r[i * 3 + k]) * offset[k];
        rest[i] = sum;
    }
    return std::fabs(rest[0]) < 0.45 * halfSpan && std::fabs(rest[2]) < 1.2 * halfSpan &&
           rest[1] > 0.6 * halfSpan && rest[1] < 2.5 * halfSpan;
}

void UploadSkinnedVectors(GLint location, GLsizei count, const GLfloat* value, bool arb)
{
    PFN_Uniform3fv upload = arb && real_glUniform3fvARB ? real_glUniform3fvARB : real_glUniform3fv;
    if (!upload)
        upload = real_glUniform3fvARB;
    if (upload)
        upload(location, count, value);
}

void UploadSkinnedRotations(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value, bool arb)
{
    PFN_UniformMatrix3fv upload = arb && real_glUniformMatrix3fvARB ? real_glUniformMatrix3fvARB : real_glUniformMatrix3fv;
    if (!upload)
        upload = real_glUniformMatrix3fvARB;
    if (upload)
        upload(location, count, transpose, value);
}

// Right before each draw: for your own rider, swap in the hidden torso; otherwise make sure the
// program holds the game's own bones again.
void PrepareSkinnedDraw()
{
    SkinnedProgram* skin = FindSkinned(g_program);
    if (!skin || skin->centreLoc < 0 || skin->positionLoc < 0 || skin->rotationLoc < 0)
        return;
    const int bones = skin->bones;
    const bool complete = bones > 0 && bones <= kMaxBones && skin->centres >= bones &&
                          skin->positions >= bones && skin->rotations >= bones && skin->haveView;
    if (bones == 0 && skin->haveView) {  // a rigid draw: one bone at its origin (the headlight, tire marks)
        static const GLfloat zero[3] = { 0, 0, 0 }, identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        postfx::NoteSkeleton(g_program, zero, zero, identity, false, 1, skin->rawView);
    }
    if (!complete && !skin->positionEdited && !skin->rotationEdited)
        return;
    if (complete && replay::Current() != replay::Mode::Off)  // the helmet camera rides on the skeleton
        replay::ObserveSkeleton(skin->centre, skin->position, skin->rotation, skin->transposed, bones);
    if (complete)  // your rider (the headlight) and your bike's wheels (tire marks, tire dirt)
        postfx::NoteSkeleton(g_program, skin->centre, skin->position, skin->rotation, skin->transposed, bones,
                             skin->rawView);

    const Config& cfg = GetConfig();
    bool hidden[kMaxBones] = {};
    double rest[3] = {}, size = 0;
    const bool own = complete && OwnTorso(*skin, bones, hidden, rest, &size);
    const bool hide = own && cfg.enabled && cfg.hideOwnBody;
    if (complete && IsDumping())
        DumpCall("^ RIDER prog=%u bones=%d: eye in the torso's rest frame %+.2f %+.2f %+.2f (half-span %.2f): %s",
                 g_program, bones, rest[0], rest[1], rest[2], size,
                 own ? (hide ? "yours, torso and head hidden" : "yours") : "someone else's / another view");

    if (hide) {
        double eye[3];
        EyeOf(skin->usedView, eye);  // the eye the frame is actually drawn from
        GLfloat position[kMaxBones * 3], rotation[kMaxBones * 9];
        std::copy(skin->position, skin->position + skin->positions * 3, position);
        std::copy(skin->rotation, skin->rotation + skin->rotations * 9, rotation);
        for (int b = 0; b < bones; ++b) {
            if (!hidden[b])
                continue;
            for (int i = 0; i < 3; ++i)
                position[b * 3 + i] = static_cast<GLfloat>(eye[i]);
            for (int i = 0; i < 9; ++i)
                rotation[b * 9 + i] *= kHiddenBoneScale;
        }
        UploadSkinnedVectors(skin->positionLoc, skin->positions, position, skin->positionArb);
        UploadSkinnedRotations(skin->rotationLoc, skin->rotations, skin->transposed, rotation, skin->rotationArb);
        std::copy(position, position + skin->positions * 3, skin->hiddenPosition);
        std::copy(rotation, rotation + skin->rotations * 9, skin->hiddenRotation);
        skin->positionEdited = skin->rotationEdited = true;
        if (!g_hidingLogged) {
            g_hidingLogged = true;
            int count = 0;
            for (int b = 0; b < bones; ++b)
                count += hidden[b] ? 1 : 0;
            Log("rider: hiding your torso and head (%d of %d bones; the eye sits %+.2f %+.2f %+.2f from the torso bone)",
                count, bones, rest[0], rest[1], rest[2]);
        }
        return;
    }
    if (skin->positionEdited)
        UploadSkinnedVectors(skin->positionLoc, skin->positions, skin->position, skin->positionArb);
    if (skin->rotationEdited)
        UploadSkinnedRotations(skin->rotationLoc, skin->rotations, skin->transposed, skin->rotation, skin->rotationArb);
    skin->positionEdited = skin->rotationEdited = false;
}

// ---- Wrappers ----------------------------------------------------------------------------
void APIENTRY Wrap_glUseProgram(GLuint program)
{
    NoteUseProgram(program);
    NoteUniformProgram(program);
    DumpCall("glUseProgram(%u)", program);
    real_glUseProgram(program);
}

void APIENTRY Wrap_glUseProgramObjectARB(GLuint program)
{
    NoteUseProgram(program);
    NoteUniformProgram(program);
    DumpCall("glUseProgramObjectARB(%u)", program);
    real_glUseProgramObjectARB(program);
}

GLint APIENTRY Wrap_glGetUniformLocation(GLuint program, const GLchar* name)
{
    const GLint location = real_glGetUniformLocation(program, name);
    NoteUniformLocation(program, name, location);
    return location;
}

GLint APIENTRY Wrap_glGetUniformLocationARB(GLuint program, const GLchar* name)
{
    const GLint location = real_glGetUniformLocationARB(program, name);
    NoteUniformLocation(program, name, location);
    return location;
}

void APIENTRY Wrap_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    DumpUniform("glUniformMatrix4fv", g_program, location, count, transpose, value, 16);
    GLfloat scratch[16];
    const GLfloat* uploaded = CorrectModelview(g_program, location, count, transpose, value, scratch);
    uploaded = NoteSkinnedView(g_program, location, count, transpose, value, uploaded, scratch);
    NoteSceneMatrix(g_program, location, count, transpose, value, uploaded);
    real_glUniformMatrix4fv(location, count, transpose, uploaded);
}

void APIENTRY Wrap_glUniformMatrix4fvARB(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    DumpUniform("glUniformMatrix4fvARB", g_program, location, count, transpose, value, 16);
    GLfloat scratch[16];
    const GLfloat* uploaded = CorrectModelview(g_program, location, count, transpose, value, scratch);
    uploaded = NoteSkinnedView(g_program, location, count, transpose, value, uploaded, scratch);
    NoteSceneMatrix(g_program, location, count, transpose, value, uploaded);
    real_glUniformMatrix4fvARB(location, count, transpose, uploaded);
}

void APIENTRY Wrap_glProgramUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    DumpUniform("glProgramUniformMatrix4fv", program, location, count, transpose, value, 16);
    GLfloat scratch[16];
    const GLfloat* uploaded = CorrectModelview(program, location, count, transpose, value, scratch);
    uploaded = NoteSkinnedView(program, location, count, transpose, value, uploaded, scratch);
    NoteSceneMatrix(program, location, count, transpose, value, uploaded);
    real_glProgramUniformMatrix4fv(program, location, count, transpose, uploaded);
}

// Light directions (and positions) arrive in the game's eye space; returns them turned into
// ours, in scratch, or value itself if this isn't a light uniform. Directions (vec3, or vec4 with
// w = 0) get the rotation only; a vec4 position with w != 0 gets the full correction.
constexpr GLsizei kMaxLights = 16;
// The time of day (postfx.h) can turn the sun first: the game's value is replaced, in the game's
// eye space, before the correction.
const GLfloat* CorrectLight(GLint location, GLsizei count, const GLfloat* value, int components, GLfloat* scratch)
{
    if (!value || count <= 0 || count > kMaxLights)
        return value;
    AcquireSRWLockShared(&g_lock);
    const bool isLight = g_lightUniforms.count(UniformKey(g_program, location)) != 0;
    ReleaseSRWLockShared(&g_lock);
    if (!isLight)
        return value;

    postfx::NoteGameLight(value, components);
    GLfloat timed[kMaxLights * 4];
    if (postfx::OverrideLight(value, count, components, timed))
        value = timed;
    if (!Correcting(GetConfig()) || !g_camera.haveCorrection) {
        if (value != timed)
            return value;
        std::copy(timed, timed + count * components, scratch);
        return scratch;
    }

    for (GLsizei i = 0; i < count; ++i) {
        const GLfloat* v = value + i * components;
        GLfloat* out = scratch + i * components;
        const double w = components == 4 ? v[3] : 0.0;
        const double* m = w != 0 ? g_camera.correction : g_camera.rotationOnly;
        for (int r = 0; r < 3; ++r)
            out[r] = static_cast<GLfloat>(m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * w);
        if (components == 4)
            out[3] = v[3];
    }
    return scratch;
}

// The environment-map matrix turns eye-space reflection vectors into world space: it is the
// transpose of the view rotation. It's uploaded before the sky, so it can also supply this
// frame's camera rotation. Ours: (C R)^T = R^T C^T = u_envmat * C^T.
const GLfloat* CorrectEnvMatrix(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value, GLfloat* scratch)
{
    if (!Correcting(GetConfig()) || !value || count != 1 || transpose)
        return value;
    AcquireSRWLockShared(&g_lock);
    const bool isEnv = g_envUniforms.count(UniformKey(g_program, location)) != 0;
    ReleaseSRWLockShared(&g_lock);
    if (!isEnv)
        return value;

    double view[16] = {};  // R = u_envmat^T as a rotation-only view: R(r, c) = value[r * 3 + c]
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            view[c * 4 + r] = value[r * 3 + c];
    view[15] = 1;
    ConsiderSkyRotation(view);
    if (!g_camera.haveCorrection)
        return value;
    const double* m = g_camera.capturedThisFrame ? g_camera.correction : g_camera.rotationOnly;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            double sum = 0;  // (E C^T)(r, c) = sum_k E(r, k) C(c, k)
            for (int k = 0; k < 3; ++k)
                sum += value[k * 3 + r] * m[k * 4 + c];
            scratch[c * 3 + r] = static_cast<GLfloat>(sum);
        }
    }
    return scratch;
}

// Some engines upload matrices as arrays of vec4.
void APIENTRY Wrap_glUniform4fv(GLint location, GLsizei count, const GLfloat* value)
{
    DumpUniform("glUniform4fv", g_program, location, count, 0, value, 4);
    GLfloat scratch[kMaxLights * 4];
    const GLfloat* up = CorrectLight(location, count, value, 4, scratch);
    NoteUniformValue(location, count, 5, up, 0);
    real_glUniform4fv(location, count, up);
}

void APIENTRY Wrap_glUniform4fvARB(GLint location, GLsizei count, const GLfloat* value)
{
    DumpUniform("glUniform4fvARB", g_program, location, count, 0, value, 4);
    GLfloat scratch[kMaxLights * 4];
    real_glUniform4fvARB(location, count, CorrectLight(location, count, value, 4, scratch));
}

// Bone positions: for the headlight (your own rider, in any camera) and the chase camera in
// replays (only looked at during one).
void ObserveBones(GLint location, GLsizei count, const GLfloat* value)
{
    if (!value)
        return;
    AcquireSRWLockShared(&g_lock);
    const bool isBones = g_boneUniforms.count(UniformKey(g_program, location)) != 0;
    ReleaseSRWLockShared(&g_lock);
    if (isBones && replay::Current() != replay::Mode::Off)
        replay::ObserveBones(value, count);
}

void APIENTRY Wrap_glUniform3fv(GLint location, GLsizei count, const GLfloat* value)
{
    DumpUniform("glUniform3fv", g_program, location, count, 0, value, 3);
    ObserveBones(location, count, value);
    NoteSkinnedVectors(location, count, value, false);
    GLfloat scratch[kMaxLights * 3];
    const GLfloat* up = CorrectLight(location, count, value, 3, scratch);
    NoteUniformValue(location, count, 4, up, 0);
    real_glUniform3fv(location, count, up);
}

void APIENTRY Wrap_glUniform3fvARB(GLint location, GLsizei count, const GLfloat* value)
{
    DumpUniform("glUniform3fvARB", g_program, location, count, 0, value, 3);
    ObserveBones(location, count, value);
    NoteSkinnedVectors(location, count, value, true);
    GLfloat scratch[kMaxLights * 3];
    real_glUniform3fvARB(location, count, CorrectLight(location, count, value, 3, scratch));
}

void APIENTRY Wrap_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    DumpUniform("glUniformMatrix3fv", g_program, location, count, transpose, value, 9);
    NoteSkinnedRotations(location, count, transpose, value, false);
    GLfloat scratch[9];
    real_glUniformMatrix3fv(location, count, transpose, CorrectEnvMatrix(location, count, transpose, value, scratch));
}

void APIENTRY Wrap_glUniformMatrix3fvARB(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    DumpUniform("glUniformMatrix3fvARB", g_program, location, count, transpose, value, 9);
    NoteSkinnedRotations(location, count, transpose, value, true);
    GLfloat scratch[9];
    real_glUniformMatrix3fvARB(location, count, transpose, CorrectEnvMatrix(location, count, transpose, value, scratch));
}

// ---- Terrain patches (see terrain.h) -----------------------------------------------------

// The terrain program's patch uniforms (and the heightmap's, uploaded when a track loads).
void NoteTerrainUniform(GLint location, GLsizei count, const GLfloat* value, int components)
{
    const TerrainUniforms& u = g_terrain.uniforms;
    if (!value || count != 1 || location < 0)
        return;
    if (location == u.spacing || location == u.heightsDim) {
        float (&last)[2] = g_heightmapUpload[location == u.spacing ? 0 : 1];
        const float second = components > 1 ? value[1] : 0.0f;
        if (last[0] != value[0] || last[1] != second) {
            last[0] = value[0];
            last[1] = second;
            terrain::ResetMap();
            postfx::MarksNewTrack();  // the ground of the marks is another track's as well
            g_variants.clear();
            g_loggedBigCasters = 0;  // (another track: log what's left out of it too)
            g_stadiumTrack = false;
            Log("terrain: %s = %g %g (a new heightmap)", location == u.spacing ? kSpacingUniform : kHeightsDimUniform,
                value[0], second);
            // The patches are whole numbers of samples; samples apart / 1 over samples across: the
            // map's width.
            terrain::SetGridUnit(g_heightmapUpload[0][0]);
            if (g_heightmapUpload[0][0] > 0 && g_heightmapUpload[1][0] > 0)
                terrain::SetMapSize(g_heightmapUpload[0][0] / g_heightmapUpload[1][0]);
        }
        return;
    }
    if (components == 2 && location == u.offset) {
        g_terrain.current.offset[0] = value[0];
        g_terrain.current.offset[1] = value[1];
    } else if (components == 2 && location == u.texOffset) {
        g_terrain.current.texOffset[0] = value[0];
        g_terrain.current.texOffset[1] = value[1];
    } else if (components == 1 && location == u.scale) {
        g_terrain.current.scale = value[0];
    } else if (components == 1 && location == u.texScale) {
        g_terrain.current.texScale = value[0];
    }
}

bool SamePatch(const terrain::Patch& a, const terrain::Patch& b)
{
    return a.offset[0] == b.offset[0] && a.offset[1] == b.offset[1] && a.scale == b.scale &&
           a.texOffset[0] == b.texOffset[0] && a.texOffset[1] == b.texOffset[1] && a.texScale == b.texScale;
}

// The vertex array most of this pass's patches were drawn with: for filled cells whose
// stitching has no known variant yet.
GLuint CommonVariant()
{
    std::unordered_map<GLuint, int> uses;
    GLuint best = 0;
    int bestUses = 0;
    for (const GamePatch& patch : g_terrain.patches) {
        if (!patch.replayable || patch.drawCount == 0)
            continue;
        const int n = ++uses[patch.vao];
        if (n > bestUses) {
            bestUses = n;
            best = patch.vao;
        }
    }
    return best;
}

void UploadPatch(const terrain::Patch& patch)
{
    const TerrainUniforms& u = g_terrain.uniforms;
    real_glUniform2fv(u.offset, 1, patch.offset);
    real_glUniform1f(u.scale, patch.scale);
    if (u.texOffset >= 0)
        real_glUniform2fv(u.texOffset, 1, patch.texOffset);
    if (u.texScale >= 0)
        real_glUniform1f(u.texScale, patch.texScale);
}

void Replay(const DrawCall& draw)
{
    switch (draw.kind) {
    case DrawCall::Elements:
        if (real_glDrawElements)
            real_glDrawElements(draw.mode, draw.count, draw.type, draw.indices);
        break;
    case DrawCall::Arrays:
        if (real_glDrawArrays)
            real_glDrawArrays(draw.mode, draw.first, draw.count);
        break;
    case DrawCall::RangeElements:
        if (real_glDrawRangeElements)
            real_glDrawRangeElements(draw.mode, draw.start, draw.end, draw.count, draw.type, draw.indices);
        break;
    }
}

// Draws the cells the game culled, after its own patches and with the same draw calls, each
// with the vertex array variant stitched to its neighbours.
void FillTerrain()
{
    if (!real_glDrawElements)
        real_glDrawElements = reinterpret_cast<PFN_DrawElements>(GetRealGLFunction("glDrawElements"));
    if (!real_glDrawArrays)
        real_glDrawArrays = reinterpret_cast<PFN_DrawArrays>(GetRealGLFunction("glDrawArrays"));
    if (!real_glUniform2fv || !real_glUniform1f)
        return;

    const int filled = terrain::Fill(g_terrain.eye, g_terrain.forward, g_filled, kMaxFilledPatches);
    g_ground.note = terrain::LastFillNote();
    std::sort(g_filled, g_filled + filled,
              [](const terrain::Patch& a, const terrain::Patch& b) { return a.variant < b.variant; });
    const GLuint common = CommonVariant();
    GLuint bound = g_vao;
    int drawn = 0, stitched = 0;
    for (int i = 0; i < filled; ++i) {
        const terrain::Patch& cell = g_filled[i];
        auto found = g_variants.find(cell.variant >= 0 ? static_cast<GLuint>(cell.variant) : common);
        if (found == g_variants.end())
            found = g_variants.find(common);
        if (found == g_variants.end())
            continue;
        const GamePatch& variant = found->second;
        if (variant.vao != bound) {
            if (!real_glBindVertexArray)
                continue;
            real_glBindVertexArray(variant.vao);
            bound = variant.vao;
        }
        UploadPatch(cell);
        for (int d = 0; d < variant.drawCount; ++d)
            Replay(variant.draws[d]);
        ++drawn;
        stitched += cell.edges != 0 && cell.variant >= 0 ? 1 : 0;
    }
    g_ground.filled += drawn;
    if (drawn > 0)
        UploadPatch(g_terrain.current);
    if (bound != g_vao)
        real_glBindVertexArray(g_vao);
    DumpCall("^ TERRAIN  %d patches drawn by the game, %d filled in behind its camera (%d stitched to bigger neighbours); "
             "%d vertex array(s) seen, %d edge combination(s) learnt",
             static_cast<int>(g_terrain.patches.size()), drawn, stitched, static_cast<int>(g_variants.size()),
             terrain::LearntEdgeMasks());
}

// The game is done with the terrain program: the map learns from its patches, and in a 3rd
// person replay or a d-pad look the cells it culled are drawn.
void FinishTerrainPass()
{
    if (g_terrain.patches.empty())
        return;
    if (g_program != g_mapProgram) {
        terrain::ResetMap();
        // The whole terrain was built again by another program: the ground is not the one the marks
        // were pressed in, however equal its heightmap's numbers came in (a track whose spacing and
        // width match the one before is otherwise missed).
        postfx::MarksNewTrack();
        g_variants.clear();
        g_mapProgram = g_program;
    }
    terrain::BeginPass();
    for (const GamePatch& game : g_terrain.patches) {
        terrain::Patch patch = game.patch;
        if (game.replayable && game.drawCount > 0) {
            patch.variant = static_cast<int>(game.vao);
            g_variants[game.vao] = game;
        }
        terrain::AddGamePatch(patch);
    }

    ++g_ground.passes;
    g_ground.game += static_cast<int>(g_terrain.patches.size());
    const bool wanted = replay::Corrects(replay::Current()) || g_terrain.lookingAway;
    if (g_variants.empty()) {
        g_ground.note = "no-variants";
        DumpCall("^ TERRAIN  can't be filled in (indices from client memory, or too many draw calls per patch)");
    } else if (!Correcting(GetConfig()) || !g_camera.haveCorrection) {
        g_ground.note = "off";
    } else if (!g_terrain.haveEye) {
        g_ground.note = "no-eye";
    } else if (!wanted) {
        g_ground.note = "ahead";
    } else {
        FillTerrain();
    }
    g_terrain.patches.clear();
    postfx::TerrainPassDone();  // the depth buffer holds the ground (and the sky) only
}

// Patch uploads are logged in the F10 dump.
void APIENTRY Wrap_glUniform2fv(GLint location, GLsizei count, const GLfloat* value)
{
    DumpUniform("glUniform2fv", g_program, location, count, 0, value, 2);
    NoteTerrainUniform(location, count, value, 2);
    NoteUniformValue(location, count, 3, value, 0);
    real_glUniform2fv(location, count, value);
}

void APIENTRY Wrap_glUniform1f(GLint location, GLfloat v0)
{
    if (IsDumping())
        DumpCall("glUniform1f prog=%u loc=%d '%s' [%.3f]", g_program, location, UniformName(g_program, location).c_str(), v0);
    NoteTerrainUniform(location, 1, &v0, 1);
    NoteUniformValue(location, 1, 2, &v0, 0);
    real_glUniform1f(location, v0);
}

void APIENTRY Wrap_glUniform1i(GLint location, GLint v0)
{
    if (IsDumping())
        DumpCall("glUniform1i prog=%u loc=%d '%s' [%d]", g_program, location, UniformName(g_program, location).c_str(), v0);
    NoteSkinnedCount(location, v0);
    NoteUniformValue(location, 1, 1, nullptr, v0);
    real_glUniform1i(location, v0);
}

void APIENTRY Wrap_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices)
{
    if (IsDumping() && !g_terrain.isTerrain)
        DumpCall("glDrawRangeElements prog=%u mode=0x%X range=%u..%u count=%d", g_program, mode, start, end, count);
    DrawCall draw;
    draw.kind = DrawCall::RangeElements;
    draw.mode = mode;
    draw.start = start;
    draw.end = end;
    draw.count = count;
    draw.type = type;
    draw.indices = indices;
    if (NoteDraw(draw))
        real_glDrawRangeElements(mode, start, end, count, type, indices);
}

void APIENTRY Wrap_glBindVertexArray(GLuint array)
{
    g_vao = array;
    real_glBindVertexArray(array);
}

// The element buffer belongs to the bound vertex array: a terrain draw can only be replayed
// if its indices come from one.
void APIENTRY Wrap_glBindBuffer(GLenum target, GLuint buffer)
{
    if (target == kElementArrayBuffer) {
        GLuint& was = g_elementBuffers[g_vao];
        if (was != buffer)
            g_vaoChangedFrame[g_vao] = g_frameNumber;
        was = buffer;
    }
    else if (target == kArrayBufferTarget)
        g_arrayBuffer = buffer;
    real_glBindBuffer(target, buffer);
}

// Sampler objects override their textures' filtering: texture filtering covers them too.
void APIENTRY Wrap_glSamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
    if (texfilter::Watches(pname))
        param = static_cast<GLint>(texfilter::SamplerParameter(sampler, pname, static_cast<float>(param)));
    real_glSamplerParameteri(sampler, pname, param);
}

void APIENTRY Wrap_glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param)
{
    if (texfilter::Watches(pname))
        param = texfilter::SamplerParameter(sampler, pname, param);
    real_glSamplerParameterf(sampler, pname, param);
}

void APIENTRY Wrap_glDeleteSamplers(GLsizei count, const GLuint* samplers)
{
    texfilter::SamplersDeleted(count, samplers);
    real_glDeleteSamplers(count, samplers);
}

// DumpShaders=1: every shader source the game compiles goes to MXS_REMASTERED_shaders\shader_<id>.glsl.
// ---- Tire dirt in the game's own drawing of your tires -----------------------------------------
// The mud goes into the bike shader itself: its vertex shader passes each point's place in its own
// model on (a wheel's turns with it), and its fragment shader mixes mud onto the dark rubber of the
// tire's own texture (a dusting on metal) when u_mx_mud is set, which it is only while your wheels
// are drawn (see PaintTireDirt). Nothing on screen is guessed: it can't land on the ground, the
// disc or another bike. A shader that doesn't compile changed is handed over unchanged.
constexpr char kMudVertexDecl[] = "out vec3 mx_pos;\n";
constexpr char kMudFragmentDecl[] =
    "in vec3 mx_pos;\n"
    "uniform float u_mx_mud;\n"
    "uniform float u_mx_metre;\n"
    "uniform float u_mx_snow;\n"
    "highp float mx_hash(highp vec3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)); }\n"
    "highp float mx_noise(highp vec3 x) {\n"
    "    highp vec3 i = floor(x); highp vec3 f = fract(x); f = f * f * (3.0 - 2.0 * f);\n"
    "    return mix(mix(mix(mx_hash(i), mx_hash(i + vec3(1.0, 0.0, 0.0)), f.x),\n"
    "                   mix(mx_hash(i + vec3(0.0, 1.0, 0.0)), mx_hash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),\n"
    "               mix(mix(mx_hash(i + vec3(0.0, 0.0, 1.0)), mx_hash(i + vec3(1.0, 0.0, 1.0)), f.x),\n"
    "                   mix(mx_hash(i + vec3(0.0, 1.0, 1.0)), mx_hash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);\n"
    "}\n";
// (The game's colours are not linear: mud in its terms.)
constexpr char kMudFragmentApply[] =
    "\tif (u_mx_mud > 0.0) {\n"
    "\t\tvec4 mx_albedo = texture(u_tex, interp_texcoord);\n"
    "\t\tfloat mx_rubber = 1.0 - smoothstep(0.35, 0.55, dot(mx_albedo.rgb, vec3(0.2126, 0.7152, 0.0722)));\n"
    "\t\thighp vec3 mx_q = mx_pos * (13.0 / u_mx_metre);\n"
    "\t\tfloat mx_n = mx_noise(mx_q) * 0.6 + mx_noise(mx_q * 2.4 + 7.0) * 0.4;\n"
    "\t\tfloat mx_edge = 0.75 - 0.6 * u_mx_mud;\n"
    "\t\tfloat mx_cover = mx_rubber * smoothstep(mx_edge, mx_edge + 0.1, mx_n);\n"
    "\t\tmx_cover = max(mx_cover, (1.0 - mx_rubber) * 0.3 * u_mx_mud * smoothstep(0.5, 0.75, mx_n));\n"
    "\t\tvec3 mx_mud = mix(vec3(0.24, 0.18, 0.13), vec3(0.37, 0.29, 0.21), mx_noise(mx_q * 0.45 + 3.0));\n"
    "\t\tif (u_mx_snow > 0.0) {\n"
    "\t\t\tfloat mx_fleck = smoothstep(0.82, 0.88, mx_noise(mx_q * 3.3 + 11.0));\n"
    "\t\t\tfloat mx_lump = smoothstep(0.88, 0.93, mx_noise(mx_q * 1.2 + 23.0));\n"
    "\t\t\tmx_mud = mix(mx_mud, vec3(0.86, 0.88, 0.92), max(mx_fleck, mx_lump) * 0.85);\n"
    "\t\t}\n"
    "\t\tcolor.rgb = mix(color.rgb, mx_mud * (u_ambient.rgb + u_diffuse.rgb * ndl), mx_cover);\n"
    "\t}\n";
bool g_loggedMudShader[2] = { false, false };

// Whether the changed source compiles (on a shader of our own; the game's is untouched until then).
bool CompilesAs(GLenum type, const std::string& source)
{
    using PFN_CreateShader = GLuint(APIENTRY*)(GLenum);
    using PFN_CompileShader = void(APIENTRY*)(GLuint);
    using PFN_GetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
    using PFN_DeleteShader = void(APIENTRY*)(GLuint);
    static const auto create = reinterpret_cast<PFN_CreateShader>(GetRealGLFunction("glCreateShader"));
    static const auto compile = reinterpret_cast<PFN_CompileShader>(GetRealGLFunction("glCompileShader"));
    static const auto get = reinterpret_cast<PFN_GetShaderiv>(GetRealGLFunction("glGetShaderiv"));
    static const auto remove = reinterpret_cast<PFN_DeleteShader>(GetRealGLFunction("glDeleteShader"));
    if (!create || !compile || !get || !remove || !real_glShaderSource)
        return false;
    const GLuint test = create(type);
    if (!test)
        return false;
    const GLchar* text = source.c_str();
    real_glShaderSource(test, 1, &text, nullptr);
    compile(test);
    GLint ok = 0;
    get(test, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    remove(test);
    return ok != 0;
}

// The bike shaders, changed for the tire dirt (empty: not one of them, or it didn't compile).
std::string MudShader(GLuint shader, const std::string& source)
{
    const bool vertex = source.find("u_bone_t") != std::string::npos && source.find("vattr_position") != std::string::npos &&
                        source.find("interp_texcoord = vattr_texcoord;") != std::string::npos &&
                        source.find("out vec2 interp_texcoord;") != std::string::npos;
    const bool fragment = source.find("uniform sampler2D u_tex, u_nmap, u_smap;") != std::string::npos &&
                          source.find("z = gl_FragCoord.z / gl_FragCoord.w;") != std::string::npos &&
                          source.find("out vec4 f_fragcolor;") != std::string::npos &&
                          source.find("ndl = dot(n, light);") != std::string::npos;
    if (!vertex && !fragment)
        return std::string();
    std::string changed = source;
    if (vertex) {
        changed.insert(changed.find("out vec2 interp_texcoord;") + strlen("out vec2 interp_texcoord;") + 1, kMudVertexDecl);
        const size_t at = changed.find("interp_texcoord = vattr_texcoord;");
        changed.insert(at + strlen("interp_texcoord = vattr_texcoord;"), "\n\tmx_pos = vattr_position;");
    } else {
        changed.insert(changed.find("out vec4 f_fragcolor;") + strlen("out vec4 f_fragcolor;") + 1, kMudFragmentDecl);
        changed.insert(changed.find("\tz = gl_FragCoord.z / gl_FragCoord.w;"), kMudFragmentApply);
    }
    using PFN_GetShaderiv = void(APIENTRY*)(GLuint, GLenum, GLint*);
    static const auto get = reinterpret_cast<PFN_GetShaderiv>(GetRealGLFunction("glGetShaderiv"));
    GLint type = vertex ? 0x8B31 : 0x8B30;  // GL_VERTEX_SHADER / GL_FRAGMENT_SHADER
    if (get)
        get(shader, 0x8B4F /* GL_SHADER_TYPE */, &type);
    const bool ok = CompilesAs(static_cast<GLenum>(type), changed);
    bool& logged = g_loggedMudShader[vertex ? 0 : 1];
    if (!logged) {
        logged = true;
        Log("tire dirt: the bike's %s shader %s", vertex ? "vertex" : "fragment",
            ok ? "carries the tire dirt (drawn on your tires themselves)" : "didn't compile changed: left as it was, no tire dirt");
    }
    return ok ? changed : std::string();
}

void APIENTRY Wrap_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
    if (strings && count > 0) {
        std::string source;
        for (GLsizei i = 0; i < count; ++i)
            if (strings[i])
                source.append(strings[i], lengths && lengths[i] >= 0 ? static_cast<size_t>(lengths[i]) : strlen(strings[i]));
        const std::string changed = MudShader(shader, source);
        if (!changed.empty()) {
            const GLchar* text = changed.c_str();
            real_glShaderSource(shader, 1, &text, nullptr);
            return;
        }
    }
    if (GetConfig().dumpShaders && strings && count > 0) {
        wchar_t path[1024];
        swprintf_s(path, L"%sMXS_REMASTERED_shaders", LogDirectory());
        CreateDirectoryW(path, nullptr);
        swprintf_s(path, L"%sMXS_REMASTERED_shaders\\shader_%u.glsl", LogDirectory(), shader);
        if (FILE* file = _wfsopen(path, L"wb", _SH_DENYWR)) {
            for (GLsizei i = 0; i < count; ++i) {
                if (!strings[i])
                    continue;
                const size_t length = lengths && lengths[i] >= 0 ? static_cast<size_t>(lengths[i]) : strlen(strings[i]);
                fwrite(strings[i], 1, length, file);
            }
            fclose(file);
            Log("shaders: wrote shader %u's source to MXS_REMASTERED_shaders", shader);
        }
    }
    real_glShaderSource(shader, count, strings, lengths);
}

// ---- Tracking for the shadow casters' batch ---------------------------------------------------
void APIENTRY Wrap_glActiveTexture(GLenum texture)
{
    g_activeUnit = static_cast<int>(texture - kTexture0Unit);
    real_glActiveTexture(texture);
}

// A buffer rewritten in two or more frames holds streamed geometry: what it held at a draw call may
// be gone by the end of the frame, so its draws are cast at once, not held back.
void NoteBufferUpdate(GLenum target)
{
    GLuint buffer = 0;
    if (target == kArrayBufferTarget) {
        buffer = g_arrayBuffer;
    } else if (target == kElementArrayBuffer) {
        const auto found = g_elementBuffers.find(g_vao);
        buffer = found != g_elementBuffers.end() ? found->second : 0;
    }
    if (!buffer)
        return;
    BufferUpdates& u = g_bufferUpdates[buffer];
    if (u.lastFrame != g_frameNumber) {
        u.lastFrame = g_frameNumber;
        ++u.frames;
    }
}

std::unordered_map<GLuint, unsigned> g_bufferGeneration;  // buffer -> times its contents were given (or it was deleted)
std::unordered_map<GLuint, unsigned> g_vaoGeneration;     // vertex array -> times it was deleted or re-pointed

void NoteBufferContents(GLenum target)
{
    GLuint buffer = 0;
    if (target == kArrayBufferTarget) {
        buffer = g_arrayBuffer;
    } else if (target == kElementArrayBuffer) {
        const auto found = g_elementBuffers.find(g_vao);
        buffer = found != g_elementBuffers.end() ? found->second : 0;
    }
    if (buffer)
        ++g_bufferGeneration[buffer];
}

void APIENTRY Wrap_glBufferData(GLenum target, ptrdiff_t size, const void* data, GLenum usage)
{
    NoteBufferUpdate(target);
    NoteBufferContents(target);
    real_glBufferData(target, size, data, usage);
}

void APIENTRY Wrap_glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (index < static_cast<GLuint>(kConstAttribs)) {
        GLfloat* a = g_constAttrib[index];
        a[0] = x;
        a[1] = y;
        a[2] = z;
        a[3] = w;
        if (!(g_constAttribSet & (1u << index))) {
            g_constAttribSet |= 1u << index;
            Log("proc: the game gives vertex attribute %u a constant value (%.3f %.3f %.3f %.3f): kept with each "
                "shadow caster held back", index, x, y, z, w);
        }
    }
    real_glVertexAttrib4f(index, x, y, z, w);
}

void APIENTRY Wrap_glBufferSubData(GLenum target, ptrdiff_t offset, ptrdiff_t size, const void* data)
{
    NoteBufferUpdate(target);
    NoteBufferContents(target);
    real_glBufferSubData(target, offset, size, data);
}

void APIENTRY Wrap_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                         const void* pointer)
{
    if (g_vao) {
        std::vector<GLuint>& buffers = g_vaoArrayBuffers[g_vao];
        if (std::find(buffers.begin(), buffers.end(), g_arrayBuffer) == buffers.end())
            buffers.push_back(g_arrayBuffer);
        if (index < 16) {
            AttribPointer& a = g_vaoAttribs[static_cast<unsigned long long>(g_vao) << 4 | index];
            if (a.buffer != g_arrayBuffer || a.offset != reinterpret_cast<uintptr_t>(pointer)) {
                ++g_vaoGeneration[g_vao];
                g_vaoChangedFrame[g_vao] = g_frameNumber;
            }
            a.buffer = g_arrayBuffer;
            a.size = size;
            a.type = type;
            a.stride = stride;
            a.offset = reinterpret_cast<uintptr_t>(pointer);
        }
    }
    real_glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}

// A kept wheel's draw call (KeepOrCastWheels) can't outlive its vertex array or buffers.
void DropKeptWheels(GLsizei count, const GLuint* names, bool vertexArrays)
{
    if (count <= 0 || !names)
        return;
    for (KeptWheel& kept : g_keptWheels) {
        if (!kept.valid)
            continue;
        for (GLsizei n = 0; n < count && kept.valid; ++n) {
            if (names[n] == 0)
                continue;
            if (vertexArrays ? names[n] == kept.caster.vao
                             : std::find(kept.buffers.begin(), kept.buffers.end(), names[n]) != kept.buffers.end())
                kept.valid = false;
        }
    }
    for (KeptRiderPiece& piece : g_riderPieces) {
        for (GLsizei n = 0; n < count && piece.used; ++n) {
            if (names[n] == 0)
                continue;
            if (vertexArrays ? names[n] == piece.caster.vao
                             : std::find(piece.buffers.begin(), piece.buffers.end(), names[n]) != piece.buffers.end())
                piece.used = false;
        }
    }
    for (RiddenPart& part : g_riddenParts) {
        for (GLsizei n = 0; n < count && part.used; ++n) {
            if (names[n] == 0)
                continue;
            if (vertexArrays ? names[n] == part.caster.vao
                             : std::find(part.buffers.begin(), part.buffers.end(), names[n]) != part.buffers.end())
                part.used = false;
        }
    }
}

void APIENTRY Wrap_glDeleteBuffers(GLsizei count, const GLuint* names)
{
    DropKeptWheels(count, names, false);
    for (GLsizei n = 0; names && n < count; ++n) {
        ++g_bufferGeneration[names[n]];  // (its name may be given out again: what was read back from it is old)
        g_bufferUpdates.erase(names[n]);  // (and filled once more then: not something rewritten while running)
    }
    real_glDeleteBuffers(count, names);
}

void APIENTRY Wrap_glDeleteVertexArrays(GLsizei count, const GLuint* names)
{
    DropKeptWheels(count, names, true);
    for (GLsizei n = 0; names && n < count; ++n) {
        ++g_vaoGeneration[names[n]];
        g_vaoArrayBuffers.erase(names[n]);
        for (unsigned index = 0; index < 16; ++index)
            g_vaoAttribs.erase(static_cast<unsigned long long>(names[n]) << 4 | index);
    }
    real_glDeleteVertexArrays(count, names);
}

struct ProcWrapper {
    const char* name;
    void*       wrapper;
    void**      realSlot;
};

#define MXCAM_WRAP(fn) { #fn, reinterpret_cast<void*>(&Wrap_##fn), reinterpret_cast<void**>(&real_##fn) }

const ProcWrapper kWrappers[] = {
    MXCAM_WRAP(glUseProgram),              MXCAM_WRAP(glUseProgramObjectARB),
    MXCAM_WRAP(glGetUniformLocation),      MXCAM_WRAP(glGetUniformLocationARB),
    MXCAM_WRAP(glUniformMatrix4fv),        MXCAM_WRAP(glUniformMatrix4fvARB),
    MXCAM_WRAP(glProgramUniformMatrix4fv),
    MXCAM_WRAP(glUniform4fv),              MXCAM_WRAP(glUniform4fvARB),
    MXCAM_WRAP(glUniform3fv),              MXCAM_WRAP(glUniform3fvARB),
    MXCAM_WRAP(glUniformMatrix3fv),        MXCAM_WRAP(glUniformMatrix3fvARB),
    MXCAM_WRAP(glUniform2fv),              MXCAM_WRAP(glUniform1f),
    MXCAM_WRAP(glUniform1i),
    MXCAM_WRAP(glDrawRangeElements),
    MXCAM_WRAP(glBindVertexArray),         MXCAM_WRAP(glBindBuffer),
    MXCAM_WRAP(glSamplerParameteri),       MXCAM_WRAP(glSamplerParameterf),
    MXCAM_WRAP(glDeleteSamplers),          MXCAM_WRAP(glShaderSource),
    MXCAM_WRAP(glActiveTexture),           MXCAM_WRAP(glBufferData),
    MXCAM_WRAP(glBufferSubData),           MXCAM_WRAP(glVertexAttribPointer),
    MXCAM_WRAP(glVertexAttrib4f),
    MXCAM_WRAP(glDeleteBuffers),           MXCAM_WRAP(glDeleteVertexArrays),
};

}  // namespace

void* WrapProc(const char* name, void* real)
{
    // wglGetProcAddress only takes names, but guard against an ordinal-style pointer anyway.
    if (reinterpret_cast<ULONG_PTR>(name) <= 0xFFFF)
        return real;

    const ProcWrapper* match = nullptr;
    for (const ProcWrapper& w : kWrappers) {
        if (strcmp(w.name, name) == 0) {
            match = &w;
            break;
        }
    }

    AcquireSRWLockExclusive(&g_lock);
    const bool firstTime = g_fetched.insert(name).second;
    ReleaseSRWLockExclusive(&g_lock);
    if (firstTime)
        Log("proc: %s%s", name, !real ? " (not available)" : match ? " (wrapped)" : "");

    if (!match || !real)
        return real;
    *match->realSlot = real;
    return match->wrapper;
}

void StartTrace(double seconds)
{
    if (g_trace)
        return;
    wchar_t path[1024];
    swprintf_s(path, L"%sMXS_REMASTERED_trace.csv", LogDirectory());
    postfx::RequestMarksDump();  // (and a picture of the tire marks map around you)
    g_trace = _wfsopen(path, L"w", _SH_DENYWR);
    if (!g_trace) {
        Log("trace: could not create %ls", path);
        return;
    }
    fprintf(g_trace, "time_ms,interval_ms,step_ms,sky_previews,other_rotations,cut,reset,bike,bike_fresh,custom,"
                     "raw_yaw,raw_pitch,raw_roll,bike_yaw,bike_pitch,bike_roll,view_yaw,view_pitch,view_roll,"
                     "lag_deg,look_yaw,air_yaw,apex_deg,air,fall_g,accel_up_g,neck_mm,"
                     "cam_x,cam_y,cam_z,eye_dx,eye_dy,eye_dz,bike_x,bike_y,bike_z,"
                     "away_deg,ground_passes,ground_game,ground_filled,ground_open,ground_note,"
                     "rear_on,rear_seen,rear_far,rear_x,rear_y,rear_z,front_on,front_seen,front_far,front_x,front_y,front_z,"
                     "tire_stretch,bike_found,rear_grounded,front_grounded,lobby_segs,lobby_yours,lobby_clear,lobby_step,"
                     "lobby_outside,lobby_pressed,lobby_near_rear_m,lobby_near_front_m,"
                     "hl_mode,third_person,bike_pass,bike_object,objects,bike_carry_m,hl_ahead_m,hl_up_m,hl_across_m,"
                     "dab_key,dab_sitting,speed_ms,sh_pieces_drawn,sh_real_body,sh_real_helmet,sh_shapes,sh_late_3d,sh_riders_dropped,"
                     "sh_near_seen/cast_now/big/range/seethrough/held_back,"
                     "sh_all_seen/cast_now/big/range/seethrough/held_back/not3d/depth_back_on,"
                     "sh_seen_m_0-50/50-100/100-200/200+,sh_flush_samples,"
                     "sh_replay_skinned_drawn/indices_changed/repointed/no_bones,sh_replay_rigid_drawn/indices_changed/repointed/no_bones,"
                     "sh_real_body_along,marks_repressed,marks_repress_waiting,marks_others_cached,marks_ms,marks_window_still,"
                     "gpu_shadow_flush_ms,gpu_effects_ms,cpu_shadow_flush_ms,cpu_effects_ms,cpu_cast_now_ms,cpu_wheel_watch_ms,"
                     "whip,game_step_ms,bike_rest,camera_rest\n");
    // Times are relative to the key press: the history before it is negative.
    const double now = camera::ContentTime();
    g_traceFrames = 0;
    for (int i = g_traceCount; i > 0; --i) {
        const TraceLine& line = g_traceRing[(g_traceHead - i + kTraceRing) % kTraceRing];
        if (now - line.time > kTraceHistory)
            continue;
        fprintf(g_trace, "%.1f,%s", (line.time - now) * 1000.0, line.text);
        ++g_traceFrames;
    }
    g_traceStart = now;
    g_traceEnd   = seconds;
    Log("trace: saved the last %.0f s, recording %.0f s more to %ls", kTraceHistory, seconds, path);
}

namespace {

// One line per frame into the ring buffer, and into the file while a trace is recording.
void TraceFrame(bool captured)
{
    if (!g_traceRing) {
        g_traceRing = new (std::nothrow) TraceLine[kTraceRing];
        if (!g_traceRing)
            return;
    }
    const double now = camera::ContentTime();
    TraceLine& line = g_traceRing[g_traceHead];
    line.time = now;
    g_traceHead = (g_traceHead + 1) % kTraceRing;
    if (g_traceCount < kTraceRing)
        ++g_traceCount;

    const camera::Telemetry& t = camera::LastTelemetry();
    const bike::Pose& pose = bike::Current();
    if (!captured) {  // (the camera mod off or no camera: the wheels still, for the tire marks and dirt)
        snprintf(line.text, sizeof line.text, "no camera,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,%s\n", postfx::WheelTraceText());
    } else if (replay::Current() != replay::Mode::Off) {
        replay::Describe(line.text, sizeof line.text - 1);
        // (Then the wheels' and the marks' columns, as on a riding row's end: after "|wheels,".)
        const size_t used = strlen(line.text);
        snprintf(line.text + used, sizeof line.text - used, "|wheels,%s\n", postfx::WheelTraceText());
    } else {
        snprintf(line.text, sizeof line.text,
            "%.2f,%.2f,%d,%d,%d,%d,%d,%d,%.2f,"
            "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
            "%.3f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%.1f,%d,%d,%d,%d,%s,%s,%.3f,%.2f,%.2f,%.2f\n",
            t.rawIntervalMs, t.dtMs, g_camera.previews, g_camera.otherRotations,
            t.cut ? 1 : 0, t.reset ? 1 : 0, t.bikeTracked ? 1 : 0, t.bikeFresh ? 1 : 0, t.customWeight,
            t.rawYaw, t.rawPitch, t.rawRoll, t.bikeYaw, t.bikePitch, t.bikeRoll, t.outYaw, t.outPitch, t.outRoll,
            t.lagDeg, t.lookYawDeg, t.airYawDeg, t.apexDeg, t.airborne, t.fallG, t.verticalAccelG, t.neckOffset * 1000.0,
            t.rawPos[0], t.rawPos[1], t.rawPos[2], t.eyeOffset[0], t.eyeOffset[1], t.eyeOffset[2],
            pose.worldPosition[0], pose.worldPosition[1], pose.worldPosition[2],
            g_ground.awayDeg, g_ground.passes, g_ground.game, g_ground.filled, g_ground.open,
            g_ground.note[0] ? g_ground.note : "-", postfx::WheelTraceText(), t.whip, camera::LastStep() * 1000.0,
            t.bikeRestDeg, t.cameraRestDeg);
    }

    if (!g_trace)
        return;
    if (now - g_traceStart > g_traceEnd) {
        fclose(g_trace);
        g_trace = nullptr;
        Log("trace: %d frames written to MXS_REMASTERED_trace.csv", g_traceFrames);
        return;
    }
    fprintf(g_trace, "%.1f,%s", (now - g_traceStart) * 1000.0, line.text);
    ++g_traceFrames;
}

}  // namespace

namespace {

// Shadow maps (postfx.h): an object's draw call, repeated from the sun right before the game's
// own, with only u_modelview and the projection swapped (and put back after). Everything else
// the draw needs - its vertex arrays, textures, bones - is already set up by the game.
bool Streamed(GLuint buffer)
{
    const auto found = g_bufferUpdates.find(buffer);
    return found != g_bufferUpdates.end() && found->second.frames >= 2;
}

// Whether this draw can be repeated later in the frame (or a later frame) exactly as it is now:
// rigid, its geometry in buffers nothing rewrites while running.
bool StaticBuffers(const DrawCall& draw);

bool StaticDraw(const DrawCall& draw)
{
    // A skinned mesh's bones change from draw to draw; the same programs' rigid draws (no bones)
    // are fine: they're repeated with no bones again.
    const SkinnedProgram* skin = FindSkinned(g_program);
    if (skin && skin->positionLoc >= 0 && skin->bones != 0)
        return false;
    return StaticBuffers(draw);
}

// Its geometry in buffers nothing rewrites while running, so the same draw call draws it later.
bool StaticBuffers(const DrawCall& draw)
{
    if (g_vao == 0 || g_activeUnit < 0 || g_activeUnit >= 16)
        return false;
    const auto buffers = g_vaoArrayBuffers.find(g_vao);
    if (buffers == g_vaoArrayBuffers.end() || buffers->second.empty())
        return false;
    for (GLuint buffer : buffers->second)
        if (buffer == 0 || Streamed(buffer))
            return false;
    if (draw.kind != DrawCall::Arrays) {
        const auto element = g_elementBuffers.find(g_vao);
        if (element == g_elementBuffers.end() || element->second == 0 || Streamed(element->second))
            return false;  // indices from memory, or from a streamed buffer
    }
    return true;
}

// Only a draw whose vertex array stays as it is (scenery: set up once) is held back to be repeated
// later. One the game re-points from draw to draw (or no vertex array of its own) would be repeated
// with whatever it points at by then: every bike's parts, held back, cast nothing (ShadowBatch=0, cast
// straight away, they did). Those are cast straight away.
bool VertexArraySteady()
{
    if (!g_vao)
        return false;
    const auto changed = g_vaoChangedFrame.find(g_vao);
    return changed == g_vaoChangedFrame.end() || changed->second + 2 <= g_frameNumber;
}

// Nor what's with a rider (within kWithRiderMetres of one: their bike, their helmet). Held back,
// every bike's parts cast nothing (in a lobby, your bike's shadow gone as soon as you rode off); with
// ShadowBatch=0 - each cast straight away, as the game drew it - they did. (Resetting the program's
// bones for the repeat, and holding back only steady vertex arrays, didn't bring them back.) The
// riders' program draws the scenery too, so it can't be told by its program: by where it is. A few
// dozen draws per rider, next to the hundreds of scenery that stay together.
constexpr double kWithRiderMetres = 4.0;
bool CanHoldBack(const DrawCall& draw, const GLfloat uploaded[16])
{
    if (!GetConfig().shadowBatch || !StaticDraw(draw) || !VertexArraySteady())
        return false;
    // (Every rider's, as this was, cast a few hundred draws one by one at a full start gate: each
    // switches to the shadow map and back, ~17 us, and the frame rate fell by a quarter. Now that a
    // held-back draw gets the constant attributes it was drawn with, only your own rider and bike are
    // still cast as drawn - your own shadow stays right whatever else a repeat might miss.)
    const float at[3] = { uploaded[12], uploaded[13], uploaded[14] };
    float you[3];
    if (!postfx::OwnRiderEye(you))
        return true;
    const float dx = at[0] - you[0], dy = at[1] - you[1], dz = at[2] - you[2];
    const float reach = static_cast<float>(kWithRiderMetres * 3.2808);
    return dx * dx + dy * dy + dz * dz >= reach * reach;
}

// The draw as it stands, to repeat later (light matrices filled in by the caller).
DeferredCaster MakeCaster(const DrawCall& draw, GLint modelviewLoc, GLint projectionLoc)
{
    DeferredCaster c;
    c.program = g_program;
    c.vao = g_vao;
    const auto element = g_elementBuffers.find(g_vao);
    c.elementBuffer = element != g_elementBuffers.end() ? element->second : 0;
    c.draw = draw;
    c.vaoGen = g_vaoGeneration[g_vao];
    c.attribSet = g_constAttribSet;
    for (int k = 0; k < kConstAttribs; ++k) {
        if (!(g_constAttribSet & (1u << k)))
            continue;
        std::copy(g_constAttrib[k], g_constAttrib[k] + 4, c.attrib[k]);
    }
    for (int u = 0; u < kTrackedUnits; ++u)
        c.textures[u] = g_boundTextures[u];
    c.modelviewLoc = modelviewLoc;
    c.projectionLoc = projectionLoc;
    if (const SkinnedProgram* skin = FindSkinned(g_program))
        c.boneCountLoc = skin->positionLoc >= 0 ? skin->countLoc : -1;
    c.uniforms = SnapshotUniforms();
    return c;
}

// ---- Too big to cast: the stadium ------------------------------------------------------------
// In a stadium the stands and the roof are objects too. Drawn into the shadow map whenever the game
// drew them (in view), with a low sun their shadow covered the whole floor: one step and the track
// went dark and the rider's shadow was gone, the next it was sunny again, and drawing them four
// times over cost a lot of frames. Each draw call's size is measured once from its own vertices
// (read back from its buffers), and what's far bigger than anything on a track (over 80 m across),
// or all of it well overhead (a roof, the lights), doesn't cast.
struct DrawBounds {
    bool     valid = false;          // measured (false: couldn't be: it casts)
    float    lo[3] = {}, hi[3] = {};  // in its own model's space
    GLuint   buffers[2] = {};        // vertices, indices
    unsigned generation[2] = {};     // their contents then
    bool     logged = false;
};
std::unordered_map<unsigned long long, DrawBounds> g_drawBounds;
struct BufferCopy { unsigned generation = 0; std::vector<unsigned char> data; };
std::unordered_map<GLuint, BufferCopy> g_bufferCopies;  // read back (only while the contents are still those)
size_t g_bufferCopyBytes = 0;

using PFN_GetBufferParameteriv = void(APIENTRY*)(GLenum target, GLenum pname, GLint* params);
using PFN_GetBufferSubData     = void(APIENTRY*)(GLenum target, ptrdiff_t offset, ptrdiff_t size, void* data);
using PFN_GetAttribLocation    = GLint(APIENTRY*)(GLuint program, const char* name);
using PFN_GetIntegerv          = void(APIENTRY*)(GLenum pname, GLint* data);

constexpr GLenum kCopyReadBuffer = 0x8F36;  // (also its binding's name)
constexpr GLenum kBufferSize     = 0x8764;

// A buffer's contents, read back once (null: couldn't).
const std::vector<unsigned char>* BufferContents(GLuint buffer)
{
    const unsigned generation = g_bufferGeneration[buffer];
    const auto found = g_bufferCopies.find(buffer);
    if (found != g_bufferCopies.end()) {
        if (found->second.generation == generation)
            return &found->second.data;
        g_bufferCopyBytes -= std::min(g_bufferCopyBytes, found->second.data.size());
        g_bufferCopies.erase(found);  // (rewritten, or deleted and given out again: another track's)
    }
    static PFN_GetBufferParameteriv parameter = nullptr;
    static PFN_GetBufferSubData read = nullptr;
    static PFN_GetIntegerv getInteger = nullptr;
    if (!read) {
        parameter = reinterpret_cast<PFN_GetBufferParameteriv>(GetRealGLFunction("glGetBufferParameteriv"));
        read = reinterpret_cast<PFN_GetBufferSubData>(GetRealGLFunction("glGetBufferSubData"));
        getInteger = reinterpret_cast<PFN_GetIntegerv>(GetRealGLFunction("glGetIntegerv"));
    }
    if (!parameter || !read || !getInteger || !real_glBindBuffer || buffer == 0)
        return nullptr;
    GLint before = 0;
    getInteger(kCopyReadBuffer, &before);  // (a binding of its own: the arrays' and the vertex array's stay)
    real_glBindBuffer(kCopyReadBuffer, buffer);
    GLint size = 0;
    parameter(kCopyReadBuffer, kBufferSize, &size);
    std::vector<unsigned char> data;
    if (size > 0 && size <= 64 * 1024 * 1024) {
        data.resize(static_cast<size_t>(size));
        read(kCopyReadBuffer, 0, size, data.data());
    }
    real_glBindBuffer(kCopyReadBuffer, static_cast<GLuint>(before));
    if (data.empty())
        return nullptr;
    if (g_bufferCopyBytes + data.size() > 256u * 1024 * 1024) {  // (kept to a quarter of a gigabyte)
        g_bufferCopies.clear();
        g_bufferCopyBytes = 0;
    }
    g_bufferCopyBytes += data.size();
    BufferCopy& copy = g_bufferCopies[buffer];
    copy.generation = generation;
    copy.data = std::move(data);
    return &copy.data;
}

// The current draw's vertices, in its own model's space (false: not measurable).
bool MeasureDraw(const DrawCall& draw, DrawBounds& b)
{
    static PFN_GetAttribLocation attribLocation = nullptr;
    if (!attribLocation)
        attribLocation = reinterpret_cast<PFN_GetAttribLocation>(GetRealGLFunction("glGetAttribLocation"));
    static std::unordered_map<GLuint, GLint> positionOf;  // program -> its vattr_position
    auto position = positionOf.find(g_program);
    if (position == positionOf.end()) {
        const GLint location = attribLocation ? attribLocation(g_program, "vattr_position") : -1;
        position = positionOf.emplace(g_program, location >= 0 ? location : 0).first;
    }
    const auto pointer = g_vaoAttribs.find(static_cast<unsigned long long>(g_vao) << 4 | static_cast<unsigned>(position->second));
    if (pointer == g_vaoAttribs.end())
        return false;
    const AttribPointer& a = pointer->second;
    if (a.buffer == 0 || a.type != GL_FLOAT || a.size < 3)
        return false;
    const std::vector<unsigned char>* vertices = BufferContents(a.buffer);
    if (!vertices)
        return false;
    const size_t stride = a.stride > 0 ? static_cast<size_t>(a.stride) : static_cast<size_t>(a.size) * 4;
    bool any = false;
    const auto Take = [&](size_t index) {
        const size_t at = a.offset + index * stride;
        if (at + 12 > vertices->size())
            return;
        float v[3];
        memcpy(v, vertices->data() + at, 12);
        for (int k = 0; k < 3; ++k) {
            b.lo[k] = any ? std::fmin(b.lo[k], v[k]) : v[k];
            b.hi[k] = any ? std::fmax(b.hi[k], v[k]) : v[k];
        }
        any = true;
    };
    b.buffers[0] = a.buffer;
    b.generation[0] = g_bufferGeneration[a.buffer];
    if (draw.kind == DrawCall::Arrays) {
        for (int i = 0; i < draw.count; ++i)
            Take(static_cast<size_t>(draw.first) + i);
    } else {
        const auto element = g_elementBuffers.find(g_vao);
        if (element == g_elementBuffers.end() || element->second == 0)
            return false;
        const std::vector<unsigned char>* indices = BufferContents(element->second);
        if (!indices)
            return false;
        b.buffers[1] = element->second;
        b.generation[1] = g_bufferGeneration[element->second];
        const size_t width = draw.type == GL_UNSIGNED_BYTE ? 1 : draw.type == GL_UNSIGNED_SHORT ? 2 : draw.type == GL_UNSIGNED_INT ? 4 : 0;
        if (width == 0)
            return false;
        const size_t start = reinterpret_cast<uintptr_t>(draw.indices);
        for (int i = 0; i < draw.count; ++i) {
            const size_t at = start + static_cast<size_t>(i) * width;
            if (at + width > indices->size())
                break;
            unsigned index = 0;
            if (width == 1)
                index = (*indices)[at];
            else if (width == 2)
                index = static_cast<unsigned>((*indices)[at]) | static_cast<unsigned>((*indices)[at + 1]) << 8;
            else
                memcpy(&index, indices->data() + at, 4);
            Take(index);
        }
    }
    return any;
}

// Whether the draw about to be made is far bigger than anything on a track, or all of it well
// overhead (see DrawBounds): it isn't cast.
bool TooBigToCast(const DrawCall& draw, const GLfloat uploaded[16])
{
    const SkinnedProgram* skin = FindSkinned(g_program);
    if (skin && skin->bones != 0)
        return false;  // (a rider: its bones place it)
    const auto element = g_elementBuffers.find(g_vao);
    // Geometry the game rewrites while running (dust, streamed pieces) isn't measured: reading it
    // back every time it changes stalls the frame. It casts as before.
    if (element != g_elementBuffers.end() && element->second && Streamed(element->second))
        return false;
    const auto arrays = g_vaoArrayBuffers.find(g_vao);
    if (arrays != g_vaoArrayBuffers.end())
        for (GLuint buffer : arrays->second)
            if (Streamed(buffer))
                return false;
    unsigned long long key = 1469598103934665603ull;
    const auto Mix = [&key](unsigned long long v) { key ^= v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2); };
    Mix(g_program);
    Mix(g_vao);
    Mix(g_vaoGeneration[g_vao]);
    Mix(element != g_elementBuffers.end() ? element->second : 0);
    Mix(static_cast<unsigned long long>(draw.kind) << 32 | draw.mode);
    Mix(static_cast<unsigned long long>(static_cast<unsigned>(draw.count)) << 32 | static_cast<unsigned>(draw.first));
    Mix(reinterpret_cast<uintptr_t>(draw.indices));
    Mix(draw.type);
    DrawBounds& b = g_drawBounds[key];
    const bool stale = b.buffers[0] && (g_bufferGeneration[b.buffers[0]] != b.generation[0] ||
                                        (b.buffers[1] && g_bufferGeneration[b.buffers[1]] != b.generation[1]));
    if (stale) {
        g_bufferCopies.erase(b.buffers[0]);
        if (b.buffers[1])
            g_bufferCopies.erase(b.buffers[1]);
        b = DrawBounds{};
    }
    if (!b.buffers[0] && !b.valid) {
        DrawBounds measured;
        measured.valid = MeasureDraw(draw, measured);
        if (!measured.buffers[0])
            measured.buffers[0] = ~0u;  // (tried: not measurable, not tried again)
        b = measured;
    }
    if (!b.valid)
        return false;
    // Its box in eye space (u_modelview as drawn), then across and up in the world.
    double view[16], up[3];
    if (!postfx::FrameRenderedView(view, up))
        return false;
    const double upEye[3] = { view[0] * up[0] + view[4] * up[1] + view[8] * up[2], view[1] * up[0] + view[5] * up[1] + view[9] * up[2],
                              view[2] * up[0] + view[6] * up[1] + view[10] * up[2] };
    double lo[3] = {}, hi[3] = {}, lowest = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const float p[3] = { (corner & 1) ? b.hi[0] : b.lo[0], (corner & 2) ? b.hi[1] : b.lo[1], (corner & 4) ? b.hi[2] : b.lo[2] };
        double e[3];
        for (int r = 0; r < 3; ++r)
            e[r] = uploaded[r] * p[0] + uploaded[4 + r] * p[1] + uploaded[8 + r] * p[2] + uploaded[12 + r];
        const double height = e[0] * upEye[0] + e[1] * upEye[1] + e[2] * upEye[2];  // above the camera
        for (int r = 0; r < 3; ++r) {
            lo[r] = corner ? std::fmin(lo[r], e[r]) : e[r];
            hi[r] = corner ? std::fmax(hi[r], e[r]) : e[r];
        }
        lowest = corner ? std::fmin(lowest, height) : height;
    }
    const double u = 3.2808;  // (game units: feet)
    const double across = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    const bool tooBig = across > 80.0 * u;
    // All of it well above the camera: a stadium's upper stands, its lights and roof panels - but
    // only where the track is a stadium (something far too big has been seen). Elsewhere it's a
    // tree on a hill, or another rider up the hill from you, and it casts: left out, its shadow
    // only came in once you got up close to it.
    if (tooBig && !g_stadiumTrack) {
        g_stadiumTrack = true;
        Log("postfx: shadow map: this track has a stadium (an object %.0f m across): objects all overhead are left out too",
            across / u);
    }
    const bool overhead = g_stadiumTrack && lowest > 12.0 * u;
    if ((tooBig || overhead) && !b.logged && g_loggedBigCasters < 12) {
        b.logged = true;
        ++g_loggedBigCasters;
        Log("postfx: shadow map: left out an object %s (%.0f m across, its lowest point %.0f m above the camera; program %u, "
            "%d indices): the stadium's stands or roof would shade the whole track", tooBig ? "far bigger than anything on a track" : "all overhead",
            across / u, lowest / u, g_program, draw.count);
    }
    return tooBig || overhead;
}

// Your rider's pieces the game left out (your body, in first person: it draws only your arms and
// legs), drawn into the shadow map right along with a piece of your rider it does draw - with the
// game's own state for your rider at that moment (its program, its bones, everything it set), only
// the piece's own vertex array and its bones taken in its own order from the ones the game uploaded.
// Repeated later instead, from what was kept (CastRiderPieces), they were drawn but came out invisible
// in the game: your first-person shadow had no torso. Once a frame, inside the caller's shadow draw,
// into the cascade it's drawing now. Returns how many were drawn.
unsigned g_realBodyFrame = 0;
int DrawRealBodyAlong(const SkinnedProgram* skin, bool firstCascade)
{
    if (!skin || skin->bones <= 0 || skin->centres < skin->bones || skin->positions < skin->bones ||
        skin->rotations < skin->bones || !real_glBindVertexArray || !real_glUniform1i)
        return 0;
    if (firstCascade && g_realBodyFrame == g_frameNumber)
        return 0;  // (once a frame: the first piece of your rider drawn)
    const double same = 1e-3 * 3.2808;
    int drawn = 0;
    GLfloat centre[kMaxBones * 3], position[kMaxBones * 3], rotation[kMaxBones * 9];
    for (const KeptRiderPiece& piece : g_riderPieces) {
        if (!piece.used || !piece.leftOut || piece.count <= 0 || piece.count > kMaxBones)
            continue;
        const auto element = g_elementBuffers.find(piece.caster.vao);
        if (piece.caster.draw.kind != DrawCall::Arrays &&
            (element == g_elementBuffers.end() || element->second != piece.caster.elementBuffer))
            continue;  // (its vertex array was given other indices since)
        bool all = true;
        for (int b = 0; b < piece.count && all; ++b) {
            const GLfloat* c = piece.centre + b * 3;
            bool found = false;
            for (int k = 0; k < skin->bones && !found; ++k) {
                const GLfloat* o = skin->centre + k * 3;
                if (std::fabs(o[0] - c[0]) + std::fabs(o[1] - c[1]) + std::fabs(o[2] - c[2]) > same)
                    continue;
                found = true;
                std::copy(o, o + 3, centre + b * 3);
                std::copy(skin->position + k * 3, skin->position + k * 3 + 3, position + b * 3);
                std::copy(skin->rotation + k * 9, skin->rotation + k * 9 + 9, rotation + b * 9);
            }
            all = found;
        }
        if (!all)
            continue;
        UploadSkinnedVectors(skin->centreLoc, piece.count, centre, skin->positionArb);
        UploadSkinnedVectors(skin->positionLoc, piece.count, position, skin->positionArb);
        UploadSkinnedRotations(skin->rotationLoc, piece.count, skin->transposed, rotation, skin->rotationArb);
        real_glUniform1i(skin->countLoc, piece.count);
        real_glBindVertexArray(piece.caster.vao);
        Replay(piece.caster.draw);
        ++drawn;
    }
    if (drawn) {
        // The game's own again: its vertex array and its bones for this draw.
        real_glBindVertexArray(g_vao);
        UploadSkinnedVectors(skin->centreLoc, skin->centres, skin->centre, skin->positionArb);
        UploadSkinnedVectors(skin->positionLoc, skin->positions, skin->position, skin->positionArb);
        UploadSkinnedRotations(skin->rotationLoc, skin->rotations, skin->transposed, skin->rotation, skin->rotationArb);
        real_glUniform1i(skin->countLoc, skin->bones);
    }
    return drawn;
}

void CastShadow(const DrawCall& draw)
{
    const auto mv = g_programModelview.find(g_program);
    const bool scene3d = mv != g_programModelview.end() && mv->second.frame == g_frameNumber &&
                         mv->second.kind == ModelviewKind::Scene && !mv->second.transposed && mv->second.location >= 0 &&
                         HasTranslation(mv->second.uploaded);
    // (For the trace: what becomes of the game's draws right around your rider - your bike, your rider.)
    bool nearYou = false;
    if (scene3d) {
        const GLfloat* m = mv->second.uploaded;
        float at[3] = { m[12], m[13], m[14] }, you[3];
        const SkinnedProgram* skin = FindSkinned(g_program);
        if (skin && skin->bones > 0 && skin->positions > 0)
            for (int r = 0; r < 3; ++r)
                at[r] = m[r] * skin->position[0] + m[4 + r] * skin->position[1] + m[8 + r] * skin->position[2] + m[12 + r];
        if (postfx::OwnRiderEye(you)) {
            const float dx = at[0] - you[0], dy = at[1] - you[1], dz = at[2] - you[2];
            nearYou = dx * dx + dy * dy + dz * dz < (2.5f * 3.2808f) * (2.5f * 3.2808f);
        }
    }
    if (nearYou)
        postfx::NoteNearCast(0);
    // See-through draws (kicked-up dirt, dust, smoke) don't write depth: they would cast solid
    // squares. (Blending alone says nothing: the game leaves it on for solid objects too.)
    if (!g_depthWrites) {
        // (Asked of GL itself: depth writes turned back on some other way than glDepthMask - a
        // glPopAttrib - left every draw after taken for see-through, casting nothing. Only asked
        // when it says off, which is a few draws a frame.)
        GLint mask = 0;
        if (!real_glGetIntegerv)
            real_glGetIntegerv = reinterpret_cast<PFN_GetIntegerv>(GetRealGLFunction("glGetIntegerv"));
        if (real_glGetIntegerv)
            real_glGetIntegerv(GL_DEPTH_WRITEMASK, &mask);
        if (mask) {
            g_depthWrites = true;
            postfx::NoteCast(7);
            static bool logged = false;
            if (!logged) {
                logged = true;
                Log("postfx: shadow map: depth writes were back on without a glDepthMask call: the draws after it are taken as solid again");
            }
        } else {
            postfx::NoteCast(4);
            if (nearYou)
                postfx::NoteNearCast(4);
            return;
        }
    }
    if (!scene3d) {
        postfx::NoteCast(6);
        return;  // not a 3D object this frame (or the sky, which has no position)
    }
    postfx::NoteCast(0);
    {
        const GLfloat* m = mv->second.uploaded;
        postfx::NoteCastDistance(std::sqrt(m[12] * m[12] + m[13] * m[13] + m[14] * m[14]) / 3.2808);
    }
    const auto projection = g_programProjection.find(g_program);
    if (projection == g_programProjection.end() || projection->second.location < 0)
        return;
    const PFN_UniformMatrix4fv upload = real_glUniformMatrix4fv ? real_glUniformMatrix4fv : real_glUniformMatrix4fvARB;
    if (!upload)
        return;

    // Rigid parts near the camera (your bike's wheels among them, see KeepOrCastWheels).
    const GLfloat* raw = mv->second.raw;
    if (g_nearDraws.size() < kMaxNearDraws && raw[12] * raw[12] + raw[13] * raw[13] + raw[14] * raw[14] <
        kNearDrawRange * kNearDrawRange && StaticDraw(draw)) {
        NearDraw candidate;
        candidate.caster = MakeCaster(draw, mv->second.location, projection->second.location);
        std::copy(raw, raw + 16, candidate.raw);
        std::copy(mv->second.uploaded, mv->second.uploaded + 16, candidate.uploaded);
        std::copy(projection->second.value, projection->second.value + 16, candidate.projection);
        g_nearDraws.push_back(candidate);
    }
    // Skinned draws near the camera (your rider's pieces among them, see CastRiderPieces), with
    // the game's own bones.
    if (const SkinnedProgram* skin = FindSkinned(g_program)) {
        const int n = skin->bones;
        if (n >= 2 && n <= kMaxBones && skin->centres >= n && skin->positions >= n && skin->rotations >= n &&
            skin->centreLoc >= 0 && skin->positionLoc >= 0 && skin->rotationLoc >= 0 && skin->countLoc >= 0 &&
            StaticBuffers(draw) && (g_riderDraws.size() < kMaxRiderDraws || (postfx::NoteRiderDrawDropped(), false))) {
            const GLfloat* m = raw;
            const GLfloat* b = skin->position;
            const float x = m[0] * b[0] + m[4] * b[1] + m[8] * b[2] + m[12], y = m[1] * b[0] + m[5] * b[1] + m[9] * b[2] + m[13],
                        z = m[2] * b[0] + m[6] * b[1] + m[10] * b[2] + m[14];
            if (x * x + y * y + z * z < kNearDrawRange * kNearDrawRange) {
                g_riderDraws.emplace_back();
                RiderDraw& r = g_riderDraws.back();
                r.caster = MakeCaster(draw, mv->second.location, projection->second.location);
                r.bones.count = n;
                r.bones.transposed = skin->transposed;
                std::copy(skin->centre, skin->centre + n * 3, r.bones.centre);
                std::copy(skin->position, skin->position + n * 3, r.bones.position);
                std::copy(skin->rotation, skin->rotation + n * 9, r.bones.rotation);
                std::copy(raw, raw + 16, r.raw);
            }
        }
    }

    if (!postfx::WantsShadowCasters())
        return;  // (collected for the tire dirt's mask only)
    if (TooBigToCast(draw, mv->second.uploaded)) {
        postfx::NoteCast(2);
        if (nearYou)
            postfx::NoteNearCast(2);
        return;
    }
    // Riders are skinned: their bones are placed in the world, and the modelview is the bare view.
    float anchor[3];
    const float* anchorEye = nullptr;
    if (const SkinnedProgram* skin = FindSkinned(g_program)) {
        if (skin->bones > 0 && skin->positions > 0) {
            const GLfloat* m = mv->second.uploaded;
            const GLfloat* b = skin->position;
            for (int r = 0; r < 3; ++r)
                anchor[r] = m[r] * b[0] + m[4 + r] * b[1] + m[8 + r] * b[2] + m[12 + r];
            anchorEye = anchor;
        }
    }
    float lightModelview[16];
    unsigned cascades = 0;
    if (!postfx::ShadowMatrices(mv->second.uploaded, anchorEye, lightModelview, &cascades)) {
        postfx::NoteCast(3);
        if (nearYou)
            postfx::NoteNearCast(3);
        return;
    }
    if (nearYou)
        postfx::NoteNearCast(CanHoldBack(draw, mv->second.uploaded) ? 5 : 1);  // (5: held back to be repeated later)

    // Static scenery (not skinned, its geometry in buffers nothing rewrites while running): held
    // back and drawn with the others in one go before the shadows are applied (FlushShadowCasters).
    if (CanHoldBack(draw, mv->second.uploaded)) {
        DeferredCaster c = MakeCaster(draw, mv->second.location, projection->second.location);
        std::copy(lightModelview, lightModelview + 16, c.lightModelview);
        c.cascades = cascades;
        if (g_deferredCasters.size() < 8192) {
            g_deferredCasters.push_back(c);
            postfx::NoteCast(5);
            return;
        }
    }
    postfx::NoteCast(1);
    if (!real_glDrawElements)
        real_glDrawElements = reinterpret_cast<PFN_DrawElements>(GetRealGLFunction("glDrawElements"));
    if (!real_glDrawArrays)
        real_glDrawArrays = reinterpret_cast<PFN_DrawArrays>(GetRealGLFunction("glDrawArrays"));
    if (!postfx::BeginShadowDraw())
        return;
    ++g_castAtOnce;
    // Your torso and head hidden on screen (Config::hideOwnBody) still cast their shadow: the
    // game's own bones for the shadow, ours again after.
    SkinnedProgram* hiddenBody = FindSkinned(g_program);
    if (hiddenBody && !(hiddenBody->positionEdited || hiddenBody->rotationEdited))
        hiddenBody = nullptr;
    if (hiddenBody) {
        UploadSkinnedVectors(hiddenBody->positionLoc, hiddenBody->positions, hiddenBody->position, hiddenBody->positionArb);
        UploadSkinnedRotations(hiddenBody->rotationLoc, hiddenBody->rotations, hiddenBody->transposed,
                               hiddenBody->rotation, hiddenBody->rotationArb);
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("postfx: shadow map: your hidden torso still casts its shadow (program %u)", g_program);
        }
    }
    // A piece of your rider (one of its bones at your rider's hips): the pieces of it the game left
    // out are drawn right along with it (DrawRealBodyAlong).
    const SkinnedProgram* yours = nullptr;
    if (const SkinnedProgram* skin = FindSkinned(g_program)) {
        float hips[3];
        if (skin->bones > 0 && skin->positions >= skin->bones && g_realBodyFrame != g_frameNumber && postfx::CameraAtYourHead() &&
            postfx::OwnHipsEye(hips)) {
            const GLfloat* m = mv->second.uploaded;
            const float tight = (0.05f * 3.2808f) * (0.05f * 3.2808f);
            for (int b = 0; b < skin->bones && !yours; ++b) {
                const GLfloat* p = skin->position + b * 3;
                float d2 = 0;
                for (int r = 0; r < 3; ++r) {
                    const float e = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r] - hips[r];
                    d2 += e * e;
                }
                if (d2 < tight)
                    yours = skin;
            }
        }
    }
    int realBody = 0;
    upload(mv->second.location, 1, GL_FALSE, lightModelview);
    for (int i = 0; i < postfx::ShadowCascades(); ++i) {  // into each cascade it reaches
        if (!(cascades & (1u << i)))
            continue;
        float lightProjection[16];
        postfx::UseShadowCascade(i, lightProjection);
        upload(projection->second.location, 1, GL_FALSE, lightProjection);
        Replay(draw);
        if (yours) {
            const int n = DrawRealBodyAlong(yours, realBody == 0);
            realBody = std::max(realBody, n);
        }
    }
    if (yours && realBody > 0) {
        g_realBodyFrame = g_frameNumber;
        postfx::NoteRealBodyDrawn(realBody);
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("postfx: shadow map: %d piece(s) of your rider the game leaves out (your body) cast along with one it draws, "
                "with your rider's own bones", realBody);
        }
    }
    upload(mv->second.location, 1, GL_FALSE, mv->second.uploaded);
    upload(projection->second.location, 1, GL_FALSE, projection->second.value);
    if (hiddenBody) {
        UploadSkinnedVectors(hiddenBody->positionLoc, hiddenBody->positions, hiddenBody->hiddenPosition,
                             hiddenBody->positionArb);
        UploadSkinnedRotations(hiddenBody->rotationLoc, hiddenBody->rotations, hiddenBody->transposed,
                               hiddenBody->hiddenRotation, hiddenBody->rotationArb);
    }
    postfx::EndShadowDraw();
}

// ---- Parts of your bike and rider the game leaves out ---------------------------------------
// In first person the game doesn't draw your helmet, your rear wheel or the swingarm (a capture:
// 7 rigid parts around your rider in third person, 4 in first), so they'd cast no shadow. While the
// game draws a part that rides on your torso bone (the helmet: it keeps still there while the torso
// moves about on the bike), its draw call is kept with where it sits there; when the game leaves it
// out, it's cast from there with the held-back casters. (Parts that keep still on the bike aren't:
// another rider alongside can too. postfx draws shapes of its own for the back of your bike.)

bool SameDrawCall(const DeferredCaster& a, const DeferredCaster& b)
{
    return a.program == b.program && a.vao == b.vao && a.elementBuffer == b.elementBuffer && a.draw.kind == b.draw.kind &&
           a.draw.mode == b.draw.mode && a.draw.count == b.draw.count && a.draw.type == b.draw.type &&
           a.draw.indices == b.draw.indices && a.draw.first == b.draw.first && a.draw.start == b.draw.start &&
           a.draw.end == b.draw.end;
}

// Column-major 4 x 4 (affine) helpers.
void Mul4(const double a[16], const double b[16], double out[16])
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = sum;
        }
}

bool InverseAffine(const double m[16], double out[16])
{
    const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12)
        return false;
    const double s = 1.0 / det;
    const double inv[9] = { A * s, -(b * i - c * h) * s, (b * f - c * e) * s,
                            B * s, (a * i - c * g) * s, -(a * f - c * d) * s,
                            C * s, -(a * h - b * g) * s, (a * e - b * d) * s };  // row-major
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k)
            out[k * 4 + r] = inv[r * 3 + k];
        out[12 + r] = -(inv[r * 3] * m[12] + inv[r * 3 + 1] * m[13] + inv[r * 3 + 2] * m[14]);
        out[r * 4 + 3] = 0;
    }
    out[15] = 1;
    return true;
}

// How far apart two placements are: their origins (units) and their axes (about radians).
void Apart(const double a[16], const double b[16], double* move, double* turn)
{
    const double dx = a[12] - b[12], dy = a[13] - b[13], dz = a[14] - b[14];
    *move = std::sqrt(dx * dx + dy * dy + dz * dz);
    *turn = 0;
    for (int c = 0; c < 3; ++c) {
        const double x = a[c * 4] - b[c * 4], y = a[c * 4 + 1] - b[c * 4 + 1], z = a[c * 4 + 2] - b[c * 4 + 2];
        *turn = std::max(*turn, std::sqrt(x * x + y * y + z * z));
    }
}

void UpdateRiddenParts(const postfx::OwnAnchors& own)
{
    const double now = camera::ContentTime();
    const double dt = g_riddenTime >= 0 ? std::min(std::max(now - g_riddenTime, 0.0), 0.1) : 0.0;
    g_riddenTime = now;
    ++g_riddenFrame;
    double invRaw[16], invAnchor[2][16];
    if (!InverseAffine(own.raw, invRaw))
        return;
    const bool have[2] = { own.bike && InverseAffine(own.bikeWorld, invAnchor[0]),
                           own.torso && InverseAffine(own.torsoWorld, invAnchor[1]) };
    const double u = own.unitsPerMetre;
    // How far a part may stray and still ride on it: the torso carries the helmet exactly; the
    // bike's swingarm moves a little with the suspension.
    const double kAllowed[2] = { 0.10 * u, 0.03 * u };

    for (const NearDraw& drawn : g_nearDraws) {
        const SkinnedProgram* skin = FindSkinned(drawn.caster.program);
        if (!skin || skin->positionLoc < 0)
            continue;  // only the game's model shaders (riders and bikes), not its sprites
        double vm[16], world[16];
        for (int i = 0; i < 16; ++i)
            vm[i] = drawn.raw[i];
        Mul4(invRaw, vm, world);
        const double dx = world[12] - own.riderPosition[0], dy = world[13] - own.riderPosition[1],
                     dz = world[14] - own.riderPosition[2];
        if (dx * dx + dy * dy + dz * dz > (2.5 * u) * (2.5 * u))
            continue;  // not part of your bike or rider
        double rel[2][16];
        for (int a = 0; a < 2; ++a)
            if (have[a])
                Mul4(invAnchor[a], world, rel[a]);

        // Which part: the same draw call, in the same place on you (other bikes can share the model).
        RiddenPart* part = nullptr;
        double best = 0.5 * u;
        for (RiddenPart& p : g_riddenParts) {
            if (!p.used || p.drawnFrame == g_riddenFrame || !SameDrawCall(p.caster, drawn.caster))
                continue;
            const int a = have[1] && p.haveRel[1] ? 1 : have[0] && p.haveRel[0] ? 0 : -1;
            double move = 0, turn = 0;
            if (a >= 0)
                Apart(rel[a], p.rel[a], &move, &turn);
            if (move < best) {
                best = move;
                part = &p;
            }
        }
        if (!part) {
            for (RiddenPart& p : g_riddenParts)
                if (!p.used) {
                    part = &p;
                    break;
                }
            if (!part) {
                if (g_riddenParts.size() >= kMaxRiddenParts)
                    continue;
                g_riddenParts.emplace_back();
                part = &g_riddenParts.back();
            }
            *part = RiddenPart{};
            part->used = true;
        }
        RiddenPart& p = *part;
        p.caster = drawn.caster;
        const auto buffers = g_vaoArrayBuffers.find(drawn.caster.vao);
        p.buffers = buffers != g_vaoArrayBuffers.end() ? buffers->second : std::vector<GLuint>{};
        if (drawn.caster.elementBuffer)
            p.buffers.push_back(drawn.caster.elementBuffer);
        p.drawnFrame = g_riddenFrame;
        if (p.seen > 0) {
            const double mx = world[12] - p.lastWorld[0], my = world[13] - p.lastWorld[1], mz = world[14] - p.lastWorld[2];
            p.travel += std::sqrt(mx * mx + my * my + mz * mz);
        }
        p.lastWorld[0] = world[12];
        p.lastWorld[1] = world[13];
        p.lastWorld[2] = world[14];
        p.seen += std::max(dt, 1e-6);
        const double k = 1.0 - std::exp(-dt / 0.5);
        double score[2] = { 1e9, 1e9 };  // its wobble there against what's allowed there (under 1: rides on it)
        for (int a = 0; a < 2; ++a) {
            if (!have[a])
                continue;
            if (!p.haveRel[a]) {
                std::copy(rel[a], rel[a] + 16, p.mean[a]);
                p.wobble[a] = 0;
            } else {
                double move = 0, turn = 0;
                Apart(rel[a], p.mean[a], &move, &turn);
                const double off = move + 0.5 * u * turn;
                p.wobble[a] += (off * off - p.wobble[a]) * k;
                for (int i = 0; i < 16; ++i)
                    p.mean[a][i] += (rel[a][i] - p.mean[a][i]) * k;
            }
            std::copy(rel[a], rel[a] + 16, p.rel[a]);
            p.haveRel[a] = true;
            score[a] = std::sqrt(p.wobble[a]) / kAllowed[a];
        }
        // On whichever it keeps still on, once it's been seen long enough to tell: clearly stiller
        // there than on the other (the torso moves about on the bike as you ride), or still on both
        // for a long while, or the other isn't found. And only once it has gone 2 m with you: an
        // object standing beside you (the start gate, a block) keeps still on you too while you wait.
        // Only parts that ride on your torso (your helmet) are learnt. Parts that keep still on your
        // bike can be someone else's: a rider alongside at your speed off the start gate kept still
        // on it and travelled with you, and cast as a block beside your shadow.
        // And only while the camera is really in third person (metres away: the only time the game
        // draws your helmet); a camera close behind you (a replay, another view) once taught small
        // parts that then floated behind your shadow's head.
        if (own.thirdPerson && p.seen >= 0.75 && p.travel >= 2.0 * u && score[1] < 1.0 && have[0] &&
            score[0] > 2.0 * score[1] + 0.3)
            p.anchor = 1;
        p.learnt = p.anchor >= 0 && p.haveRel[p.anchor];
    }

    // The learnt parts the game didn't draw this frame: cast where they sit. Not the ones on your
    // torso (your helmet), for now: in a lobby the parts learnt there were other bikes' wheels (rims,
    // discs: kept still beside you at the gate) and stuck to your shadow's shoulders, the helmet shape
    // standing aside for them; and a rigid part repeated later is what lost every bike's shadow
    // (CanHoldBack). Your head's shape stands in for your helmet in first person instead.
    for (RiddenPart& p : g_riddenParts) {
        if (!p.used || !p.learnt || p.drawnFrame == g_riddenFrame || !have[p.anchor] || p.anchor == 1)
            continue;
        if (g_deferredCasters.size() >= 8192)
            break;
        double world[16], eye[16];
        Mul4(p.anchor == 1 ? own.torsoWorld : own.bikeWorld, p.rel[p.anchor], world);
        Mul4(own.rendered, world, eye);
        float viewModel[16];
        for (int i = 0; i < 16; ++i)
            viewModel[i] = static_cast<float>(eye[i]);
        DeferredCaster c = p.caster;
        if (!postfx::ShadowMatrices(viewModel, nullptr, c.lightModelview, &c.cascades))
            continue;
        g_deferredCasters.push_back(c);
        if (p.anchor == 1)
            postfx::NoteRiderPartCast(false);
        if (!p.loggedCast && g_riddenLogs < 20) {
            p.loggedCast = true;
            ++g_riddenLogs;
            Log("postfx: shadow map: a part of your %s the game doesn't draw here casts its shadow anyway (program %u, %d indices)",
                p.anchor == 1 ? "rider (on the torso: the helmet)" : "bike", p.caster.program, p.caster.draw.count);
        }
    }
}

// The tire dirt's mask (postfx::WheelMaskDraw): each of your wheels the game drew this frame, every
// draw call of it (all programs: its tire, its shiny parts), drawn again as it was into the mask.
// Once a wheel's parts are known they're found every frame by themselves, not only while the wheel
// tracker holds your wheel (the dirt went whenever it let go): each the same draw call where its
// axle was last frame, moved as far as your rider moved since (within the suspension's travel), so
// another bike's identical wheel isn't it.
struct MaskPart {
    DeferredCaster id;
    double         axleLocal[3] = {};  // its axle in its own model (its origin swings round as it turns)
    double         world[3] = {};      // its axle last frame (world), and your rider then
    double         rider[3] = {};
};
struct MaskWheel {
    int      count = 0;
    MaskPart parts[postfx::kWheelMaskParts];
};
MaskWheel g_maskWheels[2];

// The rest of a wheel: the brake disc, the sprocket, whatever is bolted to it, drawn as models of
// their own. A draw that keeps exactly its place on the wheel frame after frame while the wheel
// turns is part of it (the frame and fork don't: the wheel turns against them; another bike's wheel
// doesn't stay locked to yours). (Each part's own axle estimate is no good for this: small parts
// spinning fast are followed poorly, and the mask was once the disc alone.)
struct WheelMember {
    DeferredCaster id;
    double         rel[16] = {};   // its transform on the wheel's
    int            steady = 0;     // frames in a row it kept it while the wheel turned
    unsigned       seenFrame = 0;
};
std::vector<WheelMember> g_wheelMembers[2];
double   g_lastWheelWorld[2][16] = {};
bool     g_haveLastWheel[2] = { false, false };
double   g_lastWheelInWorld[2][16] = {};          // the wheel last frame in the world itself (not on the bike)
bool     g_haveLastWheelInWorld[2] = { false, false };
unsigned g_memberFrame = 0;

void FindWheelMembers(int i, const NearDraw& main, const double invRaw[16], const postfx::OwnAnchors& own,
                      std::vector<const NearDraw*>& draws)
{
    const double unitsPerMetre = own.unitsPerMetre;
    double vm[16], wheel[16], invWheel[16];
    for (int k = 0; k < 16; ++k)
        vm[k] = main.raw[k];
    Mul4(invRaw, vm, wheel);
    if (!InverseAffine(wheel, invWheel))
        return;
    // Spinning: turned about its axle on your bike since last frame. (Turned in the world isn't
    // enough: the bike rocking, pitching or turning turns the wheel and everything on the bike
    // together, and standing still or rolling slowly the frame, the body and your helmet then kept
    // their place on the wheel and got its mud. Steering turns the front wheel on the bike too, but
    // about another axis.) A part still on a wheel that spins is on the wheel.
    bool turning = false;
    double invBike[16], onBike[16];
    if (own.bike && InverseAffine(own.bikeWorld, invBike)) {
        Mul4(invBike, wheel, onBike);
        double invLast[16], d[16];
        if (g_haveLastWheel[i] && InverseAffine(g_lastWheelWorld[i], invLast)) {
            Mul4(onBike, invLast, d);  // the turn since last frame, in the bike's space
            const double ax = d[6] - d[9], ay = d[8] - d[2], az = d[1] - d[4];  // 2 sin(angle) along its axis
            const double size = std::sqrt(ax * ax + ay * ay + az * az);
            const double along = std::fabs(ax * own.bikeAcross[0] + ay * own.bikeAcross[1] + az * own.bikeAcross[2]);
            turning = along > 0.1 && along > 0.9 * size;
        }
        std::copy(onBike, onBike + 16, g_lastWheelWorld[i]);  // (on the bike)
        g_haveLastWheel[i] = true;
    } else {
        g_haveLastWheel[i] = false;
    }
    // And spinning in the world as well. Beside other bikes (at the gate, the same model a metre to
    // the side) your bike was taken for one of them for a moment: the wheel then seemed to turn on
    // "your bike" while it stood still, and your bike's body, standing just as still against it, was
    // taken for a part of the wheel - and got its mud all over. A wheel that really spins turns in
    // the world too; one only the mix-up turned doesn't.
    bool spinning = false;
    if (g_haveLastWheelInWorld[i]) {
        double invLast[16], d[16];
        if (InverseAffine(g_lastWheelInWorld[i], invLast)) {
            Mul4(wheel, invLast, d);  // the turn since last frame, in the world
            const double ax = d[6] - d[9], ay = d[8] - d[2], az = d[1] - d[4];
            spinning = std::sqrt(ax * ax + ay * ay + az * az) > 0.1;
        }
    }
    std::copy(wheel, wheel + 16, g_lastWheelInWorld[i]);
    g_haveLastWheelInWorld[i] = true;
    turning = turning && spinning;
    std::vector<WheelMember>& members = g_wheelMembers[i];
    std::vector<bool> taken(g_nearDraws.size(), false);
    const double reach = 4.0 * unitsPerMetre;
    const auto RelOf = [&](const NearDraw& d, double rel[16]) {
        double dv[16], world[16];
        for (int k = 0; k < 16; ++k)
            dv[k] = d.raw[k];
        Mul4(invRaw, dv, world);
        Mul4(invWheel, world, rel);
    };
    // The known ones: each the draw of its kind nearest its place on the wheel.
    for (WheelMember& m : members) {
        int best = -1;
        double bestMove = 1e9, bestTurn = 1e9, bestRel[16];
        for (size_t k = 0; k < g_nearDraws.size(); ++k) {
            const NearDraw& d = g_nearDraws[k];
            if (taken[k] || !SameDrawCall(d.caster, m.id) || std::equal(d.raw, d.raw + 16, main.raw))
                continue;
            double rel[16], move = 0, turn = 0;
            RelOf(d, rel);
            Apart(rel, m.rel, &move, &turn);
            if (move < bestMove) {
                bestMove = move;
                bestTurn = turn;
                best = static_cast<int>(k);
                std::copy(rel, rel + 16, bestRel);
            }
        }
        if (best < 0)
            continue;
        taken[static_cast<size_t>(best)] = true;
        m.seenFrame = g_memberFrame;
        const bool kept = bestMove < 0.03 * unitsPerMetre && bestTurn < 0.03;
        if (!kept)
            m.steady = 0;
        else if (turning)
            ++m.steady;
        std::copy(bestRel, bestRel + 16, m.rel);
        if (m.steady >= 5)
            for (const NearDraw& d : g_nearDraws)  // every program's copy of it
                if (std::equal(d.raw, d.raw + 16, g_nearDraws[static_cast<size_t>(best)].raw))
                    draws.push_back(&d);
    }
    // Candidates: the other rigid draws close to the wheel, while it turns.
    if (turning) {
        for (size_t k = 0; k < g_nearDraws.size() && members.size() < 24; ++k) {
            const NearDraw& d = g_nearDraws[k];
            if (taken[k] || std::equal(d.raw, d.raw + 16, main.raw))
                continue;
            double rel[16];
            RelOf(d, rel);
            if (std::sqrt(rel[12] * rel[12] + rel[13] * rel[13] + rel[14] * rel[14]) > reach)
                continue;
            bool known = false;
            for (const WheelMember& m : members)
                known = known || SameDrawCall(m.id, d.caster);
            if (known)
                continue;
            WheelMember m;
            m.id = d.caster;
            std::copy(rel, rel + 16, m.rel);
            m.seenFrame = g_memberFrame;
            members.push_back(m);
            taken[k] = true;
        }
    }
    // Forget the ones gone a while.
    for (size_t k = 0; k < members.size();)
        if (g_memberFrame - members[k].seenFrame > 120)
            members.erase(members.begin() + static_cast<std::ptrdiff_t>(k));
        else
            ++k;
}

// Your wheels' draw calls as last found (DrawWheelMasks), for PaintTireDirt: each draw of one this
// frame is the same call with your wheel's axle where it should be by now.
struct PaintPart {
    DeferredCaster id;
    double         axleLocal[3] = {};
    double         world[3] = {}, velocity[3] = {};
    int            wheel = 0;         // 0 rear, 1 front
    bool           member = false;    // kept on the wheel (FindWheelMembers), not one of the tracker's own parts
};
std::vector<PaintPart> g_paintParts;
double g_paintTime = 0, g_paintUnits = 3.2808;
double g_paintAxle[2][3] = {}, g_paintAxleTime[2] = {};
bool   g_havePaintAxle[2] = { false, false };
std::unordered_map<GLuint, GLint> g_mudUniform;   // program -> u_mx_mud (-1: none)
std::unordered_map<GLuint, float> g_mudSet;       // program -> what it holds

// Right before each draw: u_mx_mud on for your wheels' draws, off for everything else.
void PaintTireDirt(const DrawCall& draw)
{
    auto at = g_mudUniform.find(g_program);
    if (at == g_mudUniform.end()) {
        const GLint location = real_glGetUniformLocation ? real_glGetUniformLocation(g_program, "u_mx_mud") : -1;
        at = g_mudUniform.emplace(g_program, location).first;
        g_mudSet[g_program] = 0.0f;
    }
    if (at->second < 0 || !real_glUniform1f)
        return;
    const Config& cfg = GetConfig();
    float want = 0.0f;
    double raw[16];
    const auto mv = g_programModelview.find(g_program);
    // Every bike's tires (Config::marksEveryBike): every draw of a mesh the lobby's wheels found to be a
    // wheel (lobby_wheels.h), whoever's bike, whatever its model. The mud is worked out in the tire's
    // own model and its own texture's rubber, so nothing else about the wheel is needed.
    if (cfg.shaders && cfg.tireDirt > 0.0 && cfg.marksEveryBike) {
        const auto element = g_elementBuffers.find(g_vao);
        if (lobby::IsWheel(g_program, g_vao, element != g_elementBuffers.end() ? element->second : 0, draw))
            want = static_cast<float>(cfg.tireDirt);
    }
    if (want == 0.0f && cfg.shaders && cfg.tireDirt > 0.0 && !g_paintParts.empty() && postfx::FrameRawView(raw) && mv != g_programModelview.end() &&
        mv->second.frame == g_frameNumber) {
        const DeferredCaster id = MakeCaster(draw, -1, -1);
        double invRaw[16];
        if (InverseAffine(raw, invRaw)) {
            const double ahead = camera::ContentTime() - g_paintTime;
            for (const PaintPart& part : g_paintParts) {
                if (!SameDrawCall(part.id, id))
                    continue;
                double dv[16], world[16];
                for (int e = 0; e < 16; ++e)
                    dv[e] = mv->second.raw[e];
                Mul4(invRaw, dv, world);
                double d2 = 0;
                for (int r = 0; r < 3; ++r) {
                    const double x = world[r] * part.axleLocal[0] + world[4 + r] * part.axleLocal[1] + world[8 + r] * part.axleLocal[2] +
                                     world[12 + r];
                    const double expected = part.world[r] + part.velocity[r] * ahead;
                    d2 += (x - expected) * (x - expected);
                }
                if (d2 < (0.5 * g_paintUnits) * (0.5 * g_paintUnits)) {
                    // Only a wheel's own parts: with every bike's wheels watched (lobby_wheels.h), a
                    // mesh never seen spinning like a wheel isn't one, whatever got it onto your
                    // wheel's list - next to other bikes at the gate, your bike's body did, and
                    // was covered in mud. (Each part once in the log, kept or left out.)
                    const auto element = g_elementBuffers.find(g_vao);
                    const GLuint elements = element != g_elementBuffers.end() ? element->second : 0;
                    // Only a mesh seen turning like a wheel (lobby_wheels.h, which watches every bike's
                    // parts whenever tire dirt is on). Merely "not known not to be one" let a part new to
                    // the watching through for its first moments: on joining, on a change of camera, when
                    // the tracker took a part of a bike alongside - and your bike's body and your helmet
                    // came out covered in mud for a second. A wheel of yours gets its mud as soon as it
                    // has rolled for a moment.
                    const bool wheelMesh = lobby::SeenTurning(g_program, g_vao, elements, draw);
                    static std::vector<long long> logged;
                    const long long key = (static_cast<long long>(draw.count) << 3) | (part.member ? 4 : 0) |
                                          (part.wheel ? 2 : 0) | (wheelMesh ? 1 : 0);
                    if (logged.size() < 40 && std::find(logged.begin(), logged.end(), key) == logged.end()) {
                        logged.push_back(key);
                        Log("postfx: tire dirt: %s a part on your %s wheel's list (%d indices, %s)%s",
                            wheelMesh ? "mud on" : "LEFT OUT", part.wheel ? "front" : "rear", draw.count,
                            part.member ? "kept on the wheel as it turned" : "one of the wheel tracker's own parts",
                            wheelMesh ? "" : ": not seen turning like a wheel");
                    }
                    if (wheelMesh) {
                        want = static_cast<float>(cfg.tireDirt);
                        break;
                    }
                }
            }
        }
    }
    // (WEATHER: with snow on, flecks of it in the mud. Kept with the mud's amount, so turning the snow
    // on or off sends it again as well.)
    const float snow = want > 0.0f && cfg.snow ? 1.0f : 0.0f;
    const float key = want + snow * 10.0f;
    float& set = g_mudSet[g_program];
    if (key != set) {
        real_glUniform1f(at->second, want);
        if (want > 0.0f && real_glGetUniformLocation) {
            const GLint metre = real_glGetUniformLocation(g_program, "u_mx_metre");
            if (metre >= 0)
                real_glUniform1f(metre, static_cast<float>(g_paintUnits));
            const GLint snowAt = real_glGetUniformLocation(g_program, "u_mx_snow");
            if (snowAt >= 0)
                real_glUniform1f(snowAt, snow);
        }
        set = key;
    }
}

void DrawWheelMasks(const postfx::OwnAnchors* own)
{
    ++g_memberFrame;
    std::vector<PaintPart> paint;
    double invRaw[16];
    const bool frame = own && InverseAffine(own->raw, invRaw);
    const auto Axle = [&](const NearDraw& d, const double local[3], double out[3]) {
        double vm[16], world[16];
        for (int k = 0; k < 16; ++k)
            vm[k] = d.raw[k];
        Mul4(invRaw, vm, world);
        for (int r = 0; r < 3; ++r)
            out[r] = world[r] * local[0] + world[4 + r] * local[1] + world[8 + r] * local[2] + world[12 + r];
    };
    std::vector<const NearDraw*> draws;
    for (int i = 0; i < 2; ++i) {
        MaskWheel& m = g_maskWheels[i];
        float viewModels[postfx::kWheelMaskParts][16];
        double axleLocals[postfx::kWheelMaskParts][3];
        const int given = postfx::WheelMaskDraws(i, viewModels, axleLocals, postfx::kWheelMaskParts);
        const NearDraw* found[postfx::kWheelMaskParts] = {};
        int count = 0;
        if (given > 0) {
            // The parts as the wheel tracker gives them this frame: kept for the frames it doesn't.
            for (int k = 0; k < given; ++k) {
                for (const NearDraw& d : g_nearDraws)
                    if (std::equal(d.raw, d.raw + 16, viewModels[k])) {
                        m.parts[count].id = d.caster;
                        std::copy(axleLocals[k], axleLocals[k] + 3, m.parts[count].axleLocal);
                        found[count++] = &d;
                        break;
                    }
            }
            m.count = count;
            static int logged[2] = { 0, 0 };  // (each new most, once: how much of the wheel the mask has)
            if (count > logged[i]) {
                logged[i] = count;
                Log("postfx: tire dirt: your %s wheel's mask has %d part(s) of it (tire, rim, disc...)", i ? "front" : "rear", count);
            }
        } else if (m.count > 0 && frame) {
            for (int k = 0; k < m.count; ++k) {
                MaskPart& part = m.parts[k];
                double expected[3], best = 0.4 * own->unitsPerMetre;
                for (int c = 0; c < 3; ++c)
                    expected[c] = part.world[c] + own->riderPosition[c] - part.rider[c];
                for (const NearDraw& d : g_nearDraws) {
                    if (!SameDrawCall(d.caster, part.id))
                        continue;
                    double at[3];
                    Axle(d, part.axleLocal, at);
                    const double dx = at[0] - expected[0], dy = at[1] - expected[1], dz = at[2] - expected[2];
                    const double off = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (off < best) {
                        best = off;
                        found[k] = &d;
                    }
                }
            }
            count = m.count;
        } else if (!frame) {
            m.count = 0;
        }
        const NearDraw* main = nullptr;
        int mainPart = -1;
        const size_t first = draws.size();
        for (int k = 0; k < count; ++k) {
            if (!found[k])
                continue;
            if (!main) {
                main = found[k];
                mainPart = k;
            }
            if (frame) {
                Axle(*found[k], m.parts[k].axleLocal, m.parts[k].world);
                std::copy(own->riderPosition, own->riderPosition + 3, m.parts[k].rider);
            }
            for (const NearDraw& d : g_nearDraws)  // every program's copy of it
                if (std::equal(d.raw, d.raw + 16, found[k]->raw))
                    draws.push_back(&d);
        }
        const size_t membersFrom = draws.size();  // (the tracker's own parts before it, the ones kept on the wheel after)
        if (main && frame)
            FindWheelMembers(i, *main, invRaw, *own, draws);
        else
            g_haveLastWheel[i] = g_haveLastWheelInWorld[i] = false;
        // For PaintTireDirt next frame: each of these draws, with the wheel's axle in its own model
        // (where it'll be checked for), the axle's place now and how fast it moves.
        if (main && frame) {
            const double* axle = m.parts[mainPart].world;
            const double now = camera::ContentTime();
            double velocity[3] = {};
            if (g_havePaintAxle[i] && now > g_paintAxleTime[i])
                for (int c = 0; c < 3; ++c)
                    velocity[c] = (axle[c] - g_paintAxle[i][c]) / (now - g_paintAxleTime[i]);
            std::copy(axle, axle + 3, g_paintAxle[i]);
            g_paintAxleTime[i] = now;
            g_havePaintAxle[i] = true;
            for (size_t k = first; k < draws.size(); ++k) {
                double dv[16], world[16], inv[16];
                for (int e = 0; e < 16; ++e)
                    dv[e] = draws[k]->raw[e];
                Mul4(invRaw, dv, world);
                if (!InverseAffine(world, inv))
                    continue;
                PaintPart part;
                part.id = draws[k]->caster;
                part.wheel = i;
                part.member = k >= membersFrom;
                for (int r = 0; r < 3; ++r)
                    part.axleLocal[r] = inv[r] * axle[0] + inv[4 + r] * axle[1] + inv[8 + r] * axle[2] + inv[12 + r];
                std::copy(axle, axle + 3, part.world);
                std::copy(velocity, velocity + 3, part.velocity);
                paint.push_back(part);
            }
        } else {
            g_havePaintAxle[i] = false;
        }
    }
    g_paintParts.swap(paint);
    g_paintTime = camera::ContentTime();
    g_paintUnits = own ? own->unitsPerMetre : 3.2808;
}

// Your rider's pieces (see RiderDraw): kept while drawn, the left-out ones cast with this frame's bones.
void CastRiderPieces(const postfx::OwnAnchors& own)
{
    double invRaw[16];
    if (!InverseAffine(own.raw, invRaw))
        return;
    const double u = own.unitsPerMetre;
    // Your rider's draws this frame: every bone within 1.5 m of your rider, and your rider's own: one of
    // its bones is your rider's hips, or sits exactly on a bone of a piece that has them (a piece can be
    // drawn with only some of the skeleton: a torso with its upper bones). Within 1.5 m alone, the
    // riders beside you at the gate were yours too: their pieces were kept as yours, then cast on your
    // bones - another rider's shape stuck to your shadow.
    std::vector<const RiderDraw*> mine;
    std::vector<std::vector<double>> models;  // each one's model (world), from its u_modelview
    struct NearPiece { const RiderDraw* draw; std::vector<double> model, bones; bool hips; };
    std::vector<NearPiece> nearby;
    const double tight = (0.05 * u) * (0.05 * u);
    for (const RiderDraw& r : g_riderDraws) {
        double vm[16], world[16];
        for (int i = 0; i < 16; ++i)
            vm[i] = r.raw[i];
        Mul4(invRaw, vm, world);
        NearPiece n{ &r, std::vector<double>(world, world + 16), {}, false };
        bool close = true;
        for (int b = 0; b < r.bones.count && close; ++b) {
            const GLfloat* p = r.bones.position + b * 3;
            double at[3], d2 = 0, h2 = 0;
            for (int k = 0; k < 3; ++k) {
                at[k] = world[k] * p[0] + world[4 + k] * p[1] + world[8 + k] * p[2] + world[12 + k];
                d2 += (at[k] - own.riderPosition[k]) * (at[k] - own.riderPosition[k]);
                h2 += (at[k] - own.hips[k]) * (at[k] - own.hips[k]);
            }
            close = d2 < (1.5 * u) * (1.5 * u);
            n.hips = n.hips || h2 < tight;
            n.bones.insert(n.bones.end(), at, at + 3);
        }
        if (close)
            nearby.push_back(n);
    }
    std::vector<double> yours;  // the bones of your pieces that have your hips
    for (const NearPiece& n : nearby)
        if (n.hips)
            yours.insert(yours.end(), n.bones.begin(), n.bones.end());
    for (const NearPiece& n : nearby) {
        bool same = !own.torso || n.hips;  // (no skeleton of yours to tell by: within 1.5 m, as before)
        for (size_t b = 0; b + 2 < n.bones.size() && !same; b += 3)
            for (size_t y = 0; y + 2 < yours.size() && !same; y += 3) {
                const double dx = n.bones[b] - yours[y], dy = n.bones[b + 1] - yours[y + 1], dz = n.bones[b + 2] - yours[y + 2];
                same = dx * dx + dy * dy + dz * dz < tight;
            }
        if (same) {
            mine.push_back(n.draw);
            models.push_back(n.model);
        }
    }
    postfx::NoteRiderPiecesDrawn(static_cast<int>(mine.size()));  // (whole: no shape stands in for anything)
    if (mine.empty())
        return;  // your rider isn't drawn this frame
    // (All of your rider drawn: nothing of it is left out to cast. Counted by pieces alone, some riders'
    // gear - drawn in two pieces whatever the view - read as left out with the camera close behind, and
    // copies of the body were cast into the rider's own: jagged patches on the hoodie. Only with the
    // game's camera at your rider's eyes does it leave anything out.)
    const bool whole = mine.size() >= 5 || !postfx::CameraAtYourHead();

    for (size_t i = 0; i < mine.size(); ++i) {
        const RiderDraw& r = *mine[i];
        KeptRiderPiece* piece = nullptr;
        for (KeptRiderPiece& p : g_riderPieces)
            if (p.used && SameDrawCall(p.caster, r.caster)) {
                piece = &p;
                break;
            }
        if (!piece) {
            if (replay::Current() != replay::Mode::Off)
                continue;  // a replay's rider can be someone else: nothing new is kept from it
            for (KeptRiderPiece& p : g_riderPieces)
                if (!p.used) {
                    piece = &p;
                    break;
                }
            if (!piece) {
                if (g_riderPieces.size() >= kMaxRiderPieces)
                    continue;
                g_riderPieces.emplace_back();
                piece = &g_riderPieces.back();
            }
            *piece = KeptRiderPiece{};
            piece->used = true;
        }
        piece->caster = r.caster;
        const auto buffers = g_vaoArrayBuffers.find(r.caster.vao);
        piece->buffers = buffers != g_vaoArrayBuffers.end() ? buffers->second : std::vector<GLuint>{};
        if (r.caster.elementBuffer)
            piece->buffers.push_back(r.caster.elementBuffer);
        piece->count = r.bones.count;
        std::copy(r.bones.centre, r.bones.centre + r.bones.count * 3, piece->centre);
        piece->drawnFrame = g_riddenFrame;
    }

    // (Which ones the game left out, for next frame: DrawRealBodyAlong draws them with your rider.)
    for (KeptRiderPiece& piece : g_riderPieces)
        piece.leftOut = piece.used && !whole && piece.drawnFrame != g_riddenFrame;

    // The pieces left out: each bone by its rest position, from whichever of this frame's draws has it.
    const double same = 1e-3 * u;
    for (KeptRiderPiece& piece : g_riderPieces) {
        if (!piece.used || piece.drawnFrame == g_riddenFrame || whole)
            continue;
        if (g_deferredCasters.size() >= 8192)
            break;
        CasterBones bones;
        bones.count = piece.count;
        int model = -1;
        bool all = true;
        for (int b = 0; b < piece.count && all; ++b) {
            const GLfloat* c = piece.centre + b * 3;
            bool found = false;
            for (size_t i = 0; i < mine.size() && !found; ++i) {
                const CasterBones& from = mine[i]->bones;
                for (int k = 0; k < from.count && !found; ++k) {
                    const GLfloat* o = from.centre + k * 3;
                    if (std::fabs(o[0] - c[0]) + std::fabs(o[1] - c[1]) + std::fabs(o[2] - c[2]) > same)
                        continue;
                    found = true;
                    std::copy(o, o + 3, bones.centre + b * 3);
                    std::copy(from.position + k * 3, from.position + k * 3 + 3, bones.position + b * 3);
                    std::copy(from.rotation + k * 9, from.rotation + k * 9 + 9, bones.rotation + b * 9);
                    bones.transposed = from.transposed;
                    if (model < 0)
                        model = static_cast<int>(i);
                }
            }
            all = found;
        }
        if (!all) {
            if (!piece.loggedUnmatched && g_riddenLogs < 20) {
                piece.loggedUnmatched = true;
                ++g_riddenLogs;
                Log("postfx: shadow map: a piece of your rider the game doesn't draw here (%d bones, %d indices) uses bones "
                    "the game doesn't send here; it casts no shadow",
                    piece.count, piece.caster.draw.count);
            }
            continue;
        }
        // Drawn like the piece whose bones it took (riders: the bare view), in the eye space the
        // frame is drawn from.
        double eye[16];
        Mul4(own.rendered, models[static_cast<size_t>(model)].data(), eye);
        float viewModel[16];
        for (int i = 0; i < 16; ++i)
            viewModel[i] = static_cast<float>(eye[i]);
        DeferredCaster c = piece.caster;
        if (!postfx::ShadowMatrices(viewModel, nullptr, c.lightModelview, &c.cascades))
            continue;
        c.bones = static_cast<int>(g_casterBones.size());
        g_casterBones.push_back(bones);
        g_deferredCasters.push_back(c);
        postfx::NoteRiderPartCast(true);
        if (!piece.loggedCast && g_riddenLogs < 20) {
            piece.loggedCast = true;
            ++g_riddenLogs;
            Log("postfx: shadow map: a piece of your rider the game doesn't draw here casts its shadow anyway, with your "
                "rider's bones (program %u, %d bones, %d indices)",
                piece.caster.program, piece.count, piece.caster.draw.count);
        }
    }
}

// Your wheels: a wheel the game drew this frame has its draw call kept (found among the rigid draws
// near the camera by its u_modelview); one it didn't draw is cast from the kept call, in its place
// on your bike, with the held-back casters. (A wheel never drawn this session, your rear one in
// first person, gets a shape of postfx's own instead.)
void KeepOrCastWheels()
{
    static const char* const kWheelName[2] = { "rear", "front" };
    for (int i = 1; i >= 0; --i) {
        unsigned program = 0;
        float viewModel[16];
        const postfx::WheelCast cast = postfx::OwnWheelCast(i, &program, viewModel);
        KeptWheel& kept = g_keptWheels[i];
        if (cast == postfx::WheelCast::Drawn) {
            for (const NearDraw& candidate : g_nearDraws) {
                if (candidate.caster.program != program || !std::equal(candidate.raw, candidate.raw + 16, viewModel))
                    continue;
                kept.caster = candidate.caster;
                const auto buffers = g_vaoArrayBuffers.find(candidate.caster.vao);
                kept.buffers = buffers != g_vaoArrayBuffers.end() ? buffers->second : std::vector<GLuint>{};
                if (candidate.caster.elementBuffer)
                    kept.buffers.push_back(candidate.caster.elementBuffer);
                kept.valid = true;
                if (!kept.loggedKept) {
                    kept.loggedKept = true;
                    Log("postfx: shadow map: kept your %s wheel's draw call (program %u), for when the game doesn't draw it",
                        kWheelName[i], program);
                }
                break;
            }
        }
        // (Its kept call is never cast in its place: placed from where it was learnt on your bike it
        // could sit off the real one, two front tires in your shadow. A stand-in shape of postfx's
        // covers a wheel the game doesn't draw.)
        // The game drew that call near you this frame: the wheel is drawn (and casts its own shadow),
        // whatever the wheel tracker made of it, so no stand-in tire goes beside it. (The tracker
        // letting go of your front wheel a moment, in first person where the game always draws it,
        // put a second front tire in your shadow.)
        if (!kept.valid)
            continue;
        postfx::OwnAnchors own;
        if (!postfx::GetOwnAnchors(own))
            continue;
        double eye[3];
        for (int r = 0; r < 3; ++r)
            eye[r] = own.raw[r] * own.riderPosition[0] + own.raw[4 + r] * own.riderPosition[1] +
                     own.raw[8 + r] * own.riderPosition[2] + own.raw[12 + r];
        const double reach = 3.0 * own.unitsPerMetre;
        for (const NearDraw& d : g_nearDraws) {
            if (!SameDrawCall(d.caster, kept.caster))
                continue;
            const double dx = d.raw[12] - eye[0], dy = d.raw[13] - eye[1], dz = d.raw[14] - eye[2];
            if (dx * dx + dy * dy + dz * dz < reach * reach) {
                postfx::NoteWheelDrawnByGame(i);
                break;
            }
        }
    }
}

}  // namespace

void NoteDepthMask(bool on)
{
    g_depthWrites = on;
}

void NoteCapability(unsigned int capability, bool on)
{
    if (capability == GL_BLEND)
        g_blending = on;
}

void NoteBindTexture(unsigned int target, unsigned int texture)
{
    if (target == GL_TEXTURE_2D && g_activeUnit >= 0 && g_activeUnit < 16)
        g_boundTextures[g_activeUnit] = texture;
}

void DiscardShadowCasters()
{
    g_deferredCasters.clear();
    ClearUniformSnaps();
    g_nearDraws.clear();
    g_riderDraws.clear();
    g_casterBones.clear();
}

void FlushShadowCasters()
{
    KeepOrCastWheels();  // (the frame already counts as applied here: ShadowMatrices says if the map is on)
    {
        postfx::OwnAnchors own;
        const bool haveOwn = postfx::GetOwnAnchors(own);
        if (haveOwn) {
            UpdateRiddenParts(own);
            CastRiderPieces(own);
        }
        DrawWheelMasks(haveOwn ? &own : nullptr);
    }
    g_nearDraws.clear();
    g_riderDraws.clear();
    if (g_deferredCasters.empty()) {
        g_casterBones.clear();
        return;
    }
    const PFN_UniformMatrix4fv upload = real_glUniformMatrix4fv ? real_glUniformMatrix4fv : real_glUniformMatrix4fvARB;
    static PFN_EnableDisable enable = nullptr, disable = nullptr;
    static PFN_BindTexture bindTexture = nullptr;
    if (!enable) {
        enable = reinterpret_cast<PFN_EnableDisable>(GetRealGLFunction("glEnable"));
        disable = reinterpret_cast<PFN_EnableDisable>(GetRealGLFunction("glDisable"));
        bindTexture = reinterpret_cast<PFN_BindTexture>(GetRealGLFunction("glBindTexture"));
    }
    if (!real_glIsEnabled)
        real_glIsEnabled = reinterpret_cast<PFN_IsEnabled>(GetRealGLFunction("glIsEnabled"));
    if (!real_glDrawElements)
        real_glDrawElements = reinterpret_cast<PFN_DrawElements>(GetRealGLFunction("glDrawElements"));
    if (!real_glDrawArrays)
        real_glDrawArrays = reinterpret_cast<PFN_DrawArrays>(GetRealGLFunction("glDrawArrays"));
    if (!upload || !real_glUseProgram || !real_glBindVertexArray || !real_glActiveTexture || !enable || !disable ||
        !bindTexture || !real_glIsEnabled || !postfx::BeginShadowDraw()) {
        g_deferredCasters.clear();
        ClearUniformSnaps();
        g_casterBones.clear();
        return;
    }
    // (For the log: fixed-function state the game left on that the held-back casters are drawn with -
    // logged whenever it changes, a few times a session, to tell a lobby's from a single race's.)
    {
        if (!real_glGetIntegerv)
            real_glGetIntegerv = reinterpret_cast<PFN_GetIntegerv>(GetRealGLFunction("glGetIntegerv"));
        static PFN_GetFloatv getFloat = reinterpret_cast<PFN_GetFloatv>(GetRealGLFunction("glGetFloatv"));
        static int logged = 0;
        static char last[192] = "";
        if (real_glGetIntegerv && getFloat && logged < 12) {
            GLint profile = 0, alphaFunc = 0;
            real_glGetIntegerv(0x9126, &profile);  // GL_CONTEXT_PROFILE_MASK (0 before 3.2: compatibility)
            const bool compatibility = (profile & 1) == 0;
            GLfloat alphaRef = 0, offset[2] = {};
            const bool alphaTest = compatibility && real_glIsEnabled(GL_ALPHA_TEST) != GL_FALSE;
            if (alphaTest) {
                real_glGetIntegerv(GL_ALPHA_TEST_FUNC, &alphaFunc);
                getFloat(GL_ALPHA_TEST_REF, &alphaRef);
            }
            const bool polygonOffset = real_glIsEnabled(GL_POLYGON_OFFSET_FILL) != GL_FALSE;
            getFloat(GL_POLYGON_OFFSET_FACTOR, &offset[0]);
            getFloat(GL_POLYGON_OFFSET_UNITS, &offset[1]);
            char now[192];
            snprintf(now, sizeof now, "alpha test %s (func 0x%X ref %.2f), polygon offset %s (%.1f %.1f), discard %d, clip %d, "
                     "alpha to coverage %d, depth clamp %d",
                     alphaTest ? "ON" : "off", alphaFunc, alphaRef, polygonOffset ? "ON" : "off", offset[0], offset[1],
                     real_glIsEnabled(0x8C89) != GL_FALSE, real_glIsEnabled(0x3000) != GL_FALSE,
                     real_glIsEnabled(0x809E) != GL_FALSE, real_glIsEnabled(0x864F) != GL_FALSE);
            if (strcmp(now, last) != 0) {
                strcpy_s(last, now);
                ++logged;
                Log("postfx: shadow map: the held-back casters are drawn with %s (%zu of them)", now, g_deferredCasters.size());
            }
        }
    }
    // Both faces: the nearest surface to the sun is the same either way, and two-sided leaves cast.
    const bool cull = real_glIsEnabled(GL_CULL_FACE) != GL_FALSE;
    disable(GL_CULL_FACE);
    GLuint program = 0, vao = 0;
    GLuint bound[kTrackedUnits];
    for (int u = 0; u < kTrackedUnits; ++u)
        bound[u] = g_boundTextures[u];
    std::vector<GLuint> touched, boned;  // programs used; those given bones of ours
    std::vector<GLuint> rigid;           // skinned programs holding the rigid draws' bone now (see below)
    // Each program's uniforms as GL holds them while the casters are drawn (from the game's, as
    // each program is first used): what a caster's own differ from is uploaded.
    std::unordered_map<GLuint, std::vector<UniformSlot>> uniformsNow;
    const auto UploadSlot = [](GLint location, const UniformSlot& slot) {
        switch (slot.kind) {
        case 1: if (real_glUniform1i) real_glUniform1i(location, slot.i); break;
        case 2: if (real_glUniform1f) real_glUniform1f(location, slot.v[0]); break;
        case 3: if (real_glUniform2fv) real_glUniform2fv(location, 1, slot.v); break;
        case 4: if (real_glUniform3fv) real_glUniform3fv(location, 1, slot.v); break;
        case 5: if (real_glUniform4fv) real_glUniform4fv(location, 1, slot.v); break;
        default: break;
        }
    };
    // (The bones and their count are a skinned program's own business, below.)
    const auto BoneLocation = [](const SkinnedProgram* skin, GLint location) {
        return skin && (location == skin->centreLoc || location == skin->positionLoc || location == skin->rotationLoc ||
                        location == skin->countLoc);
    };
    GLfloat attribNow[kConstAttribs][4];  // the constant attributes as they stand (the game's, then ours)
    std::copy(&g_constAttrib[0][0], &g_constAttrib[0][0] + kConstAttribs * 4, &attribNow[0][0]);
    // Cascade by cascade (each a tile of the shadow map), each caster into those it reaches.
    for (int cascade = 0; cascade < postfx::ShadowCascades(); ++cascade) {
    float cascadeProjection[16];
    postfx::UseShadowCascade(cascade, cascadeProjection);
    for (const DeferredCaster& c : g_deferredCasters) {
        if (!(c.cascades & (1u << cascade)))
            continue;
        // (Counted once each, in the first cascade it's drawn into: the far ones are in none of the
        // near cascades.)
        const bool first = (c.cascades & ((1u << cascade) - 1)) == 0;
        const auto element = g_elementBuffers.find(c.vao);
        if (c.draw.kind != DrawCall::Arrays && (element == g_elementBuffers.end() || element->second != c.elementBuffer)) {
            if (first)
                postfx::NoteReplay(c.bones >= 0, 1);
            continue;  // its vertex array was given other indices since: skip it
        }
        if (c.program != program) {
            real_glUseProgram(c.program);
            program = c.program;
            if (std::find(touched.begin(), touched.end(), program) == touched.end())
                touched.push_back(program);
        }
        if (c.vao != vao) {
            real_glBindVertexArray(c.vao);
            vao = c.vao;
        }
        for (int u = 0; u < kTrackedUnits; ++u) {
            if (bound[u] != c.textures[u]) {
                real_glActiveTexture(kTexture0Unit + u);
                bindTexture(GL_TEXTURE_2D, c.textures[u]);
                bound[u] = c.textures[u];
            }
        }
        if (c.uniforms >= 0 && c.uniforms < static_cast<int>(g_uniformSnaps.size()) &&
            g_uniformSnaps[static_cast<size_t>(c.uniforms)].program == c.program) {
            auto at = uniformsNow.find(c.program);
            if (at == uniformsNow.end())
                at = uniformsNow.emplace(c.program, g_programUniforms[c.program].now).first;
            std::vector<UniformSlot>& now = at->second;
            const SkinnedProgram* own = FindSkinned(c.program);
            for (const auto& [location, slot] : g_uniformSnaps[static_cast<size_t>(c.uniforms)].slots) {
                if (location == c.modelviewLoc || location == c.projectionLoc || BoneLocation(own, location))
                    continue;
                if (now.size() <= static_cast<size_t>(location))
                    now.resize(static_cast<size_t>(location) + 1);
                if (now[static_cast<size_t>(location)] == slot)
                    continue;
                UploadSlot(location, slot);
                now[static_cast<size_t>(location)] = slot;
                static int logged = 0;
                static std::vector<unsigned long long> seen;
                const unsigned long long key = UniformKey(c.program, location);
                if (logged < 10 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
                    seen.push_back(key);
                    ++logged;
                    Log("postfx: shadow map: '%s' (program %u) put back for the held-back casters drawn with it (the game "
                        "had changed it before they were drawn again)", UniformName(c.program, location).c_str(), c.program);
                }
            }
        }
        upload(c.modelviewLoc, 1, GL_FALSE, c.lightModelview);
        upload(c.projectionLoc, 1, GL_FALSE, cascadeProjection);
        const SkinnedProgram* skin = c.bones >= 0 ? FindSkinned(c.program) : nullptr;
        if (skin && static_cast<size_t>(c.bones) < g_casterBones.size()) {
            const CasterBones& b = g_casterBones[static_cast<size_t>(c.bones)];
            UploadSkinnedVectors(skin->centreLoc, b.count, b.centre, skin->positionArb);
            UploadSkinnedVectors(skin->positionLoc, b.count, b.position, skin->positionArb);
            UploadSkinnedRotations(skin->rotationLoc, b.count, b.transposed, b.rotation, skin->rotationArb);
            if (real_glUniform1i)
                real_glUniform1i(skin->countLoc, b.count);
            if (std::find(boned.begin(), boned.end(), c.program) == boned.end())
                boned.push_back(c.program);
            rigid.erase(std::remove(rigid.begin(), rigid.end(), c.program), rigid.end());
        } else if (c.bones >= 0) {
            if (first)
                postfx::NoteReplay(true, 3);
            continue;
        } else if (c.boneCountLoc >= 0 && real_glUniform1i) {
            // A rigid draw of the riders' program (a bike's parts, a helmet): the game draws it with no
            // bones counted and one bone at the origin. Replayed here after the riders, the program
            // still held a rider's bones, and the part was moved by one of them - into nowhere: your
            // bike cast no shadow (every bike's held-back parts, ShadowBatch=1; drawn straight away,
            // ShadowBatch=0, it did). That bone again first, as the game had it.
            real_glUniform1i(c.boneCountLoc, 0);
            if (std::find(rigid.begin(), rigid.end(), c.program) == rigid.end()) {
                if (const SkinnedProgram* own = FindSkinned(c.program)) {
                    static const GLfloat kZero[3] = { 0, 0, 0 }, kIdentity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
                    if (own->centreLoc >= 0)
                        UploadSkinnedVectors(own->centreLoc, 1, kZero, own->positionArb);
                    if (own->positionLoc >= 0)
                        UploadSkinnedVectors(own->positionLoc, 1, kZero, own->positionArb);
                    if (own->rotationLoc >= 0)
                        UploadSkinnedRotations(own->rotationLoc, 1, false, kIdentity, own->rotationArb);
                    rigid.push_back(c.program);
                    if (std::find(boned.begin(), boned.end(), c.program) == boned.end())
                        boned.push_back(c.program);  // (its bones put back as the game had them, after)
                }
            }
        }
        if (first)
            postfx::NoteReplay(c.bones >= 0, g_vaoGeneration[c.vao] != c.vaoGen ? 2 : 0);
        if (real_glVertexAttrib4f) {
            for (int k = 0; k < kConstAttribs; ++k) {
                if (!(c.attribSet & (1u << k)))
                    continue;
                if (!std::equal(c.attrib[k], c.attrib[k] + 4, attribNow[k])) {
                    real_glVertexAttrib4f(static_cast<GLuint>(k), c.attrib[k][0], c.attrib[k][1], c.attrib[k][2], c.attrib[k][3]);
                    std::copy(c.attrib[k], c.attrib[k] + 4, attribNow[k]);
                }
            }
        }
        Replay(c.draw);
    }
    }  // (each cascade)
    // The constant attributes as the game left them.
    if (real_glVertexAttrib4f) {
        for (int k = 0; k < kConstAttribs; ++k) {
            if (!(g_constAttribSet & (1u << k)))
                continue;
            if (!std::equal(g_constAttrib[k], g_constAttrib[k] + 4, attribNow[k]))
                real_glVertexAttrib4f(static_cast<GLuint>(k), g_constAttrib[k][0], g_constAttrib[k][1], g_constAttrib[k][2],
                                      g_constAttrib[k][3]);
        }
    }
    // Back as the game left it: each program's own matrices, its program, arrays and textures.
    for (GLuint p : touched) {
        real_glUseProgram(p);
        // Its other uniforms as the game left them (those a caster's own changed).
        if (const auto at = uniformsNow.find(p); at != uniformsNow.end()) {
            const ProgramUniforms& game = g_programUniforms[p];
            const SkinnedProgram* own = FindSkinned(p);
            for (GLint location : game.used) {
                const size_t l = static_cast<size_t>(location);
                if (l >= game.now.size() || l >= at->second.size() || game.now[l].kind == 0 || BoneLocation(own, location))
                    continue;
                if (at->second[l] != game.now[l])
                    UploadSlot(location, game.now[l]);
            }
        }
        const auto mv = g_programModelview.find(p);
        if (mv != g_programModelview.end() && mv->second.location >= 0)
            upload(mv->second.location, 1, GL_FALSE, mv->second.uploaded);
        const auto projection = g_programProjection.find(p);
        if (projection != g_programProjection.end() && projection->second.location >= 0)
            upload(projection->second.location, 1, GL_FALSE, projection->second.value);
        const SkinnedProgram* skin = FindSkinned(p);
        if (skin && skin->positionLoc >= 0 && skin->countLoc >= 0 && real_glUniform1i)
            real_glUniform1i(skin->countLoc, skin->bones);  // the game's bone count again
        if (skin && std::find(boned.begin(), boned.end(), p) != boned.end()) {
            // Its bones as they were: the game's, or ours with your torso hidden.
            if (skin->centres > 0)
                UploadSkinnedVectors(skin->centreLoc, skin->centres, skin->centre, skin->positionArb);
            if (skin->positions > 0)
                UploadSkinnedVectors(skin->positionLoc, skin->positions,
                                     skin->positionEdited ? skin->hiddenPosition : skin->position, skin->positionArb);
            if (skin->rotations > 0)
                UploadSkinnedRotations(skin->rotationLoc, skin->rotations, skin->transposed,
                                       skin->rotationEdited ? skin->hiddenRotation : skin->rotation, skin->rotationArb);
        }
    }
    real_glUseProgram(g_program);
    real_glBindVertexArray(g_vao);
    for (int u = 0; u < kTrackedUnits; ++u) {
        if (bound[u] != g_boundTextures[u]) {
            real_glActiveTexture(kTexture0Unit + u);
            bindTexture(GL_TEXTURE_2D, g_boundTextures[u]);
        }
    }
    real_glActiveTexture(kTexture0Unit + static_cast<GLenum>(g_activeUnit));
    if (cull)
        enable(GL_CULL_FACE);
    postfx::EndShadowDraw();
    if (!g_loggedShadowBatch) {
        g_loggedShadowBatch = true;
        Log("postfx: shadow map: %d objects drawn together, %d one at a time (riders, bikes, streamed geometry)",
            static_cast<int>(g_deferredCasters.size()), g_castAtOnce);
    }
    g_deferredCasters.clear();
    ClearUniformSnaps();
    g_casterBones.clear();
}

bool NoteDraw(const DrawCall& draw)
{
    if (!g_terrain.isTerrain && postfx::WantsSceneEnd())
        CheckSceneEnd();
    if (!g_terrain.isTerrain) {
        if (postfx::HidesGameFlare() && IsGameFlare()) {
            DumpCall("^ SKIPPED  the game's lens flare (a time of day is set)");
            return false;
        }
        if (LeaveOutGameOverlay(draw))
            return false;
        // (For the trace: a 3D object after the effects were applied this frame gets no shadow.)
        if (postfx::SceneApplied()) {
            const auto mv = g_programModelview.find(g_program);
            if (mv != g_programModelview.end() && mv->second.frame == g_frameNumber && mv->second.kind == ModelviewKind::Scene &&
                HasTranslation(mv->second.raw))
                postfx::NoteDrawAfterScene(g_program);
        }
        PrepareSkinnedDraw();
        // Every bike's wheels (lobby_wheels.h): this rigid 3D draw, by its mesh and where it is.
        {
            const auto mv = g_programModelview.find(g_program);
            if (mv != g_programModelview.end() && mv->second.frame == g_frameNumber && mv->second.kind == ModelviewKind::Scene &&
                !mv->second.transposed && HasTranslation(mv->second.raw) && StaticDraw(draw)) {
                const auto element = g_elementBuffers.find(g_vao);
                const GLuint elements = element != g_elementBuffers.end() ? element->second : 0;
                if (lobby::NoteRigidDraw(g_program, g_vao, elements, draw, mv->second.raw)) {
                    DrawBounds bounds;  // (a wheel's shape: its axle's centre and its tire's radius)
                    const bool ok = MeasureDraw(draw, bounds);
                    lobby::GiveBounds(g_program, g_vao, elements, draw, ok, bounds.lo, bounds.hi);
                }
            }
        }
        if (postfx::WantsShadowCasters() || postfx::WantsWheelMask()) {
            const auto castStart = std::chrono::steady_clock::now();
            CastShadow(draw);
            postfx::NoteCastNowMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - castStart).count());
        }
        PaintTireDirt(draw);
        return true;
    }
    const auto found = g_elementBuffers.find(g_vao);
    const GLuint elementBuffer = found != g_elementBuffers.end() ? found->second : 0;
    if (IsDumping())
        DumpCall("%s prog=%u mode=0x%X count=%d type=0x%X offset=%p first=%d vertexArray=%u elementBuffer=%u",
                 draw.kind == DrawCall::Elements ? "glDrawElements" : draw.kind == DrawCall::Arrays ? "glDrawArrays" : "glDrawRangeElements",
                 g_program, draw.mode, draw.count, draw.type, draw.indices, draw.first, g_vao, elementBuffer);

    std::vector<GamePatch>& patches = g_terrain.patches;
    if (patches.empty())
        patches.reserve(512);
    if (patches.empty() || !SamePatch(patches.back().patch, g_terrain.current) || patches.back().vao != g_vao) {
        patches.emplace_back();
        patches.back().patch = g_terrain.current;
        patches.back().vao = g_vao;
    }
    GamePatch& patch = patches.back();
    if (patch.drawCount < kMaxPatchDraws)
        patch.draws[patch.drawCount++] = draw;
    else
        patch.replayable = false;
    if (draw.kind != DrawCall::Arrays && elementBuffer == 0)
        patch.replayable = false;
    return true;
}

bool EndShaderFrame()
{
    g_deferredCasters.clear();  // a frame whose shadows weren't applied
    ClearUniformSnaps();
    g_nearDraws.clear();
    g_riderDraws.clear();
    g_casterBones.clear();
    g_castAtOnce = 0;
    // A terrain pass still open at the end of the frame isn't filled in, only forgotten.
    g_ground.open = static_cast<int>(g_terrain.patches.size());
    g_terrain.patches.clear();
    // Once per look away: what the ground fill did, so a plain log shows it.
    if (g_terrain.lookingAway && !g_groundLogged) {
        g_groundLogged = true;
        Log("terrain: view %.0f deg from the game camera: %d pass(es), the game drew %d patches, %d filled in (%s), %d left open",
            g_ground.awayDeg, g_ground.passes, g_ground.game, g_ground.filled, g_ground.note[0] ? g_ground.note : "-",
            g_ground.open);
    } else if (!g_terrain.lookingAway && g_ground.awayDeg >= 0) {
        g_groundLogged = false;
    }
    g_terrain.haveEye = false;
    g_terrain.lookingAway = false;
    const bool captured = g_camera.capturedThisFrame;
    // The mod's own camera ran this frame (with the mod off the look keys may still have turned
    // the game's: captured, but no bike tracker, replay camera or camera trace).
    const bool modCamera = captured && GetConfig().enabled;
    if (modCamera && replay::Current() == replay::Mode::Off)
        bike::EndFrame(camera::GameTime(), camera::FrameInterval());
    if (IsDumping())
        Log("  shader path: camera %s, %d '%s' uploads corrected, %d sky preview(s), %d other rotation-only draw(s)",
            captured ? "found" : "NOT found", g_camera.correctedThisFrame, GetConfig().modelviewUniform,
            g_camera.previews, g_camera.otherRotations);
    TraceFrame(modCamera);
    g_ground = GroundStats{};
    replay::EndFrame(camera::ContentTime(), modCamera);
    camera::FrameTick();
    // The game has no look keys: the controller (the binds on the KEYBINDS tab, the d-pad unless
    // changed) turns the head for the next frame. Polled while the menu is open too, for binding.
    camera::Look look = camera::Look::Ahead;
    const Config& lookCfg = GetConfig();
    if (lookCfg.controllerLook || OverlayPanelOpen() || (lookCfg.dabIndicator && dab::NeedsGamepad()) ||
        brake::NeedsGamepad()) {
        gamepad::Poll();
        if (lookCfg.controllerLook) {
            const bool left = gamepad::IsDown(lookCfg.lookLeftControl);
            const bool right = gamepad::IsDown(lookCfg.lookRightControl);
            look = gamepad::IsDown(lookCfg.lookBackControl) ? camera::Look::Back
                 : left != right ? (left ? camera::Look::Left : camera::Look::Right)
                 : camera::Look::Ahead;
        }
    }
    camera::SetLook(look);
    // The mod off, a look just asked for: the next frame's sky, drawn before its camera is
    // known, is to turn with it from its first frame, so a correction (none yet) is on.
    if (LookOnly(lookCfg) && look != camera::Look::Ahead && captured && !g_camera.haveCorrection) {
        for (int i = 0; i < 16; ++i)
            g_camera.correction[i] = g_camera.rotationOnly[i] = (i % 5 == 0) ? 1.0 : 0.0;
        g_camera.haveCorrection = true;
    }
    // The brake squeal (brake.h): the brakes just read, your speed as postfx follows your rider.
    {
        double speed = 0;
        const bool haveSpeed = postfx::RiderSpeed(speed);
        brake::Update(haveSpeed, speed);
    }

    // No camera this frame (menu, effect off): don't carry a stale correction into the
    // next frame's sky.
    if (!captured)
        g_camera.haveCorrection = false;
    g_camera.capturedThisFrame = false;
    // The game's HUD and cursor (LeaveOutGameOverlay): this frame's 3D scene, and the cursor.
    if (IsDumping() && (g_hudSkipped || g_cursorSkipped))
        Log("  hud: %d HUD draw(s) and %d cursor draw(s) left out", g_hudSkipped, g_cursorSkipped);
    if (GetConfig().hideHud || GetConfig().hideCursor)
        UpdateGameMenu();
    else
        g_gameMenu.open = false;
    g_sceneOnScreen = g_shaderViewSet;
    g_hudSkipped = g_cursorSkipped = 0;
    if (GetConfig().hideCursor)
        LearnCursor();
    {
        const auto lobbyStart = std::chrono::steady_clock::now();
        lobby::FrameEnd(g_haveLobbyView ? g_lobbyView : nullptr, camera::ContentTime());
        postfx::NoteLobbyMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lobbyStart).count());
    }
    g_haveLobbyView = false;
    ++g_frameNumber;
    g_shaderViewSet = false;
    g_camera.havePreview = false;
    g_camera.skyAngle = 0;
    g_camera.previews = 0;
    g_camera.otherRotations = 0;
    g_camera.correctedThisFrame = 0;
    return captured;
}

bool SceneOnScreen()
{
    return g_sceneOnScreen.load(std::memory_order_relaxed);
}

bool GameMenuOverScene()
{
    return g_gameMenu.open;
}

const char* GameOverlayStatusText()
{
    const Config& cfg = GetConfig();
    if (!cfg.hideHud && !cfg.hideCursor)
        return "";
    static char text[200];
    const char* hud = !cfg.hideHud ? ""
                    : g_gameMenu.open ? "HUD: SHOWN WITH THE GAME'S MENU"
                    : g_loggedHudHidden ? "HUD: HIDDEN WHILE RIDING, SHOWN WITH THE GAME'S MENU" : "HUD: HIDDEN ONCE YOU RIDE";
    const char* cursor = !cfg.hideCursor ? ""
                       : g_cursorSignature ? "CURSOR: FOUND, HIDDEN WHILE RIDING (MOVE THE MOUSE TO SHOW IT)"
                                           : "CURSOR: HIDDEN WHILE RIDING (MOVE THE MOUSE TO SHOW IT)";
    snprintf(text, sizeof text, "%s%s%s", hud, hud[0] && cursor[0] ? "   \xB7   " : "", cursor);
    return text;
}

}  // namespace mxcam
