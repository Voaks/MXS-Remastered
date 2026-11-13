// postfx_shaders.h
//   GLSL for the post-processing passes (postfx.cpp). Every fragment shader is compiled as
//   "#version 150" (or 130) + kPrelude + its body, and drawn as one fullscreen triangle.
//
//   Conventions: v_uv is 0..1 with y up (OpenGL). Depth is the window depth buffer (0..1). View
//   space is the camera's: x right, y up, looking down -z. Colours in the HDR buffers are linear;
//   the game's image is taken as gamma 2.2 and turned back at the end.
#pragma once

namespace mxcam {
namespace postfx {
namespace shaders {

constexpr const char* kVertex = R"GLSL(
in vec2 a_pos;
out vec2 v_uv;
void main()
{
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)GLSL";

// Shared by every fragment shader.
constexpr const char* kPrelude = R"GLSL(
in vec2 v_uv;
out vec4 o_color;
uniform mat4  u_invProj;   // clip -> view
uniform mat4  u_proj;      // view -> clip
uniform vec2  u_texel;     // 1 / full resolution
uniform float u_skyDepth;  // depth values at or above this are the sky

vec3 ViewPos(vec2 uv, float d)
{
    vec4 p = u_invProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

// Distance in front of the camera (positive), from a depth buffer value.
float LinearZ(float d)
{
    float z = min(d, 0.9999999) * 2.0 - 1.0;
    float vz = u_invProj[2][2] * z + u_invProj[3][2];
    float vw = u_invProj[2][3] * z + u_invProj[3][3];
    return -vz / vw;
}

vec2 Project(vec3 p)
{
    vec4 c = u_proj * vec4(p, 1.0);
    return c.xy / c.w * 0.5 + 0.5;
}

// Interleaved gradient noise: a cheap per-pixel rotation for sample patterns.
float Noise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
bool IsSky(float d) { return d >= u_skyDepth; }
bool OnScreen(vec2 uv) { return uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0; }

// Surface normal (view space) from the depth buffer: of the two neighbours on each axis, the
// one on the same surface (smaller depth step) is used, so silhouettes don't bend it.
vec3 NormalAt(sampler2D depth, vec2 uv, vec3 p)
{
    vec2 dx = vec2(u_texel.x, 0.0), dy = vec2(0.0, u_texel.y);
    vec3 pr = ViewPos(uv + dx, textureLod(depth, uv + dx, 0.0).r);
    vec3 pl = ViewPos(uv - dx, textureLod(depth, uv - dx, 0.0).r);
    vec3 pu = ViewPos(uv + dy, textureLod(depth, uv + dy, 0.0).r);
    vec3 pd = ViewPos(uv - dy, textureLod(depth, uv - dy, 0.0).r);
    vec3 ddx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    vec3 ddy = abs(pu.z - p.z) < abs(p.z - pd.z) ? pu - p : p - pd;
    return normalize(cross(ddx, ddy));
}
)GLSL";

// ---- Ambient occlusion + screen-space sun shadows (half resolution) -> RG ------------------
constexpr const char* kLighting = R"GLSL(
uniform sampler2D u_depth;
uniform vec3  u_sunView;      // toward the light, view space
uniform float u_aoOn;
uniform float u_aoRadius;     // m
uniform float u_shadowOn;
uniform float u_shadowLength; // m

float AmbientOcclusion(vec3 p, vec3 n, float noise)
{
    const int kSamples = 12;
    vec3 t = normalize(abs(n.y) < 0.99 ? cross(n, vec3(0.0, 1.0, 0.0)) : cross(n, vec3(1.0, 0.0, 0.0)));
    vec3 b = cross(n, t);
    float z = -p.z;
    float occlusion = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        float f = (float(i) + 0.5) / float(kSamples);
        float phi = float(i) * 2.39996323 + noise * 6.2831853;
        float r = sqrt(f);
        vec3 dir = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(1.0 - f);  // cosine-weighted
        float s = fract(float(i) * 0.618034 + noise);
        vec3 q = p + dir * (u_aoRadius * mix(0.12, 1.0, s * s));
        vec2 uv = Project(q);
        if (!OnScreen(uv))
            continue;
        float sceneZ = LinearZ(textureLod(u_depth, uv, 0.0).r);
        float range = smoothstep(0.0, 1.0, u_aoRadius / max(abs(z - sceneZ), 1e-4));
        occlusion += sceneZ < -q.z - (0.02 * u_aoRadius + 0.0015 * z) ? range : 0.0;
    }
    float ao = 1.0 - occlusion / float(kSamples);
    return mix(ao, 1.0, smoothstep(60.0, 120.0, z));
}

// 1 = lit, 0 = something on screen stands between this point and the sun.
float SunShadow(vec3 p, vec3 n, float noise)
{
    const int kSteps = 24;
    float z = -p.z;
    vec3 origin = p + n * (0.015 + 0.0015 * z);
    for (int i = 0; i < kSteps; ++i) {
        float f = (float(i) + noise) / float(kSteps);
        vec3 r = origin + u_sunView * (u_shadowLength * f * f + 0.02);
        if (r.z > -0.05)
            break;
        vec2 uv = Project(r);
        if (!OnScreen(uv))
            break;
        float sceneZ = LinearZ(textureLod(u_depth, uv, 0.0).r);
        float rayZ = -r.z;
        float behind = rayZ - sceneZ;  // > 0: the surface seen there is in front of the ray
        if (behind > 0.02 + 0.004 * rayZ && behind < 0.5 + 0.03 * rayZ) {
            vec2 e = min(uv, 1.0 - uv);
            float edge = smoothstep(0.0, 0.06, min(e.x, e.y));
            return 1.0 - edge * (1.0 - 0.5 * f * f);
        }
    }
    return 1.0;
}

void main()
{
    float d = textureLod(u_depth, v_uv, 0.0).r;
    if (IsSky(d)) {
        o_color = vec4(1.0);
        return;
    }
    vec3 p = ViewPos(v_uv, d);
    vec3 n = NormalAt(u_depth, v_uv, p);
    float noise = Noise(gl_FragCoord.xy);
    float ao = u_aoOn > 0.5 ? AmbientOcclusion(p, n, noise) : 1.0;
    float lit = 1.0;
    if (u_shadowOn > 0.5 && dot(n, u_sunView) > 0.05) {
        lit = SunShadow(p, n, noise);
        lit = mix(lit, 1.0, smoothstep(80.0, 150.0, -p.z));
    }
    o_color = vec4(ao, lit, 0.0, 1.0);
}
)GLSL";

// ---- Composite: dirt detail, AO, shadows, time of day (sky and grading) -> linear HDR -------
constexpr const char* kComposite = R"GLSL(
uniform sampler2D u_color;
uniform sampler2D u_depth;
uniform sampler2D u_terrainDepth;
uniform sampler2D u_light;      // half resolution: AO, sun
uniform sampler2D u_detail;     // tiling dirt detail: normal xy, height, albedo
uniform vec2  u_halfTexel;
uniform float u_depthOk;        // depth and projection known: depth-based effects possible
uniform float u_lightOn;
uniform float u_ao;
uniform float u_shadow;
uniform float u_dirt;
uniform float u_dirtScale;
uniform float u_haveTerrain;
uniform sampler2D u_markMap;    // tire marks on the ground (see postfx.cpp: TireMarks)
uniform float u_marks;          // 0 = off
uniform float u_markDepth;      // m at the deepest: what the map's groove values measure down
uniform vec2  u_markCam;        // the camera in the map's window, in texels
uniform vec2  u_markPhys;       // where the window's corner is in the (wrapping) texture
uniform vec3  u_markU, u_markV; // the map's axes, view space
uniform vec3  u_markUW, u_markVW; // and world space
uniform float u_markTexel;      // m
uniform float u_markSize;       // texels
uniform float u_markBlend;      // m: the fade before the window's edge (kMarkBlend; a few m when the
                                // window holds every rut ridden: postfx.cpp, kMarkFitMargin)
uniform float u_tireDirt;       // 0..1
uniform float u_metre;          // game units per metre (MX Simulator: feet)
uniform float u_wheelOn[2];     // 0 rear, 1 front
uniform mat4  u_wheelLocal[2];  // view space -> the wheel's own (axle along x, turning with it)
uniform float u_wheelRadius[2]; // m, to the outside of the tire
uniform sampler2D u_wheelMask;    // depth of your wheels as the game drew them (tire dirt goes only there)
uniform float u_wheelMaskOn;
uniform float u_wheelStretch[2];// 1 standing still; smaller the faster it turns (the mud blurs into rings)
uniform mat3  u_viewToWorld;
uniform vec3  u_camWrapped;     // camera position, wrapped to keep the numbers small
uniform vec3  u_up;
uniform vec3  u_lightWorld;     // toward the light the scene is lit by
uniform vec3  u_sunWorld;
uniform vec3  u_moonWorld;
uniform float u_tod;            // 1: the time of day grades the picture and paints the sky
uniform vec3  u_tint;
uniform float u_exposure;
uniform float u_desaturate;
uniform float u_day;
uniform float u_golden;
uniform float u_night;
uniform float u_haze;
uniform float u_nightLevel;  // how bright the night is (1 = default)
uniform float u_time;
uniform float u_pixelAngle;     // radians per pixel
uniform sampler2DShadow u_shadowMap;  // depth seen from the sun (shadow map)
uniform float u_shadowMapOn;
uniform mat4  u_viewToShadow[4];  // view space -> each cascade's clip space (the sun's)
uniform float u_shadowTexel;      // one texel of the whole map (the cascades are its 2 x 2 tiles)
uniform float u_shadowBias[4];
uniform float u_shadowTexelWorld[4];  // one texel of each cascade on the ground, game units
uniform float u_shadowSoft;     // 0..1: how soft detailed shadows' edges are (Config::shadowSoftness)
uniform vec3  u_lightView;      // toward the light, view space
uniform float u_headlight;      // 0 = off
uniform int   u_headCount;      // headlights: yours first, then the nearest other bikes'
uniform vec3  u_headPos[12];    // view space: the front plate
uniform vec3  u_headDir[12];    // view space: where the beam points
uniform vec3  u_headRight[12];  // view space: the beam's own right and up (across the bike)
uniform vec3  u_headUp[12];
uniform float u_headPower[12];  // each one's strength (0..1: fading in and out as bikes are found and lost)
uniform vec3  u_headBeam;       // radians: the beam's half-width, the middle of it below level, its half-height
uniform vec3  u_headColor;      // the light's colour (white or amber, Config::headlightAmber)
uniform float u_headReach;      // how far it reaches (1 = level; more aimed up, less aimed down)
uniform float u_snow;           // WEATHER: 0..1 snow on the ground and an overcast sky (0: no weather)
uniform float u_snowfall;       // 0..1 how heavily it falls: the haze in the distance
uniform vec3  u_snowFog;        // that haze's colour (as bright as the light of the time of day)

// 1 = lit, 0 = in the shadow of something the sun's view saw first. 3 x 3 hardware-filtered
// taps for soft edges; fades out at the edge of the map.
// One cascade's shadow at q (1 = lit), and how far inside it q is (0 at its usable edge, up to 1).
float CascadeShadow(int i, vec3 p, vec3 n, float onGround, out float inside)
{
    // Pushed off the surface before it's looked up. The ground: 0.04 game units and a little more with
    // distance, as it always was. Anything standing on it (a rider's gear, a bike): a texel and a half
    // of this cascade (the nearest one's are 2.3 cm in the game; 0.04 of its feet is 1.2 cm, half of
    // one, and a rider's curved gear shaded itself in blotches). Pushed further, thin shadows on the
    // ground - where it isn't known as ground - thinned out.
    // The ground: the same, but by how steeply the sun meets it - flat ground facing the sun keeps its
    // small push (thin shadows on it stay whole), a berm's face the sun only grazes gets the most:
    // pushed 1.2 cm, those faces shaded themselves in jagged stripes.
    bool object = onGround < 0.5;
    float facingSun = clamp(dot(n, u_lightView), 0.0, 1.0);
    float grazing = sqrt(1.0 - facingSun * facingSun);
    float push = max(0.04, 1.5 * u_shadowTexelWorld[i] * (object ? 1.0 : grazing * grazing));
    vec3 q = p + n * (push + 0.0015 * -p.z);
    vec4 s = u_viewToShadow[i] * vec4(q, 1.0);
    vec3 c = s.xyz / s.w * 0.5 + 0.5;
    vec2 e = min(c.xy, 1.0 - c.xy);
    inside = clamp((min(e.x, e.y) - 0.03) / 0.12, 0.0, 1.0);
    if (c.z >= 1.0 || inside <= 0.0)
        return 1.0;
    vec2 tile = vec2(float(i - (i / 2) * 2), float(i / 2)) * 0.5;  // each a quarter tile of the map
    vec2 uv = tile + c.xy * 0.5;
    // (Softer on what stands on the ground: up close a rider's gear spans ten pixels to one of the
    // map's texels, and its shadow on itself - an arm across the hoodie - came out in blocks.)
    // On the ground, softer the more the sun grazes it: a low sun stretches each texel into a long
    // strip across a berm's face, and its shadow's edge came out as a staircase. (The same nine taps.)
    // (How soft, by the softness setting: half way, a little crisper than they've been - the extra
    // on ground the sun grazes softened the riders' and bikes' shadows in a low sun as well.)
    float spread = object ? mix(1.0, 2.0, u_shadowSoft)
                          : mix(1.0, 1.5, u_shadowSoft) + 1.25 * u_shadowSoft * grazing * grazing;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(u_shadowMap, vec3(uv + vec2(float(x), float(y)) * u_shadowTexel * spread, c.z - u_shadowBias[i]));
    return sum / 9.0;
}

float MapShadow(vec3 p, vec3 n, float onGround)
{
    // The smallest (sharpest) cascade it's inside. Near that one's edge it blends into the next
    // one out, so there's no line where shadows suddenly turn softer (those lines go round with
    // the camera: they flickered). The biggest fades out at its edge.
    for (int i = 0; i < 4; ++i) {
        float inside;
        float lit = CascadeShadow(i, p, n, onGround, inside);
        if (inside <= 0.0)
            continue;
        if (inside >= 1.0)
            return lit;
        if (i == 3)
            return mix(1.0, lit, inside);
        float outerInside;
        float outer = CascadeShadow(i + 1, p, n, onGround, outerInside);
        return mix(mix(1.0, outer, outerInside), lit, smoothstep(0.0, 1.0, inside));
    }
    return 1.0;
}

// The headlight's light on a surface: a round, soft-edged cone seen from the plate, aimed down
// so its bottom edge lands just in front of the front wheel and its top edge reaches out into the
// distance. On the ground that makes an oval pool joined to the bike, brightest a few metres
// ahead (a real headlight throws most of its light near the top of the beam); surfaces facing
// the bike catch more of it. Nothing behind the plate or on the bike itself.
float HeadlightOne(vec3 p, vec3 n, int i)
{
    vec3 l = u_headPos[i] - p;
    float d = length(l);
    if (d > 70.0 * u_headReach)
        return 0.0;
    l /= max(d, 1e-4);
    vec3 q = -l * d;
    float ahead = dot(q, u_headDir[i]);
    if (ahead <= 0.0)
        return 0.0;
    float side = atan(dot(q, u_headRight[i]), max(ahead, 1e-4));
    float down = atan(-dot(q, u_headUp[i]), max(ahead, 1e-4));
    vec2 e = vec2(side / u_headBeam.x, (down - u_headBeam.y) / u_headBeam.z);
    float cone = ahead > 0.0 ? smoothstep(1.0, 0.45, length(e)) : 0.0;
    float hot = mix(0.3, 1.0, smoothstep(0.8, 0.12, down));
    float own = smoothstep(0.7, 1.3, d);
    float facing = mix(0.35, 1.0, clamp(dot(n, l) * 3.0, 0.0, 1.0)) * step(0.0, dot(n, l));
    float reach = (1.0 / (1.0 + d * d / (144.0 * u_headReach * u_headReach))) *
                  (1.0 - smoothstep(45.0 * u_headReach, 70.0 * u_headReach, d));
    return cone * hot * own * facing * reach;
}

vec3 Headlight(vec3 albedo, vec3 p, vec3 n)
{
    float sum = 0.0;
    for (int i = 0; i < u_headCount; ++i)
        sum += HeadlightOne(p, n, i) * u_headPower[i];
    return albedo * u_headColor * (sum * u_headlight * 2.6);
}

float Hash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec2 LightAt(vec2 uv, float z)
{
    vec2 sum = vec2(0.0);
    float total = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 o = vec2(float(x), float(y)) * u_halfTexel;
            float sz = LinearZ(textureLod(u_depth, uv + o, 0.0).r);
            float w = exp(-abs(sz - z) / (0.03 * z + 0.05)) * ((x == 0 && y == 0) ? 2.0 : 1.0);
            sum += textureLod(u_light, uv + o, 0.0).rg * w;
            total += w;
        }
    }
    return sum / max(total, 1e-4);
}

// Fine detail laid over the ground in world space (triplanar, two scales), lit by the sun.
vec3 Dirt(vec3 c, vec3 wp, vec3 dwx, vec3 dwy, vec3 n, float z, float mask)
{
    vec3 w = pow(abs(n), vec3(4.0));
    w /= (w.x + w.y + w.z);
    float fades[2];
    fades[0] = 1.0 - smoothstep(25.0, 60.0, z);
    fades[1] = 1.0 - smoothstep(7.0, 22.0, z);
    vec3 bump = vec3(0.0);
    vec2 tone = vec2(0.0);
    for (int k = 0; k < 2; ++k) {
        float s = (k == 0 ? 0.6 : 2.6) / u_dirtScale;
        vec4 tx = textureGrad(u_detail, wp.zy * s, dwx.zy * s, dwy.zy * s);
        vec4 ty = textureGrad(u_detail, wp.xz * s, dwx.xz * s, dwy.xz * s);
        vec4 tz = textureGrad(u_detail, wp.xy * s, dwx.xy * s, dwy.xy * s);
        vec2 gx = tx.rg * 2.0 - 1.0, gy = ty.rg * 2.0 - 1.0, gz = tz.rg * 2.0 - 1.0;
        float f = fades[k] * (k == 0 ? 0.7 : 1.0);
        bump += (vec3(0.0, gx.y, gx.x) * w.x + vec3(gy.x, 0.0, gy.y) * w.y + vec3(gz.x, gz.y, 0.0) * w.z) * f;
        vec4 t = tx * w.x + ty * w.y + tz * w.z;
        tone += (t.ba - 0.5) * f;
    }
    vec3 nd = normalize(n + bump * 1.3);
    float before = max(dot(n, u_lightWorld), 0.0) * 0.75 + 0.35;
    float after = max(dot(nd, u_lightWorld), 0.0) * 0.75 + 0.35;
    float shade = clamp(after / before, 0.6, 1.45);
    float cavity = 1.0 + tone.x * 0.5;
    float albedo = 1.0 + tone.y * 0.55;
    return mix(c, c * (shade * cavity * albedo), mask);
}

// The ground's own detail texel at wp (triplanar, the three faces weighted by the surface's
// own normal), as Dirt reads it: rg are the bump's xy, b its height and a its albedo. Only the
// blended texel comes back, which is what the marks want of it: their floor is the same dirt
// the surface has, so it has to be the same texel, just read closer.
vec4 DetailAt(vec3 wp, vec3 dwx, vec3 dwy, vec3 n, float s)
{
    vec3 w = pow(abs(n), vec3(4.0));
    w /= (w.x + w.y + w.z);
    vec4 tx = textureGrad(u_detail, wp.zy * s, dwx.zy * s, dwy.zy * s);
    vec4 ty = textureGrad(u_detail, wp.xz * s, dwx.xz * s, dwy.xz * s);
    vec4 tz = textureGrad(u_detail, wp.xy * s, dwx.xy * s, dwy.xy * s);
    return tx * w.x + ty * w.y + tz * w.z;
}

float ValueNoise(vec3 p)
{
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(Hash13(i), Hash13(i + vec3(1, 0, 0)), f.x);
    float b = mix(Hash13(i + vec3(0, 1, 0)), Hash13(i + vec3(1, 1, 0)), f.x);
    float c = mix(Hash13(i + vec3(0, 0, 1)), Hash13(i + vec3(1, 0, 1)), f.x);
    float d = mix(Hash13(i + vec3(0, 1, 1)), Hash13(i + vec3(1, 1, 1)), f.x);
    return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}
)GLSL" R"GLSL(
// How far inside the map's window this is (m), and how much of the map is used there: the
// ruts hold on until the window's edge and fade out over the last kMarkBlend metres before
// it. What the window's wrap-around cleared is not lost: postfx.cpp keeps the ruts you
// pressed in a cache and re-presses them into the fresh strip the window lets in, with the
// very same stamps a rut gets when it's first dug - so a mark never ends on a line where
// ground is reused, and last lap's ruts come back whole after a U-turn.
float MarkBlend() { return max(u_markBlend, 1.0); }

float MarkNear(vec2 t) { return texture(u_markMap, (t + u_markPhys) / u_markSize).r; }

// How much groove is at t, and the fullest of the four texels two each way of it (the rut's
// edges are where a neighbour holds a groove and this one little).
vec2 MarkStatsNear(vec2 t)
{
    return vec2(MarkNear(t), max(max(MarkNear(t + vec2(2.0, 0.0)), MarkNear(t - vec2(2.0, 0.0))),
                               max(MarkNear(t + vec2(0.0, 2.0)), MarkNear(t - vec2(0.0, 2.0)))));
}

// The height field's own slope at t, over texels two each way along the map's axes: the dug
// walls. Two texels rather than one, because the map holds 8-bit steps and a nearer stencil
// reads those steps as ridges in the sides.
vec2 MarkGrad(vec2 t)
{
    return vec2(MarkNear(t + vec2(2.0, 0.0)) - MarkNear(t - vec2(2.0, 0.0)),
                MarkNear(t + vec2(0.0, 2.0)) - MarkNear(t - vec2(0.0, 2.0)));
}

// Ruts your tires dug into the ground: a channel of darker, packed and damp dirt, deepest on
// the tire's own line and easing out to the undisturbed ground on either side, its walls lit
// by the sun like any slope, and loose dirt thrown up along both edges. The ruts sink the
// ground itself: GrooveDepth below says how far this pixel's ray reaches down into a groove,
// and main pulls the whole look of the surface (the scene's own colour, the ground's detail,
// and these marks) from that lower point - the whole ground texture moves with the groove,
// not just the marks. The map only covers the window around you: its own ruts, and whatever
// the cache re-pressed into ground the window reused, and what's left thins out over the last
// kMarkBlend metres before the window's edge instead of ending on a line.
// The look the rut has of the ground it lies in comes from main: n is the surface's own normal
// where the groove's floor is (not this pixel's, so a rut across a hillside is lit against the
// ground it really sits on), and lFloor / lHere are the scene's own ambient and sun where that
// floor was drawn and where this pixel is - their ratio is what carries the rut into the same
// shade and light the ground around it stands in. relief is how deep this pixel's groove can
// be at all, as the displacement actually left it: the walls' shading has to lean on the same
// height the floor was lowered by, or the sides read steeper than the ground they hold.
vec3 TireMarks(vec3 c, vec3 p, vec3 wp, vec3 dwx, vec3 dwy, vec3 n, vec2 lFloor, vec2 lHere,
               float relief)
{
    vec2 g = vec2(dot(p, u_markU), dot(p, u_markV));  // this pixel on the ground's plane, game units
    vec2 t = u_markCam + g / u_markTexel;             // in the map's texels
    // How far inside the window this is (m), and how much of the map counts there: the last
    // kMarkBlend metres before its edge fade out.
    float inNear = min(min(t.x, u_markSize - t.x), min(t.y, u_markSize - t.y)) * (u_markTexel / u_metre);
    float wNear = smoothstep(0.0, MarkBlend(), inNear);
    if (wNear < 0.004)
        return c;
    vec2 sNear = MarkStatsNear(t);
    float m = sNear.x * wNear, around = max(sNear.y * wNear, m);
    if (m < 0.004 && around < 0.004)
        return c;
    float gx = (MarkNear(t + vec2(1.0, 0.0)) - MarkNear(t - vec2(1.0, 0.0))) * wNear;
    float gy = (MarkNear(t + vec2(0.0, 1.0)) - MarkNear(t - vec2(0.0, 1.0))) * wNear;
    float texel = u_markTexel;
    float k = u_marks;
    // The sides, from the height field's own slope two texels each way and over the height the
    // floor was really lowered here: the dug walls light as real slopes, and their shading
    // lines up with where main lowered this pixel.
    vec2 gr = MarkGrad(t);
    vec3 nd = normalize(n + (u_markUW * gr.x + u_markVW * gr.y) *
                            (wNear * relief / (4.0 * texel)));
    float before = max(dot(n, u_lightWorld), 0.0) * 0.75 + 0.35;
    float after = max(dot(nd, u_lightWorld), 0.0) * 0.75 + 0.35;
    float shade = clamp(after / before, 0.45, 1.6);
    // The floor's own dirt: the ground's detail texture, read finer than the ground wears it,
    // so the packed floor is the same dirt the surface has, pressed close - and it agrees with
    // the ground around it in grain and colour by construction.
    vec4 tex = vec4(0.5);
    if (u_dirt > 0.0)
        tex = DetailAt(wp, dwx, dwy, n, 5.2 / u_dirtScale);
    // The dirt the tire left is this ground's own dirt, pressed: only the things pressed dirt
    // does to a colour are done to it - a little less varied, a touch warmer, and darker. Not
    // one of these is a colour of its own, so the track's own texture comes through and the
    // rut reads as the surface read through dirt rather than as dirt painted on it.
    float depth = min(m * k, 1.0);
    float l = max(Luma(c), 1e-3);
    vec3 col = mix(c, vec3(l), 0.22 * depth);  // packed earth varies less than the surface
    col *= vec3(1.04, 1.0, 0.94);              // a breath of warmth, the hue kept
    col *= mix(1.0, 0.62, depth);              // and darker: the press, not a paint
    // The ground already wears the detail texture, so the floor wears it again only lightly:
    // its grain then reads as the same dirt pressed close, not as a second layer of it.
    col *= 1.0 + (tex.a - 0.5) * 0.25 * depth;
    col *= 1.0 + (tex.b - 0.5) * 0.3 * depth;
    // Groove floors keep down out of the light: darker where the map's height steps up to a
    // lip around them, and up the walls, where the ground climbs back out of the channel.
    float aoGroove = clamp(sNear.y * wNear - m, 0.0, 1.0);
    // And lit as the ground that holds it is lit: where its floor was drawn in the sun and this
    // pixel stands in a shadow (or the other way round), the ratio carries the rut across, the
    // way the composite itself weighs ao and sun below.
    float aoR = clamp(lFloor.x / max(lHere.x, 1e-3), 0.35, 2.0);
    float litR = clamp(mix(0.42, 1.0, lFloor.y) / max(mix(0.42, 1.0, lHere.y), 1e-3), 0.35, 2.2);
    float relight = mix(1.0, aoR, u_ao) * mix(1.0, litR, u_shadow);
    col *= shade * relight * (1.0 - 0.3 * k * aoGroove);
    // Loose dirt thrown up along the lips: this ground's own colour, broken up and catching a
    // little more of the light. Mixed in rather than added to, so the lips don't leave a pale
    // ring on pale ground or a dark one on dark. Kept to the rut's shallow flanks: the walls,
    // where the dirt really was pushed aside, not down the floor of the channel.
    float edge = clamp(around - m, 0.0, 1.0) * k * smoothstep(0.25, 0.1, m);
    float clods = smoothstep(0.35, 0.75, ValueNoise(wp * (30.0 / u_metre)) * 0.6 + Hash13(floor(wp * (40.0 / u_metre))) * 0.4);
    col = mix(col, c * (0.92 + 0.24 * clods) * relight, edge * (0.3 + 0.45 * clods));
    return col;
}

)GLSL" R"GLSL(
// Whether the game drew bare ground at this pixel, nothing standing on it: not your bike, not your
// boots (the dug grooves borrow the ground's look from a little way off, and borrowing your tire there
// put a copy of it in the rut). Its depth where the ground was expected (zExpect, when asked: < 0 not),
// the ground the terrain left there before anything stood on it, to the millimetre, and not one of your
// own wheels as the game drew them. And, asked with nExpect (view space; zero: not), facing the way
// the ground here faces: where a tire or a boot touches the ground it's within a centimetre of it,
// and the terrain's own depth can have your bike in it (drawn before the terrain pass ended), but the
// bottom of a tire curves away underneath and a boot's side stands up - neither faces like the ground.
bool BareGroundAt(vec2 uv, float zExpect, vec3 nExpect)
{
    float d = textureLod(u_depth, uv, 0.0).r;
    if (IsSky(d))
        return false;
    float z = LinearZ(d);
    if (zExpect > 0.0 && abs(z - zExpect) > 0.01 * zExpect + 0.03 * u_metre)
        return false;
    if (dot(nExpect, nExpect) > 0.5 && dot(NormalAt(u_depth, uv, ViewPos(uv, d)), nExpect) < 0.9)
        return false;
    if (u_haveTerrain > 0.5 && abs(LinearZ(textureLod(u_terrainDepth, uv, 0.0).r) - z) > 0.002 * z + 0.004)
        return false;
    if (u_wheelMaskOn > 0.5 && abs(LinearZ(textureLod(u_wheelMask, uv, 0.0).r) - z) < 0.006 * z + 0.02)
        return false;
    return true;
}

// The rut your tires dug here: how much groove (x) and the fullest groove beside it (y), as
// TireMarks reads them (0..1, with the marks' strength). Where y is well above x is a rut's lip.
vec2 RutStatsAt(vec3 p)
{
    vec2 t = u_markCam + vec2(dot(p, u_markU), dot(p, u_markV)) / u_markTexel;
    float inNear = min(min(t.x, u_markSize - t.x), min(t.y, u_markSize - t.y)) * (u_markTexel / u_metre);
    return MarkStatsNear(t) * (smoothstep(0.0, MarkBlend(), inNear) * u_marks);
}

// The snow's own relief, as a normal to light it by: ripples the wind laid across it (about every
// 35 cm, wavering), soft lumps a couple of metres across and its fine grain (the detail texture, read
// flat on the ground at two sizes). Faint, and gone into the distance, where it would only shimmer.
vec3 SnowNormal(vec3 n, vec3 wp, vec3 dwx, vec3 dwy, float z)
{
    vec3 ax = normalize(cross(u_up, vec3(0.0, 0.0, 1.0)));
    vec3 az = cross(ax, u_up);
    vec2 g = vec2(dot(wp, ax), dot(wp, az)) / u_metre;  // metres across the ground
    vec2 gx = vec2(dot(dwx, ax), dot(dwx, az)) / u_metre, gy = vec2(dot(dwy, ax), dot(dwy, az)) / u_metre;
    float fade = 1.0 - smoothstep(15.0, 50.0, z / u_metre);
    if (fade <= 0.0)
        return n;
    vec2 across = vec2(0.8, 0.6);
    float warp = ValueNoise(vec3(g * 0.45, 0.0)) * 6.0;
    float strength = 0.4 + 0.6 * ValueNoise(vec3(g * 0.18, 3.0));
    vec2 slope = across * (cos(dot(g, across) * 18.0 + warp) * 0.05 * strength);
    vec2 lumps = textureGrad(u_detail, g * 0.4, gx * 0.4, gy * 0.4).rg * 2.0 - 1.0;
    vec2 grain = textureGrad(u_detail, g * 3.1, gx * 3.1, gy * 3.1).rg * 2.0 - 1.0;
    slope += lumps * 0.16 + grain * 0.06 * (1.0 - smoothstep(4.0, 12.0, z / u_metre));
    slope *= fade;
    return normalize(n - ax * slope.x - az * slope.y);
}

// Snow on the ground (WEATHER): white, a little blue in it, thinner on steep slopes and in drifts
// that come and go over the ground, its own ripples and lumps lit by the sun (the faces turned from
// it a little bluer: the sky lights them). The ground's own shading (its baked light: the shadows of
// trees) carries over gently. A rut through it is laid out by how deep it was dug: packed snow where
// a tire only pressed it, grey slush deeper, where snow and dirt were churned together, and the wet
// dirt itself at the bottom; along its edges, a lip of the snow it pushed aside. cover: how much of
// this is snow (for the cold blue of its shadows, after the lighting).
vec3 Snow(vec3 c, vec3 wp, vec3 dwx, vec3 dwy, vec3 n, float z, float ground, vec2 rut, out float cover)
{
    float upright = smoothstep(0.35, 0.8, dot(n, u_up));
    float drifts = ValueNoise(wp * (0.3 / u_metre)) * 0.65 + ValueNoise(wp * (1.6 / u_metre)) * 0.35;
    float depth = u_snow * (0.45 + 0.55 * upright) * (0.7 + 0.6 * drifts);
    float settled = smoothstep(0.25, 0.6, depth);
    // The game's own light on the ground, gently (its texture's blotches mostly left out).
    float shading = clamp(pow(max(Luma(c), 1e-4) / 0.2, 0.3), 0.65, 1.2);
    // The snow's own relief, lit as the ground is lit (against the ground's plain slope).
    vec3 ns = SnowNormal(n, wp, dwx, dwy, z);
    float before = max(dot(n, u_lightWorld), 0.0) * 0.7 + 0.4;
    float after = max(dot(ns, u_lightWorld), 0.0) * 0.7 + 0.4;
    float relief = clamp(after / before, 0.72, 1.28);
    vec3 fresh = vec3(0.80, 0.84, 0.90) * (0.94 + 0.06 * ValueNoise(wp * (5.0 / u_metre)));
    fresh *= mix(vec3(0.9, 0.95, 1.07), vec3(1.0), smoothstep(0.8, 1.05, relief));  // turned away: bluer
    vec3 snow = fresh * shading * relief;  // (no glints: grains a pixel wide or more read as white dashes, and shimmer)
    // The rut, by how deep it is.
    float m = rut.x, lip = clamp(rut.y - rut.x, 0.0, 1.0);
    float pressed = smoothstep(0.03, 0.2, m), slushy = smoothstep(0.15, 0.5, m), bare = smoothstep(0.45, 0.85, m);
    vec3 wet = c * mix(1.0, 0.62, u_snow);  // the dirt under it, damp
    vec3 slush = mix(vec3(Luma(snow)) * vec3(0.95, 0.96, 0.99) * 0.55, wet, 0.45);
    vec3 col = mix(snow, snow * vec3(0.88, 0.89, 0.9), pressed);  // pressed: duller, a little darker
    col = mix(col, slush, slushy);
    col = mix(col, wet, bare * 0.85);
    col = mix(col, snow * 1.06, lip * (1.0 - slushy) * 0.8);  // the snow pushed aside, fresh and lit
    cover = settled * (1.0 - slushy) * ground;
    return mix(c, mix(wet, col, settled), ground);
}

// How far (in game units) this pixel's ray has to go down along the ray to reach the groove's
// surface, 0 where there's none: the map holds how deep each 4 cm texel is pressed, and the
// ray is marched against those heights, down from the deepest stop to the surface - where the
// groove there is deeper than this stop's height, the ray has reached the dirt. main takes the
// whole look of the surface from that lower point, which is what cuts the grooves into the
// ground rather than laying them on it.
float GrooveDepth(vec3 p, vec3 n, vec3 dirW)
{
    float texel = u_markTexel;
    // The groove's full depth, kept under eight texels (32 cm): marching further than that would
    // slide the surface far outside the groove and carry it with the camera.
    float dep = min(u_markDepth * u_metre * u_marks, 8.0 * texel);
    float vdZ = dot(dirW, n);  // < 0: this pixel's ray goes into the ground; a view shallower
                               // than this keeps its own shift growing, so the depth eases out
    if (dep <= 0.5 * texel || vdZ > -0.12)
        return 0.0;
    dep *= smoothstep(-0.12, -0.26, vdZ);  // eases out rather than cutting: a hard cut would
                                          // make whole strips of the rut appear and vanish at
                                          // once as you walk round it
    if (dep <= 0.5 * texel)
        return 0.0;
    vec2 t = u_markCam + vec2(dot(p, u_markU), dot(p, u_markV)) / texel;
    // How far inside the window this is: the depth fades out over the last kMarkBlend metres
    // with the map, so the floor can't slide past the fade into the strip the window cleared.
    float inNear = min(min(t.x, u_markSize - t.x), min(t.y, u_markSize - t.y)) * (texel / u_metre);
    float wNear = smoothstep(0.0, MarkBlend(), inNear);
    if (wNear < 0.004)
        return 0.0;
    vec2 sNear = MarkStatsNear(t);
    float m = sNear.x * wNear, around = max(sNear.y * wNear, m);
    if (m < 0.004 && around < 0.004)
        return 0.0;
    // Where the floor sits at full depth: texels along the ray's own course over the map.
    vec2 shift = vec2(dot(dirW, u_markUW), dot(dirW, u_markVW)) * (dep / max(-vdZ, 1e-3)) / texel;
    shift *= wNear;
    // Down from the deepest stop to the ground itself: the first stop under whose height the
    // ray's own height there has fallen is where the ray has reached the dirt. The crossing is
    // then weighed from that stop and the deeper one it missed, so the floor is put where the
    // two say rather than at a stop: no steps in the depth, and no jitter to grow into noise
    // the deeper the groove is. The last stop is the ground's own level, which the depth of a
    // shallow pixel is measured from.
    float sPrev = 1.0, gPrev = 0.0;
    for (int i = 0; i <= 5; ++i) {
        float s = float(5 - i) / 5.0;
        // The map's own press never goes above 1.0, so this only keeps a texel that did
        // (strength from the panel rounding up) from asking for a floor below the march.
        float g = min(MarkNear(t + shift * s) * wNear, 1.0) - s;  // >= 0: the ray is in the dirt
        if (g >= 0.0) {
            if (i == 0)
                return dep;  // the ray is already under the dirt at the groove's full depth
            return clamp(sPrev - gPrev * (s - sPrev) / max(g - gPrev, 1e-4), 0.0, 1.0) * dep;
        }
        sPrev = s;
        gPrev = g;
    }
    return 0.0;
}

// Mud on your tires: clumps caked on the tread and the sidewalls that turn with the wheel, a
// little dust on the rim and hub. Lit by the sun from the surface's own normal.
vec3 TireDirt(vec3 c, vec3 p, vec3 nView, float ground)
{
    float m = u_metre;
    for (int i = 0; i < 2; ++i) {
        if (u_wheelOn[i] < 0.5)
            continue;
        vec3 q = (u_wheelLocal[i] * vec4(p, 1.0)).xyz;
        float r = length(q.yz), a = abs(q.x), R = u_wheelRadius[i];
        // Only pixels of your wheel get here (the mask), so where the tire is isn't guessed from how
        // far it is from the axle (that guess, a few cm off, put the mud on the rim and spokes): the
        // tire is the dark rubber. From the rim (about 0.6 of the wheel's radius) out to a little past
        // the tire (in case its size or the axle is a bit off).
        // (From the rim out: the spokes in the bike's shade are dark too.)
        if (r < 0.58 * R || r > 1.25 * R || a > 0.25 * m)
            continue;
        float radial = smoothstep(0.58 * R, 0.66 * R, r) * (1.0 - smoothstep(1.15 * R, 1.25 * R, r));
        float rubber = 1.0 - smoothstep(0.08, 0.2, Luma(c));  // c is linear here
        float tire = radial * rubber;
        float hub = radial * (1.0 - rubber);                  // the rim, spokes and hub: a dusting
        float tread = smoothstep(0.75 * R, 0.95 * R, r);
        float edge0 = 0.7 - 0.7 * u_tireDirt - 0.2 * tread;
        // The pattern turns with the tire. Turning fast, a frame apart it would be a different one
        // (it flickered), so it's blended into how much mud there is all the way round at that
        // distance from the axle, 8 places round averaged: what a spinning tire looks like. Never
        // all or nothing, and steady whatever the speed.
        float spin = 1.0 - u_wheelStretch[i];  // 0 standing still, towards 1 turning fast
        vec3 cp = vec3(atan(q.z, q.y) * R, q.x, r) * (26.0 / m);
        float n = ValueNoise(cp) * 0.6 + ValueNoise(cp * 2.4 + 7.0) * 0.4;
        float still = smoothstep(edge0, edge0 + 0.1, n), around = 0.0, hubRound = 0.0;
        if (spin > 0.01) {
            for (int k = 0; k < 8; ++k) {
                vec3 ck = vec3(float(k) * (6.2831853 * R / 8.0) * (26.0 / m) + 0.37, cp.y, cp.z);
                float nk = ValueNoise(ck) * 0.6 + ValueNoise(ck * 2.4 + 7.0) * 0.4;
                around += smoothstep(edge0, edge0 + 0.1, nk) * 0.125;
                hubRound += smoothstep(0.45, 0.7, nk) * 0.125;
            }
        }
        float cover = tire * mix(still, around, spin);
        cover = max(cover, hub * 0.35 * u_tireDirt * mix(smoothstep(0.45, 0.7, n), hubRound, spin));
        vec3 mud = mix(vec3(0.15, 0.095, 0.055), vec3(0.34, 0.23, 0.14), ValueNoise(vec3(cp.x * (1.0 - spin), cp.yz) * 0.45 + 3.0));
        float lit = 0.45 + 0.8 * max(dot(nView, u_lightView), 0.0);
        c = mix(c, mud * lit, cover);
    }
    return c;
}
)GLSL" R"GLSL(
vec3 Stars(vec3 d)
{
    vec3 a = abs(d);
    vec2 uv;
    float face;
    if (a.x >= a.y && a.x >= a.z) { uv = d.yz / a.x; face = d.x > 0.0 ? 0.0 : 1.0; }
    else if (a.y >= a.z)          { uv = d.xz / a.y; face = d.y > 0.0 ? 2.0 : 3.0; }
    else                          { uv = d.xy / a.z; face = d.z > 0.0 ? 4.0 : 5.0; }
    // About 1 cell in 70 holds a star: a sparse sky, mostly faint ones and a few bright.
    const float kCells = 110.0;  // per half face
    vec2 g = uv * kCells;
    vec2 cell = floor(g);
    float h = Hash13(vec3(cell, face));
    if (h < 0.986)
        return vec3(0.0);
    vec2 at = vec2(Hash13(vec3(cell, face + 7.0)), Hash13(vec3(cell, face + 13.0))) * 0.6 + 0.2;
    vec2 off = (fract(g) - at) / max(u_pixelAngle * kCells, 1e-5);  // in pixels
    float mag = fract(h * 37.0);
    float bright = 0.12 + 0.9 * mag * mag * mag;
    float b = exp(-dot(off, off) * 3.2);  // a point about a pixel wide
    float twinkle = 0.8 + 0.2 * sin(u_time * (1.5 + 3.0 * fract(h * 91.0)) + h * 60.0);
    vec3 colour = mix(vec3(0.75, 0.82, 1.0), vec3(1.0, 0.9, 0.75), fract(h * 13.0));
    return colour * (b * bright * twinkle);
}

// The night sky's own colour: a little lighter toward the horizon.
vec3 NightBase(float h)
{
    return mix(vec3(0.0040, 0.0060, 0.0130), vec3(0.0007, 0.0011, 0.0032), sqrt(max(h, 0.0))) * u_nightLevel;
}

vec3 NightSky(vec3 d)
{
    float h = dot(d, u_up);
    vec3 col = NightBase(h);
    col += Stars(d) * smoothstep(0.0, 0.2, h);
    float m = dot(d, u_moonWorld);
    col += vec3(0.85, 0.88, 0.95) * smoothstep(0.99955, 0.99975, m) * 1.5;
    col += vec3(0.25, 0.30, 0.45) * (pow(max(m, 0.0), 300.0) * 0.12 + pow(max(m, 0.0), 16.0) * 0.012) * u_nightLevel;
    return col;
}

// The colour the distance fades into at dusk and at night, looking along d.
vec3 HazeColour(vec3 d)
{
    float toward = max(dot(d, u_sunWorld), 0.0);
    vec3 dusk = mix(vec3(0.45, 0.32, 0.30), vec3(0.95, 0.55, 0.30), u_golden * pow(toward, 3.0)) * u_exposure;
    return mix(dusk, NightBase(0.0), u_night);
}

vec3 Sky(vec3 game, vec3 d)
{
    float h = dot(d, u_up);
    float toward = max(dot(d, u_sunWorld), 0.0);
    vec3 base = game * u_exposure * mix(vec3(1.0), vec3(0.7, 0.75, 0.95), 1.0 - u_day);
    float band = 1.0 - smoothstep(-0.05, 0.5, h);
    vec3 glow = mix(vec3(0.95, 0.38, 0.12), vec3(1.0, 0.72, 0.38), pow(toward, 8.0));
    float glowAmount = clamp(u_golden * band * (0.35 + 1.1 * pow(toward, 4.0)), 0.0, 0.9);
    vec3 dusk = mix(base, glow * (0.3 + 0.9 * pow(toward, 3.0)), glowAmount);
    dusk += vec3(1.0, 0.85, 0.6) * pow(toward, 900.0) * 3.0 * u_golden;  // the low sun's disc
    return mix(dusk, NightSky(d), u_night);
}

void main()
{
    vec3 c = pow(max(texture(u_color, v_uv).rgb, vec3(0.0)), vec3(2.2));
    float d = textureLod(u_depth, v_uv, 0.0).r;
    bool sky = u_depthOk > 0.5 && IsSky(d);

    // In uniform control flow, for the detail texture's mip selection.
    vec3 p = ViewPos(v_uv, min(d, 0.99999));
    vec3 wp = u_camWrapped + u_viewToWorld * p;
    vec3 dwx = dFdx(wp), dwy = dFdy(wp);
    vec3 dir = u_viewToWorld * normalize(ViewPos(v_uv, 0.5));

    if (sky) {
        if (u_tod > 0.5)
            c = Sky(c, dir);
        if (u_snow > 0.0) {
            // Overcast: the blue goes grey, the clouds stay a little lighter, the sun is hidden.
            float l = Luma(c);
            vec3 overcast = vec3(l) * vec3(0.93, 0.97, 1.04) * 0.92 + u_snowFog * 0.08;
            c = mix(c, overcast, 0.85 * u_snow);
            c = mix(c, u_snowFog, 0.35 * u_snowfall * u_snow * (1.0 - smoothstep(0.0, 0.35, dot(dir, u_up))));
        }
    } else {
        float z = -p.z;
        vec3 albedo = c;  // what the headlight lights: the surface before the sun's shadows
        vec3 nView = vec3(0.0, 0.0, 1.0);
        if (u_depthOk > 0.5) {
            nView = NormalAt(u_depth, v_uv, p);
            float ground = 0.0;
            if (u_haveTerrain > 0.5) {
                float tz = LinearZ(textureLod(u_terrainDepth, v_uv, 0.0).r);
                ground = 1.0 - smoothstep(0.002 * z + 0.004, 0.006 * z + 0.02, abs(tz - z));
            }
            vec3 nWorld = normalize(u_viewToWorld * nView);
            // The scene's own ambient and sun here, gathered once: the rut's lighting below is
            // asked for at the same place the game's picture was, and the two are then weighed
            // against each other and against these the way the composite weighs them at the end.
            vec2 lHere = vec2(1.0);
            if (u_lightOn > 0.5)
                lHere = LightAt(v_uv, z);
            float snowCover = 0.0;  // (WEATHER: how much of this is snow, for its shadows' cold blue)
            // Grooves sink the ground itself: where this pixel's ray reaches down into one, the
            // whole look of the surface (the scene's own colour, its detail, the marks) comes
            // from the groove floor the ray sees through this pixel - a point gd below the
            // ground's level, gd / -vdZ further down the ray. That point itself projects back
            // to this very pixel (it's on the ray), so the game's picture is asked at the point
            // of the ground's level right above it, which sits off to the side on screen: the
            // ground's own texture moves with the groove. The floor's own normal and light are
            // asked there too, so a rut across a hillside or into a shadow is lit as the ground
            // it really sits on.
            vec3 pD = p, wpD = wp, dwxD = dwx, dwyD = dwy;
            vec3 nFloor = nWorld;
            vec2 lFloor = lHere;  // unknown until the floor's own point is asked for: kept equal
                                  // so the rut's own relighting is neutral where it can't be had
            // The relief the marks can reach for this pixel's own view: what the slider asks for,
            // eased out where the ray comes in so shallow that its shift grows faster than it can
            // buy any more floor. GrooveDepth eases exactly this way, so the height the shading
            // below weighs the walls against is the height actually dug here.
            float relief = 0.0;
            if (u_marks > 0.0 && ground > 0.5) {
                float vdZ = dot(dir, nWorld);
                relief = min(u_markDepth * u_metre * u_marks, 8.0 * u_markTexel) *
                         smoothstep(-0.12, -0.26, vdZ);
                float gd = GrooveDepth(p, nWorld, dir);
                if (gd > 0.0 && vdZ < -0.12) {
                    float along = gd / max(-vdZ, 1e-3);
                    vec3 pF = p + transpose(u_viewToWorld) * (dir * along);  // the floor point, view space
                    vec2 suv = Project(pF + nView * gd);  // above it, back at the ground's level: where the game drew it
                    if (OnScreen(suv)) {
                        // One pixel exactly (no filtering: blended with the next one, the edge of a
                        // tire leaked in), and only bare ground: there and in the four pixels around
                        // it, all or nothing. Taken from your tire or a boot instead, the rut showed a
                        // copy of your bike beside the bike - a second tire, a second exhaust, smeared
                        // gear - and, kept soft, a faint one where they touch the ground.
                        suv = (floor(suv / u_texel) + 0.5) * u_texel;
                        float dS = textureLod(u_depth, suv, 0.0).r;
                        vec3 cF = pow(max(textureLod(u_color, suv, 0.0).rgb, vec3(0.0)), vec3(2.2));
                        float zE = -(pF + nView * gd).z;
                        bool bare = BareGroundAt(suv, zE, nView) &&
                                    BareGroundAt(suv + vec2(u_texel.x, 0.0), -1.0, vec3(0.0)) &&
                                    BareGroundAt(suv - vec2(u_texel.x, 0.0), -1.0, vec3(0.0)) &&
                                    BareGroundAt(suv + vec2(0.0, u_texel.y), -1.0, vec3(0.0)) &&
                                    BareGroundAt(suv - vec2(0.0, u_texel.y), -1.0, vec3(0.0));
                        float onGround = bare ? 1.0 : 0.0;
                        // The lip's last part of the way takes a little of each: the depth
                        // there is a fraction of what this groove can reach at all, so the
                        // band stays a band whatever the depth slider says - taking all of
                        // one or none of the other makes that edge flip as you move.
                        float wd = smoothstep(0.05 * relief, 0.3 * relief, gd) * onGround;
                        c = mix(c, cF, wd);
                        if (gd > 0.5 * u_markTexel && onGround > 0.5) {
                            pD = pF;
                            wpD = wp + dir * along;
                            dwxD = dFdx(wpD);
                            dwyD = dFdy(wpD);
                        }
                        if (onGround > 0.5) {
                            vec3 pv = ViewPos(suv, dS);  // the floor's own view point: its normal and its light
                            nFloor = NormalAt(u_depth, suv, pv);
                            if (u_lightOn > 0.5)
                                lFloor = LightAt(suv, -pv.z);
                        }
                    }
                }
            }
            if (u_dirt > 0.0) {
                float mask = ground * u_dirt * (1.0 - smoothstep(45.0, 60.0, z));
                if (mask > 0.001)
                    c = Dirt(c, wpD, dwxD, dwyD, nFloor, z, mask);
            }
            if (u_marks > 0.0 && ground > 0.5)
                c = TireMarks(c, pD, wpD, dwxD, dwyD, nFloor, lFloor, lHere, relief);
            // (Snow on what this pixel shows of the ground: where a groove was dug, its floor - so a rut
            // that is dug into the ground also shows the dirt through the snow.)
            if (u_snow > 0.0 && ground > 0.001)
                c = Snow(c, wpD, dwx, dwy, nFloor, z, ground, u_marks > 0.0 ? RutStatsAt(pD) : vec2(0.0), snowCover);
            // (Not tied to the ground mask: when your bike is drawn before the terrain pass ends, the
            // tire is in the terrain's depth too and counted as ground, and its mud came and went.
            // TireDirt keeps to the tire around its own axle.)
            // (Tire dirt: drawn by the bike shader itself on your tires, see proc_hooks PaintTireDirt.)
            float ao = lHere.x, lit = lHere.y;  // gathered above, beside the groove itself
            // On what stands on the ground, eased in as the surface turns to the sun rather than cut at
            // facing it: the normal read from the depth is noisy on small curved things (a rider's gear),
            // and pixel by pixel it flipped in and out of the map's shadow - pixelated patches.
            float towards = dot(nView, u_lightView);
            if (u_shadowMapOn > 0.5) {
                if (ground > 0.5) {
                    // The ground: its own light is baked in by the game, not turned to this sun, so
                    // ground turned away from the sun is put in shade here, eased in as it turns edge-on.
                    // (Cut at edge-on, as this was, a berm face edge-on to a low sun behind a wall had
                    // its noisy normal flip pixel by pixel between the wall's shadow and full sun: a
                    // speckled patch where the shadow should have been solid.)
                    float sunSide = smoothstep(-0.02, 0.12, towards);
                    float shade = sunSide > 0.0 ? MapShadow(p, nView, ground) : 0.0;
                    lit = min(lit, sunSide * shade);
                } else {
                    float facing = smoothstep(0.0, 0.2, towards);
                    if (facing > 0.0)
                        lit = min(lit, mix(1.0, MapShadow(p, nView, ground), facing));
                }
            }
            ao = mix(1.0, ao, u_ao);
            albedo = c * ao;
            // (Overcast in the snow: the sun's shadows fade, the light comes from the whole sky.)
            c *= ao * mix(1.0, mix(0.42, 1.0, lit), u_shadow * (1.0 - 0.65 * u_snow));
            // Snow in shade (the sun's shadow, a crease) is lit by the sky alone: cold, blue-grey.
            if (snowCover > 0.0)
                c *= mix(vec3(1.0), vec3(0.84, 0.92, 1.1),
                         snowCover * max((1.0 - lit) * u_shadow * (1.0 - 0.5 * u_snow), 1.0 - ao));
        }
        if (u_tod > 0.5) {
            c *= u_tint * u_exposure;
            c = mix(c, vec3(Luma(c)), u_desaturate);
            if (u_depthOk > 0.5)
                c = mix(c, HazeColour(dir), (1.0 - exp(-z / 350.0)) * u_haze);
        }
        if (u_snow > 0.0) {
            // A cold, grey day: the colours fade a little, and the falling snow hazes the distance.
            c = mix(c, vec3(Luma(c)) * vec3(0.96, 0.99, 1.04), 0.25 * u_snow);
            if (u_depthOk > 0.5)
                c = mix(c, u_snowFog, (1.0 - exp(-z / (90.0 * u_metre))) * 0.7 * u_snowfall * u_snow);
        }
        if (u_headlight > 0.0)
            c += Headlight(albedo, p, nView);
    }
    o_color = vec4(c, 1.0);
}
)GLSL";

// ---- Snowfall (WEATHER): flakes around the camera, drawn into the HDR image after the composite ----
// No buffers: each flake is 6 vertices of a quad, made from gl_VertexID. Every flake has its own
// place in a box of u_box around the world (its corner a multiple of u_box), drifting with the wind
// and falling at its own pace; the box wraps round the camera, so the same flakes are always around
// you and fly past as you ride (true parallax, no streaks painted on the screen). They fade out near
// the box's edge (no flake pops in where it wraps) and right in front of the lens, and are hidden
// behind whatever is nearer (the scene's depth). A far flake is kept at least about a pixel and a
// half across, fainter instead of smaller, so it doesn't shimmer.
constexpr const char* kSnowVertex = R"GLSL(
uniform mat4  u_proj;
uniform mat3  u_viewRot;     // world -> view (rotation)
uniform vec3  u_camBox;      // the camera in the box (world, wrapped to it)
uniform float u_box;         // the box's size, game units
uniform float u_time;        // s
uniform vec3  u_fall;        // world: how far the snow falls in a second (each flake at its own pace)
uniform vec3  u_drift;       // world: how far the wind has carried it (added up frame by frame, wrapped)
uniform vec3  u_sway1, u_sway2;  // world: two directions across the fall, for each flake's wobble
uniform float u_size;        // a flake's radius, game units
uniform float u_pixel;       // game units per pixel at one unit away
uniform float u_metre;
out vec2  v_corner;
out float v_alpha;
out float v_z;

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float Random(uint flake, uint k) { return float(Hash(flake * 8u + k) & 0xffffffu) / 16777216.0; }

void main()
{
    int flake = gl_VertexID / 6, corner = gl_VertexID - flake * 6;
    vec2 c = vec2((corner == 1 || corner == 2 || corner == 4) ? 1.0 : -1.0,
                  (corner == 2 || corner == 4 || corner == 5) ? 1.0 : -1.0);
    uint id = uint(flake);
    vec3 base = vec3(Random(id, 0u), Random(id, 1u), Random(id, 2u)) * u_box;
    float pace = 0.7 + 0.6 * Random(id, 3u);
    float phase = Random(id, 4u) * 6.2831853;
    float wobble = 0.12 * u_metre;
    vec3 sway = (u_sway1 * sin(u_time * (0.9 + 0.8 * Random(id, 5u)) + phase) +
                 u_sway2 * cos(u_time * (0.7 + 0.6 * Random(id, 6u)) + phase * 1.7)) * wobble;
    vec3 at = base + u_fall * (pace * u_time) + u_drift + sway;
    vec3 rel = (fract((at - u_camBox) / u_box + 0.5) - 0.5) * u_box;  // the nearest copy, around the camera
    vec3 v = u_viewRot * rel;
    float z = -v.z;
    float r = u_size * (0.6 + 0.8 * Random(id, 7u));
    float least = 0.75 * u_pixel * max(z, 0.0);  // at least ~1.5 pixels across
    float shown = max(r, least);
    v_alpha = (r / shown) * (r / shown)
            * smoothstep(0.3 * u_metre, 0.9 * u_metre, z)              // not right in front of the lens
            * (1.0 - smoothstep(0.38 * u_box, 0.5 * u_box, length(rel)));  // nor where the box wraps
    v.xy += c * shown;
    v_corner = c;
    v_z = z;
    gl_Position = u_proj * vec4(v, 1.0);
}
)GLSL";

constexpr const char* kSnowFragment = R"GLSL(
uniform sampler2D u_depth;   // the scene's
uniform mat4  u_invProj;
uniform vec2  u_texel;
uniform float u_metre;
uniform vec3  u_colour;      // a flake's colour, lit as the time of day lights the scene
in vec2  v_corner;
in float v_alpha;
in float v_z;
out vec4 o_color;

void main()
{
    float r2 = dot(v_corner, v_corner);
    if (r2 > 1.0 || v_alpha < 0.003)
        discard;
    float d = texture(u_depth, gl_FragCoord.xy * u_texel).r;
    float seen = 1.0;
    if (d < 0.999999) {
        float n = d * 2.0 - 1.0;
        float sceneZ = -(u_invProj[2][2] * n + u_invProj[3][2]) / (u_invProj[2][3] * n + u_invProj[3][3]);
        seen = clamp((sceneZ - v_z) / (0.1 * u_metre + 0.02 * v_z), 0.0, 1.0);  // softly behind what's nearer
    }
    float a = (1.0 - r2);
    a = a * a * v_alpha * seen;
    o_color = vec4(u_colour * a, a);  // premultiplied
}
)GLSL";

// ---- Bloom: 13-tap downsample (the first one also keeps only what's bright) ----------------
constexpr const char* kBloomDown = R"GLSL(
uniform sampler2D u_src;
uniform vec2  u_srcTexel;
uniform float u_prefilter;
uniform float u_threshold;

vec3 S(vec2 o) { return textureLod(u_src, v_uv + o * u_srcTexel, 0.0).rgb; }

void main()
{
    vec3 col = S(vec2(0.0)) * 0.125
             + (S(vec2(-2.0, 2.0)) + S(vec2(2.0, 2.0)) + S(vec2(-2.0, -2.0)) + S(vec2(2.0, -2.0))) * 0.03125
             + (S(vec2(0.0, 2.0)) + S(vec2(-2.0, 0.0)) + S(vec2(2.0, 0.0)) + S(vec2(0.0, -2.0))) * 0.0625
             + (S(vec2(-1.0, 1.0)) + S(vec2(1.0, 1.0)) + S(vec2(-1.0, -1.0)) + S(vec2(1.0, -1.0))) * 0.125;
    if (u_prefilter > 0.5) {
        float bright = max(col.r, max(col.g, col.b));
        float knee = u_threshold * 0.5 + 1e-4;
        float soft = clamp(bright - u_threshold + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-4);
        col *= max(soft, bright - u_threshold) / max(bright, 1e-4);
    }
    o_color = vec4(col, 1.0);
}
)GLSL";

// ---- Bloom: 9-tap tent upsample, added onto the next larger level ---------------------------
constexpr const char* kBloomUp = R"GLSL(
uniform sampler2D u_src;
uniform vec2 u_srcTexel;

vec3 S(vec2 o) { return textureLod(u_src, v_uv + o * u_srcTexel, 0.0).rgb; }

void main()
{
    vec3 col = (S(vec2(-1.0, 1.0)) + S(vec2(1.0, 1.0)) + S(vec2(-1.0, -1.0)) + S(vec2(1.0, -1.0)))
             + (S(vec2(0.0, 1.0)) + S(vec2(-1.0, 0.0)) + S(vec2(1.0, 0.0)) + S(vec2(0.0, -1.0))) * 2.0
             + S(vec2(0.0)) * 4.0;
    o_color = vec4(col / 16.0, 1.0);
}
)GLSL";

// ---- Sun rays: radial blur of the bright sky around the sun (half resolution) --------------
constexpr const char* kRays = R"GLSL(
uniform sampler2D u_src;
uniform sampler2D u_depth;
uniform vec2  u_sunUV;
uniform float u_aspect;

void main()
{
    const int kSamples = 40;
    vec2 delta = (v_uv - u_sunUV) / float(kSamples) * 0.85;
    vec2 uv = v_uv - delta * Noise(gl_FragCoord.xy);
    float decay = 1.0;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < kSamples; ++i) {
        uv -= delta;
        if (!OnScreen(uv))
            break;
        vec2 o = (uv - u_sunUV) * vec2(u_aspect, 1.0);
        float near = exp(-dot(o, o) * 9.0);
        if (IsSky(textureLod(u_depth, uv, 0.0).r))
            sum += textureLod(u_src, uv, 0.0).rgb * (near * decay);
        decay *= 0.965;
    }
    o_color = vec4(sum * (2.5 / float(kSamples)), 1.0);
}
)GLSL";

// ---- Tire marks: a groove from a to b (texels), or 0 to clear (strength 0, blending off) -------
constexpr const char* kStamp = R"GLSL(
uniform vec2  u_a, u_b;
uniform float u_half;
uniform float u_strength;
uniform float u_arcA;             // how far this segment's first contact lies along the wheel's
                                  // current run of ruts, in the map's texels of trail, counted from
                                  // the run's own first contact (so 0: this is where it began)
uniform float u_fadeOut;          // how far along that run its end turn has run out, the same for
                                  // every segment of a run: past it nothing is pressed. Huge while
                                  // the run is still going: the tire may still come down again
uniform float u_fadeLen;          // how far either turn spreads over, in the same texels of trail
uniform vec3  u_eyeA, u_eyeB;   // the two contacts (view space): pressed only where the tire is down
uniform sampler2D u_ground;     // the terrain's depth, before anything stood on it
uniform float u_check;          // 1: check against it on screen
uniform float u_fallback;       // this wheel's own answer, from its height in the world: 0 in the
                                // air, presses nothing anywhere; 1: where the screen can see this
                                // groove, the terrain's depth decides; 2 (another bike's wheel, whose
                                // air nothing else knows): pressed only where the screen says down
uniform float u_groundTol;      // how far above the ground still counts as down
uniform vec3  u_upView;
uniform float u_fill;           // 1: the first of a groove's two passes (min blend): the ruts already
                                // there are filled in to what this tire leaves of them (see main)

// A tire pressing a fresh rut pushes the dirt aside: ruts already beside it, within kLipReach of its
// half-width, are filled in to kLipFill of their depth (the one under it, to its own new shape). Each
// rut was once a near-flat channel kept by the deepest press anywhere, and a lobby riding much the
// same line merged a dozen of them into one flat trench a couple of metres wide: one huge tire mark.
const float kLipReach = 2.2;
const float kLipFill  = 0.35;

// Both answers must agree. The terrain's depth is the exact one but exists only where the game drew
// the terrain under this wheel, and as the wheel rises the camera rises with it, so in first person
// that pixel holds far-ahead ground at about the tire's height and calls a tire that is in the air
// "down": the world's own answer has to hold as well rather than speak only off screen.
float OnGround(float t)
{
    if (u_fallback < 0.5)
        return 0.0;
    vec3 c = mix(u_eyeA, u_eyeB, t);
    if (u_check > 0.5 && c.z < -0.05) {
        vec2 uv = Project(c);
        if (OnScreen(uv)) {
            float d = textureLod(u_ground, uv, 0.0).r;
            if (!IsSky(d)) {
                vec3 g = ViewPos(uv, d);
                if (u_fallback > 1.5) {
                    // Another bike's wheel: the ground seen there must be at the tire, not past it -
                    // as high within the tolerance either way, and near it along the line of sight.
                    // (Only "no higher than it" pressed ruts under wheels in the air, wherever the
                    // ground seen past them stood as high: a hill, a jump's face.)
                    return abs(dot(c - g, u_upView)) < u_groundTol && length(c - g) < 10.0 * u_groundTol ? 1.0 : 0.0;
                }
                return dot(c - g, u_upView) < u_groundTol ? 1.0 : 0.0;
            }
        }
    }
    return u_fallback > 1.5 ? 0.0 : u_fallback;
}

// The ground's own unevenness: the hash read bilinear over a lattice nine texels wide (about
// 36 cm), so the wobble turns across the ground rather than changing at every texel, and stays
// a function of this texel alone, which is what makes overlapping stamps and a re-press agree.
float Wobble(vec2 p)
{
    vec2 q = p * (1.0 / 9.0);
    vec2 i = floor(q), f = q - i;
    f = f * f * (3.0 - 2.0 * f);
    float a = Noise(i), b = Noise(i + vec2(1.0, 0.0));
    float c = Noise(i + vec2(0.0, 1.0)), d = Noise(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    // Clearing (the window's wipe) comes through with no strength: write nothing.
    if (u_strength <= 0.0) {
        o_color = vec4(0.0);
        return;
    }
    vec2 ab = u_b - u_a;
    // Unclamped along the trail, and clamped only at the ends of it below: a neighbouring
    // segment's rectangle reaches past this segment's contacts, and if the fade stopped at
    // those it would hold the wrong share of the press there - the deeper of the two wins,
    // which steps the ramp at every contact. Read along the trail it keeps on as the same
    // line, so overlapping stamps agree and the fade comes out smooth.
    float te = dot(p - u_a, ab) / max(dot(ab, ab), 1e-6);
    float t = clamp(te, 0.0, 1.0);
    float d = length(p - (u_a + ab * t));
    // Where this texel lies along the trail, in the same distance the run's turns are measured
    // with. Read as a distance rather than as a share of this segment, and every segment of a
    // run answers against the same two marks on the trail: where their rectangles overlap they
    // hold the same value there, which is what lets a turn come out smooth under a max blend.
    // A share of the segment would answer as a straight line of its own, and neighbouring
    // segments reach full depth at different distances, so their lines disagree where they
    // overlap and the deeper of the two steps the turn at every contact.
    float arc = u_arcA + te * length(ab);
    // How much of the press this holds: a wheel's run turns in over the first of it and out over
    // the last of it. The end turn runs over ground beyond the tire's last contact, which the
    // ground test below keeps to where that ground really is - over a jump's gap there is none,
    // so a rut still ends at the lip, only without a straight cut across it.
    float fade = min(clamp(arc / u_fadeLen, 0.0, 1.0), clamp((u_fadeOut - arc) / u_fadeLen, 0.0, 1.0));
    // A tire enters and leaves the ground as a wedge, so where the press is shallow its face
    // doesn't sit as wide either: the rut's beginning and its end narrow along with their depth.
    float hw = max(u_half * (0.4 + 0.6 * fade), 1.5);
    float x = clamp(d / max(hw, 1e-3), 0.0, 1.0);
    // The rut's cross-section: it goes down gradually from the undisturbed ground on either
    // side to the tire's own centre line, where the whole face was pressed deepest and is
    // deepest still. A rounded section, like a tire's channel in soft ground: the walls curve
    // in, they don't drop, so nothing here stands above the ground it came out of.
    // (Rounded down to nothing at its edge: a channel still 70% deep there, as this was, left no
    // ridge between ruts side by side - they merged into one flat trench.)
    float profile = sqrt(max(cos(x * 1.5707963), 0.0));
    // A little wider than the deepest section, so the ground eases into the groove over the
    // last texels and its lips hold the faintest press.
    float cover = 1.0 - smoothstep(hw - 1.5, hw + 0.5, d);
    // And nothing on the ground is quite even, so a little unevenness rides on the whole depth.
    // This has to be a wobble over ground about half a metre across rather than texel to texel:
    // the march answers against this height field, and a field that changes at every texel is a
    // sawtooth whose crossing jumps per texel, which reads as speckle inside the rut and gives
    // its walls random normals - and the jump is a share of the depth, so it grows the deeper
    // you dig. A function of this texel only, so overlapping stamps agree on it and a cache
    // re-press leaves the very same marks.
    float depth = profile * (0.85 + 0.15 * Wobble(p));  // never above 1: the map holds the press
                                                        // as a share of the depth, so going over
                                                        // it would only be flattened where the ray
                                                        // keeps to it, a plateau along the rut
    float down = OnGround(t);
    float press = cover * depth * u_strength * fade * down;
    if (u_fill > 0.5) {
        // Beside this segment (not past its ends, where the same rut carries on): the ruts there
        // are filled in to kLipFill, easing back to untouched at kLipReach; under the tire, to its
        // own new shape. Where nothing is pressed (in the air, a run's ends) nothing is filled.
        float level = te >= 0.0 && te <= 1.0 ? mix(kLipFill, 1.0, smoothstep(hw, hw * kLipReach, d)) : 1.0;
        o_color = vec4(mix(1.0, max(press, level), fade * down));
        return;
    }
    o_color = vec4(press);
}
)GLSL";

// ---- Auto-focus (1x1): eases towards the distance in the middle of the screen --------------
constexpr const char* kFocus = R"GLSL(
uniform sampler2D u_depth;
uniform sampler2D u_prev;
uniform float u_rate;      // 0..1 how far to move this frame (1: jump)
uniform float u_manual;    // m, 0 = auto

void main()
{
    float focus;
    if (u_manual > 0.0) {
        focus = u_manual;
    } else {
        float sum = 0.0;
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                vec2 uv = vec2(0.5) + vec2(float(x) * 0.025, float(y) * 0.035);
                float z = min(LinearZ(textureLod(u_depth, uv, 0.0).r), 300.0);
                sum += log(max(z, 0.3));
            }
        }
        float target = sum / 25.0;
        float prev = log(max(textureLod(u_prev, vec2(0.5), 0.0).r, 0.3));
        focus = (u_rate >= 0.999 || isnan(prev)) ? exp(target) : exp(mix(prev, target, u_rate));
    }
    o_color = vec4(focus, 0.0, 0.0, 1.0);
}
)GLSL";

// ---- Depth of field: bokeh gather at half resolution (rgb, circle of confusion) ------------
constexpr const char* kDof = R"GLSL(
uniform sampler2D u_src;
uniform sampler2D u_depth;
uniform sampler2D u_focus;
uniform float u_strength;
uniform float u_maxBlur;    // half-resolution pixels
uniform float u_radScale;
uniform vec2  u_halfTexel;

float Coc(float z, float focus) { return clamp(abs(1.0 - focus / z) * u_strength * u_maxBlur, 0.0, u_maxBlur); }

void main()
{
    float focus = textureLod(u_focus, vec2(0.5), 0.0).r;
    float centerZ = LinearZ(textureLod(u_depth, v_uv, 0.0).r);
    float centerSize = Coc(centerZ, focus);
    vec3 color = textureLod(u_src, v_uv, 0.0).rgb;
    float total = 1.0;
    float radius = u_radScale;
    float angle = Noise(gl_FragCoord.xy) * 6.2831853;
    for (int i = 0; i < 96; ++i) {
        if (radius >= u_maxBlur)
            break;
        vec2 uv = v_uv + vec2(cos(angle), sin(angle)) * u_halfTexel * radius;
        vec3 sampleColor = textureLod(u_src, uv, 0.0).rgb;
        float sampleZ = LinearZ(textureLod(u_depth, uv, 0.0).r);
        float sampleSize = Coc(sampleZ, focus);
        if (sampleZ > centerZ)
            sampleSize = clamp(sampleSize, 0.0, centerSize * 2.0);
        float m = smoothstep(radius - 0.5, radius + 0.5, sampleSize);
        color += mix(color / total, sampleColor, m);
        total += 1.0;
        radius += u_radScale / radius;
        angle += 2.39996323;
    }
    o_color = vec4(color / total, centerSize / u_maxBlur);
}
)GLSL";

// ---- Depth of field: the sharp image blended with the blurred one by circle of confusion ---
constexpr const char* kDofMerge = R"GLSL(
uniform sampler2D u_src;
uniform sampler2D u_blur;
uniform sampler2D u_depth;
uniform sampler2D u_focus;
uniform float u_strength;
uniform float u_maxBlur;

void main()
{
    float focus = textureLod(u_focus, vec2(0.5), 0.0).r;
    float z = LinearZ(textureLod(u_depth, v_uv, 0.0).r);
    float coc = clamp(abs(1.0 - focus / z) * u_strength * u_maxBlur, 0.0, u_maxBlur);
    vec3 sharp = textureLod(u_src, v_uv, 0.0).rgb;
    vec3 blur = texture(u_blur, v_uv).rgb;
    o_color = vec4(mix(sharp, blur, smoothstep(0.4, 1.5, coc)), 1.0);
}
)GLSL";

// ---- Camera motion blur: each pixel's movement since last frame, from depth -----------------
constexpr const char* kMotion = R"GLSL(
uniform sampler2D u_src;
uniform sampler2D u_depth;
uniform mat4  u_reproj;     // this frame's clip space -> last frame's
uniform float u_scale;
uniform float u_maxPx;

void main()
{
    float d = textureLod(u_depth, v_uv, 0.0).r;
    vec3 c = textureLod(u_src, v_uv, 0.0).rgb;
    vec4 prev = u_reproj * vec4(v_uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    if (prev.w <= 1e-6) {
        o_color = vec4(c, 1.0);
        return;
    }
    vec2 vel = (v_uv - (prev.xy / prev.w * 0.5 + 0.5)) * u_scale;
    float z = LinearZ(d);
    vel *= smoothstep(1.0, 3.0, z);  // the bike and the rider ride along with the camera
    float len = length(vel / u_texel);
    if (len < 0.5) {
        o_color = vec4(c, 1.0);
        return;
    }
    if (len > u_maxPx)
        vel *= u_maxPx / len;
    const int kSamples = 12;
    vec3 sum = c;
    float total = 1.0;
    float jitter = Noise(gl_FragCoord.xy) - 0.5;
    for (int i = 0; i < kSamples; ++i) {
        float t = (float(i) + 0.5 + jitter) / float(kSamples) - 0.5;
        vec2 uv = v_uv + vel * t;
        float sz = LinearZ(textureLod(u_depth, uv, 0.0).r);
        float w = (sz < 2.5 && z > 4.0) ? 0.0 : 1.0;  // keep the bike out of the background's streaks
        sum += textureLod(u_src, uv, 0.0).rgb * w;
        total += w;
    }
    o_color = vec4(sum / total, 1.0);
}
)GLSL";

// ---- Grade: bloom, rays, exposure, saturation, contrast, vignette -> display (luma in alpha) -
constexpr const char* kGrade = R"GLSL(
uniform sampler2D u_src;
uniform sampler2D u_bloom;
uniform sampler2D u_rays;
uniform float u_bloomAmount;
uniform float u_raysAmount;
uniform vec3  u_raysTint;
uniform float u_exposure;
uniform float u_contrast;
uniform float u_saturation;
uniform float u_vignette;
uniform float u_aspect;

void main()
{
    vec3 c = textureLod(u_src, v_uv, 0.0).rgb;
    c += texture(u_bloom, v_uv).rgb * u_bloomAmount;
    c += texture(u_rays, v_uv).rgb * u_raysTint * u_raysAmount;
    c *= u_exposure;
    c = max(mix(vec3(Luma(c)), c, 1.0 + u_saturation), 0.0);
    vec3 g = pow(min(c, vec3(1.0)), vec3(1.0 / 2.2));
    g = clamp((g - 0.5) * (1.0 + u_contrast) + 0.5, 0.0, 1.0);
    vec2 p = (v_uv - 0.5) * vec2(u_aspect, 1.0);
    g *= 1.0 - u_vignette * smoothstep(0.4, 1.1, length(p));
    o_color = vec4(g, dot(g, vec3(0.299, 0.587, 0.114)));
}
)GLSL";

// ---- Final: FXAA (3.11 quality algorithm) and contrast-adaptive sharpening -> the window ---
constexpr const char* kFinal = R"GLSL(
uniform sampler2D u_src;          // display colour, luma in alpha
uniform float u_fxaa;
uniform float u_steps[12];
uniform int   u_stepCount;
uniform float u_edgeThreshold;
uniform float u_edgeThresholdMin;
uniform float u_subpix;
uniform float u_sharpen;

float L(vec2 uv) { return textureLod(u_src, uv, 0.0).a; }

vec3 Fxaa(vec2 uv)
{
    vec2 t = u_texel;
    vec4 rgbyM = textureLod(u_src, uv, 0.0);
    float lumaM = rgbyM.a;
    // FXAA's own texture-space offsets (+y is "south"): the sign logic below depends on them.
    float lumaS = L(uv + vec2(0.0, t.y));
    float lumaE = L(uv + vec2(t.x, 0.0));
    float lumaN = L(uv + vec2(0.0, -t.y));
    float lumaW = L(uv + vec2(-t.x, 0.0));
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(u_edgeThresholdMin, rangeMax * u_edgeThreshold))
        return rgbyM.rgb;

    float lumaNW = L(uv + vec2(-t.x, -t.y));
    float lumaSE = L(uv + vec2(t.x, t.y));
    float lumaNE = L(uv + vec2(t.x, -t.y));
    float lumaSW = L(uv + vec2(-t.x, t.y));
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS;
    float edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE;
    float edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW;
    float edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    float edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = t.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) {
        lumaN = lumaW;
        lumaS = lumaE;
    } else {
        lengthSign = t.y;
    }
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    float lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
        lengthSign = -lengthSign;
    float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);

    vec2 posB = uv;
    vec2 offNP = horzSpan ? vec2(t.x, 0.0) : vec2(0.0, t.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan)  posB.y += lengthSign * 0.5;
    vec2 posN = posB - offNP * u_steps[0];
    vec2 posP = posB + offNP * u_steps[0];
    float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = L(posN);
    float subpixE = subpixC * subpixC;
    float lumaEndP = L(posP);
    if (!pairN)
        lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * u_steps[1];
    if (!doneP) posP += offNP * u_steps[1];
    for (int i = 2; i < 12; ++i) {
        if (i >= u_stepCount || (doneN && doneP))
            break;
        if (!doneN) lumaEndN = L(posN) - lumaNN * 0.5;
        if (!doneP) lumaEndP = L(posP) - lumaNN * 0.5;
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN) posN -= offNP * u_steps[i];
        if (!doneP) posP += offNP * u_steps[i];
    }
    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLengthRcp = 1.0 / (dstP + dstN);
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * -spanLengthRcp + 0.5;
    float subpixH = subpixG * u_subpix;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) uv.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan)  uv.y += pixelOffsetSubpix * lengthSign;
    return textureLod(u_src, uv, 0.0).rgb;
}

void main()
{
    vec3 c = u_fxaa > 0.5 ? Fxaa(v_uv) : textureLod(u_src, v_uv, 0.0).rgb;
    if (u_sharpen > 0.0) {
        vec3 n = textureLod(u_src, v_uv + vec2(0.0, u_texel.y), 0.0).rgb;
        vec3 s = textureLod(u_src, v_uv - vec2(0.0, u_texel.y), 0.0).rgb;
        vec3 e = textureLod(u_src, v_uv + vec2(u_texel.x, 0.0), 0.0).rgb;
        vec3 w = textureLod(u_src, v_uv - vec2(u_texel.x, 0.0), 0.0).rgb;
        vec3 mn = min(c, min(min(n, s), min(e, w)));
        vec3 mx = max(c, max(max(n, s), max(e, w)));
        vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
        vec3 wgt = -amp / mix(8.0, 4.5, u_sharpen);
        c = clamp((c + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);
    }
    o_color = vec4(c, 1.0);
}
)GLSL";

}  // namespace shaders
}  // namespace postfx
}  // namespace mxcam
