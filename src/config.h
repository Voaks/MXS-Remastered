// config.h
//   Tunables, loaded from MXS_REMASTERED.ini (see paths.h). The defaults below match the shipped
//   MXS_REMASTERED.ini; see that file for what each value does in practice.
#pragma once

#include <string>

#include "gamepad.h"

namespace mxcam {

// What one cached rut segment costs in memory (postfx.cpp's MarkSeg: four doubles for its two
// contacts, and four floats for the rut's half width, for how far its first contact lies along
// the wheel's run of ruts and for how far that run's end turn runs out), so the settings panel
// and the log can say what a segment count costs. The index that keeps big counts cheap to
// re-press adds about 2 bytes a segment of boxes on top.
constexpr unsigned long long kMarkSegBytes = 48;

struct Config {
    // [General]
    bool   enabled   = true;
    int    toggleKey = 0x77;  // VK_F8
    int    reloadKey = 0x78;  // VK_F9
    int    dumpKey   = 0x79;  // VK_F10
    int    menuKey   = 0x45;  // 'E': in-game settings panel
    int    traceKey  = 0x7A;  // VK_F11: save the last 15 s (+5 s) of per-frame trace

    // [Detection]
    // Shader path (MX Simulator): matrices arrive as glUniformMatrix4fv uploads. Both names are
    // read once at startup, because uniform roles are assigned as the game looks them up.
    char   modelviewUniform[64]     = "u_modelview";  // per-object View * Model uniform
    char   cameraProgramUniform[64] = "u_heights";    // a uniform only the terrain shader has
    char   lightUniform[64]         = "u_light_position";  // light direction, sent in eye space
    char   envUniform[64]           = "u_envmat";    // reflection lookup rotation (eye -> world)
    int    viewMatrixIndex  = 0;          // fixed-function path: which perspective-pass modelview load is the camera
    double worldUp[3]       = { 0, 1, 0 };
    double snapAngleDeg     = 60.0;       // bigger jumps are camera cuts: reset the filters
    double teleportDistance = 5.0;
    double bikeSearchRadius = 6.0;        // how close to the camera the rider's bike is drawn

    // [Camera]
    bool   customCamera   = false;        // build the view from the bike, ignoring the game's 1st-person settings
    double cameraAngleDeg = 8.0;          // custom camera: look-down angle (positive = down)
    double headForward    = 0.0;          // eye offset along the bike, units (+ = forward)
    double headHeight     = 0.0;          // eye offset along the bike's up axis (+ = up)
    bool   hideOwnBody    = false;        // hide your own rider's torso and head (for high FOVs)

    // [Smoothing]
    double rotationResponse = 14.0;       // 1/s, 0 = off
    double maxLagDeg        = 12.0;

    // [Leveling]
    double horizonLock          = 0.6;    // 0..1 fraction of roll removed
    double pitchLock            = 0.5;    // 0..1 fraction of the bike's pitch removed (wheelies, jump faces)
    double maxRollCorrectionDeg = 90.0;
    double steadiness           = 0.6;    // 0..2 how much of the bike's quick jolts (bumps, landings, suspension) the view ignores

    // [Air]
    double airLooseness     = 0.6;        // 0..1 how much of the nose's left / right swing in the air the view lets pass
    double whipLock         = 0.0;        // 0..1 in the air: hold the view on the direction of travel, level

    // [Look] a controller looks left / right / back (the game has no look keys)
    bool   controllerLook  = true;
    // ...with these (gamepad::Control: any button, trigger, stick direction or paddle). Keybinds,
    // like the keys: not part of the presets. In the ini by name: LookLeftControl=DPAD_LEFT.
    int    lookLeftControl  = gamepad::kDPadLeft;
    int    lookRightControl = gamepad::kDPadRight;
    int    lookBackControl  = gamepad::kDPadDown;
    double lookSideDeg     = 90.0;        // how far left / right looks turn the head
    double lookTurnSpeed   = 12.0;        // 1/s how quickly the head turns (12: 90 deg in about 0.25 s)
    double apexLook        = 0.0;         // 0..1 in corners the head turns toward the apex
    double apexSpeed       = 2.5;         // 1/s how quickly it turns in and back out
    double apexAnticipation = 0.0;        // s how far ahead it reads the corner (work in progress: always 0)
    // extra eye offsets while looking sideways / back (custom camera)
    double lookSideForward = 0.0;
    double lookSideHeight  = 0.0;
    double lookBackForward = 0.0;
    double lookBackHeight  = 0.0;

    // [Neck]
    double verticalResponse = 20.0;       // 1/s, 0 = off
    double maxVerticalLag   = 0.05;
    double compressionPerG  = 0.012;      // units of sag per g of vertical acceleration
    double springHz         = 3.0;
    double dampingRatio     = 0.5;
    double maxCompression   = 0.07;
    double maxExtension     = 0.04;
    double nodDegPerMeter   = 40.0;
    double accelFilterHz    = 12.0;

    // [Replay]
    int    replayCamera     = 0;          // 0 = the game's replay camera, 1 = 3rd person, 2 = helmet
    double replayDistance   = 4.5;        // 3rd person camera: m behind the rider
    double replayHeight     = 1.55;       // m above the rider (the middle of its body)
    double replayPitchDeg   = 12.0;       // look-down angle (positive = down)
    // helmet camera: a camera mounted on top of the rider's helmet, placed from its eyes
    int    helmetMount      = 0;          // 0 = on top of the helmet, 1 = its left side, 2 = its right side, 3 = the chest
    double helmetHeight     = 0.7;        // top mount: above the eyes, along the head's up axis
    double helmetForward    = 0.0;        // top mount: along the head's forward axis (+ = forward)
    double helmetTiltDeg    = 20.0;       // top mount: how far the camera is angled down
    double sideHeight       = 0.35;       // side mount: the same three, kept apart from the top mount's
    double sideForward      = -0.6;
    double sideTiltDeg      = 5.0;
    double sideOut          = 0.6;        // side mount: how far out from the middle of the head
    double chestHeight      = -0.55;      // chest mount: above the middle of the shoulders (- = below), along the torso
    double chestForward     = 0.45;       // chest mount: in front of the shoulders
    double chestTiltDeg     = 10.0;       // chest mount: how far the camera is angled down from the torso
    double helmetSmoothing  = 0.3;        // 0..1 stabilisation of its rotation (0 = every jolt of the head)
    double helmetLevel      = 0.0;        // 0..1 fraction of the head's roll removed

    // [Graphics] shaders (postfx.cpp) and texture filtering (texfilter.cpp). Pure OpenGL: the
    // finished 3D image (colour + depth) is reworked before the HUD is drawn over it.
    bool   shaders          = true;       // master switch for every effect below
    int    shaderToggleKey  = 0x76;       // VK_F7: flips the master switch
    bool   applyBeforeHud   = true;       // run the effects before the HUD (else at the end of the frame)
    double timeOfDay        = -1.0;       // hours 0..24; -1 = the game's own lighting
    bool   moveSun          = true;       // the time of day also turns the game's sun (its lighting)
    double nightBrightness  = 1.0;        // 0..2 how much moonlight the night keeps (1 = default)
    bool   hideGameFlare    = true;       // with a time of day set: hide the game's lens flare (it stays at the game's sun)
    bool   invertLight      = false;      // the light uniform points away from the sun, not toward it
    double shadows          = 0.6;        // 0..1 sun shadows
    bool   shadowMap        = true;       // shadow maps: objects drawn again from the sun (off: screen-space only)
    double shadowDistance   = 20.0;       // m (real metres: the game's unit is feet): shadow maps cover this far around the camera
    int    shadowResolution = 0;          // 0 = 2048, 1 = 4096, 2 = 8192 texels per side
    int    shadowMapSize    = 2048;       // texels per side (from shadowResolution)
    double shadowSoftness   = 0.5;        // 0..1 detailed shadows' edges: 0 crisp, 1 soft (and the steps a
                                          // low sun leaves on a berm's face smoothed the most)
    bool   shadowBatch      = true;       // static objects' shadows drawn together (0: one at a time)
    double shadowLength     = 6.0;        // m: how far a screen-space shadow can reach
    double headlight        = 0.8;        // 0..1 the bike's headlight (when the time of day makes it dark)
    double headlightBeamDeg = 22.0;       // the beam's half-angle
    double headlightForward = 0.55;       // m: the front plate, from the rider's eyes along the bike
    double headlightHeight  = -0.45;      // m: the same, up the bike (negative = below the eyes)
    double headlightAimDeg  = 0.0;        // where the beam's top edge is aimed, degrees above level (up: it
                                          // reaches further ahead; down: nearer)
    // WEATHER (postfx: the composite pass and the snowfall pass)
    bool   rain             = false;      // rain: drops falling around you, wet ground and bikes, puddles, a
                                          // dark overcast sky (postfx: the composite, the rainfall pass; the
                                          // bike shader's wet shine, proc_hooks PaintWet)
    double rainfall         = 0.6;        // 0..1 how hard it rains (the drops around you, the ripples in the
                                          // puddles, the haze in the distance); 0: wet, nothing falling
    double rainWind         = 0.15;       // 0..1 how hard the wind blows the rain sideways
    double wetness          = 0.75;       // 0..1 how wet everything looks: darker dirt, and the shine on the
                                          // ground, the bikes and the riders
    double puddles          = 0.5;        // 0..1 how much standing water: puddles on the flat ground, water in
                                          // the deepest ruts
    double rainReflections  = 0.8;        // 0..1 how strongly the puddles and wet surfaces reflect (the sky,
                                          // and what's on screen: bikes, trees, the track's banners)
    bool   snow             = false;      // snow on the ground (terrain only; your ruts dig through it to the
                                          // dirt) and an overcast sky; off = no weather
    double snowfall         = 0.5;        // 0..1 how heavily it falls (flakes around you, and the haze they make)
    double snowWind         = 0.2;        // 0..1 how hard the wind blows the falling snow
    bool   ultraGraphics    = false;      // more ground detail, further out, lit more strongly (postfx Dirt)
    int    headlightColor   = 0;          // the headlight's colour (every bike's): 0 white, 1 amber, 2 rainbow
    bool   headlightEveryBike = true;     // a headlight on every bike around you too, not just yours (found
                                          // by their wheels: lobby_wheels.h)
    double ambientOcclusion = 0.5;        // 0..1
    double aoRadius         = 0.8;        // m
    double sunRays          = 0.35;       // 0..1
    double bloom            = 0.3;        // 0..1
    double bloomThreshold   = 0.7;        // linear brightness where bloom starts
    double exposure         = 0.0;        // EV
    double contrast         = 0.0;        // -0.5..0.5
    double saturation       = 0.0;        // -1..1
    double vignette         = 0.2;        // 0..1
    int    antiAliasing     = 1;          // 0 = off, 1 = FXAA, 2 = FXAA high quality
    double sharpen          = 0.25;       // 0..1
    double anisotropy       = 16.0;       // 1 = off
    bool   trilinear        = true;       // bilinear mipmapped textures blend between mip levels
    double textureLodBias   = 0.0;        // negative = sharper (can shimmer)
    double dirtDetail       = 0.6;        // 0..1 extra fine detail on the ground
    double dirtScale        = 1.0;        // size of the detail (2 = twice as big)
    double tireMarks        = 0.7;        // 0..1 grooves your tires leave in the dirt
    double markCacheSegs    = 40000.0;    // segments per wheel (10000..1000000) of your ruts kept
    bool   marksEveryBike   = false;      // tire marks from every bike in the lobby, not just yours (lobby_wheels.h)
                                          // in memory: how far back along your ride they come
                                          // back exactly as they were (the panel shows the memory
                                          // it costs: kMarkSegBytes each, both wheels)
    double markDepth        = 0.5;        // 0..1: how deep the ruts' floors read below the ground:
                                          // the map doubles as a height, and the mark's floor is
                                          // sampled where this pixel's ray reaches down into it,
                                          // so the near lip covers the floor and the far side
                                          // shows at low angles (0: flat on the ground)
    double tireDirt         = 0.6;        // 0..1 mud caked on your tires
    double motionBlur       = 0.0;        // 0..1 camera motion blur
    double depthOfField     = 0.0;        // 0..1
    double dofFocus         = 0.0;        // m; 0 = auto-focus on the middle of the screen
    bool   dumpShaders      = false;      // writes the game's shader sources to MXS_REMASTERED_shaders\ (diagnostics)

    // [Other] quality of life
    bool   hideHud          = false;      // hide the game's HUD while riding (it shows again while you use the mouse)
    bool   hideCursor       = false;      // hide the game's mouse cursor while riding (same)
    double brakeSqueal      = 0.0;        // 0..1 the brake squeal's loudness (brake.cpp; 0 = off): OTHER, SOUND MODS
    bool   dabIndicator     = false;      // bottom right: red standing, green while the game's Sit/Dab key sits you (dab.cpp)
    int    dabSticky        = 0;          // the indicator's Sticky Sit/Dab: 0 = as the game's config file says, 1 = off
                                          // (held), 2 = on (a press switches). The game writes its file only now and
                                          // then (not when the setting is changed), so it can say the old one.
    bool   fadeMenu         = true;       // fade this menu while a graphics setting is being adjusted
    bool   fullscreenWindow = false;      // windowed mode: the window made borderless, covering its monitor
                                          // (a display choice: not part of the presets or DEFAULTS)

    // [Other] sound mods: which engine sound packs are installed (see sound_packs.h). What's in the
    // game folder is what counts: read from it at startup, never from the ini, and not part of the
    // presets or DEFAULTS.
    bool   soundPack125     = false;
    bool   soundPack250     = false;
    bool   soundPack250f    = false;
    bool   soundPack450f    = false;
    bool   soundPack450fClear = false;    // the 450F's other pack: the limiter a little more muffled, the rest less
};

const Config& GetConfig();
Config& EditConfig();  // for the settings panel; changes apply from the next frame

// Remembers the path and loads it. A missing file just leaves the defaults in place.
void LoadConfig(const wchar_t* iniPath);
void ReloadConfig();
void SetEnabled(bool enabled);

// The shadow map's texels per side, from the Shadow resolution choice as it is now (a change in
// the menu applies straight away, without reloading the ini).
inline int ShadowMapSize(const Config& c)
{
    return 2048 << (c.shadowResolution < 0 ? 0 : c.shadowResolution > 2 ? 2 : c.shadowResolution);
}

// Writes Enabled, MenuKey and the camera tunables back to MXS_REMASTERED.ini (comments and other keys are kept).
bool SaveConfig();

// What the mod learnt about your bike, kept in the ini's [Learned] section between sessions
// (three numbers). Read returns false when the key isn't there.
bool ReadLearned(const char* key, double value[3]);
void WriteLearned(const char* key, const double value[3]);
void ForgetLearned(const char* key);
// Puts the camera and graphics tunables back to their defaults; keys and detection settings are untouched.
void ResetTunables();

// Copies every setting the presets and DEFAULTS cover (the FIRST PERSON, GRAPHICS, REPLAY and OTHER
// tabs; not the keybinds or the detection settings).
void CopyTunables(const Config& from, Config& to);

// Presets: five whole sets of those settings to switch between. The live settings are always the
// active preset's. Switching keeps them with the preset they belong to and loads the other's (not
// written to the ini until SaveConfig, which writes them all). A preset never saved starts as a
// copy of the settings in use.
constexpr int kPresetCount = 5;
int  ActivePreset();
void SwitchPreset(int index);

// Each preset's name, as the menu shows it (yours to set: printable ASCII, up to kPresetNameMax
// characters; empty: its number). Kept apart from the settings: switching, DEFAULTS and loading a
// preset code never change it. In the ini's [Presets] (Name1 .. Name5), written by SaveConfig.
constexpr int kPresetNameMax = 16;
std::string PresetName(int index);         // as set ("" if never named)
std::string PresetLabel(int index);        // as shown: the name, or its number
void SetPresetName(int index, const std::string& name);

// Preset codes, to share settings as one line of text (on Discord): one for the first person tab and
// one for the graphics tab (tab: overlay's kTabs, 0 or 1; the other tabs are preferences, with none).
// "MXS-REMASTERED-CAM-" or "MXS-REMASTERED-GFX-" and that tab's settings that aren't at their defaults,
// packed. PresetCode gives the active preset's (as it is now, saved or not); LoadPresetCode finds a
// code anywhere in text (a whole message pasted is fine) and loads it into the active preset: every
// setting of its tab to the code's (its default where the code doesn't name it), every other tab's
// kept (not saved until SaveConfig).
bool HasPresetCode(int tab);
std::string PresetCode(int tab);
struct PresetCodeResult {
    enum Status { NotFound, Damaged, Newer, Loaded } status = NotFound;
    int tab = -1;      // the tab it's for
    int settings = 0;  // settings it set
    int unknown = 0;   // settings it names that this version doesn't have (left out)
};
PresetCodeResult LoadPresetCode(const char* text);

}  // namespace mxcam
