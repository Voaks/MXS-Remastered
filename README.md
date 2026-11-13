# MXS Remastered: MX Simulator Dynamic First Person Camera Mod and Shaders upgrade for MX Simulator

Open source and purely OpenGL based.

**Discord:** [discord.gg/YvkUHCMFy6](https://discord.gg/YvkUHCMFy6)

> [!WARNING]
> **Work in progress.** Expect bugs and changes between updates.

---

## How to install

1. **Download the zip** from the releases page on the right side. Latest version: v1.3
2. **Open the zip.** Inside are **`opengl32.dll`** and the **`MXS_REMASTERED_sounds`** folder (for the engine sound mods).
3. **Open your MX Simulator folder**, the one with `mx.exe` in it. To find it, right-click your MX Simulator shortcut and pick **Open file location**.
4. **Put both of them in that folder**, next to `mx.exe`. Drag them out of the zip, keeping `MXS_REMASTERED_sounds` as a folder; don't put anything in Windows' own folders.
5. **Start the game** and get on a bike. Press **E** to open the menu.

You don't need `MXS_REMASTERED.ini`. The mod starts with its default settings and creates `MXS_REMASTERED.ini` in the same folder the first time you press **SAVE** in the menu.

You never need to run the game as administrator. If your MX Simulator folder can't be written to without it (the game is installed under `C:\Program Files (x86)`), the mod keeps `MXS_REMASTERED.ini` and `MXS_REMASTERED.log` in `%LOCALAPPDATA%\MXS Remastered` instead, and puts the sound mods in the game's personal folder, `%LOCALAPPDATA%\MX Simulator`. Paste either path into the File Explorer address bar to open it.

### Uninstall

Delete `opengl32.dll` from your MX Simulator folder. If you turned on any sound mods, turn them off first (or delete the `MXS_REMASTERED_engine_*.saf` files and the `MXS_REMASTERED_sounds` folder; check `%LOCALAPPDATA%\MX Simulator` for them too). To remove everything, also delete `MXS_REMASTERED.ini` and the `MXS_REMASTERED.log` / `MXS_REMASTERED_*.txt` files next to it, and the `%LOCALAPPDATA%\MXS Remastered` folder if there is one.

### Not working?

- **The menu doesn't open with E:** make sure `opengl32.dll` sits right next to `mx.exe` and not in a subfolder. Check that an `MXS_REMASTERED.log` file appears there (or in `%LOCALAPPDATA%\MXS Remastered`) after you start the game. If it doesn't, the game isn't loading the mod.
- **Something else is wrong:** send `MXS_REMASTERED.log` from your MX Simulator folder (or from `%LOCALAPPDATA%\MXS Remastered`, if it's there instead) with your bug report (see below).

---

## Updating

Download the new zip and put `opengl32.dll` and the `MXS_REMASTERED_sounds` folder from it in your MX Simulator folder, replacing the old ones. **Keep your `MXS_REMASTERED.ini`**: it holds your settings, keybinds and what the mod has learnt about your bike, and replacing it resets all of that.

Coming from a version that used `mxcam.ini` and `mxcam_sounds`? The mod renames them to the new names by itself the first time it starts, so you keep everything. Just don't copy a new `MXS_REMASTERED.ini` in next to your old `mxcam.ini`.

---

## The menu

Press **E** in game to open the menu. Its eight tabs run down the left: **FIRST PERSON**, **LOOK** (looking around with a controller), **GRAPHICS**, **WEATHER**, **REPLAY**, **KEYBINDS**, **SOUNDS** (the engine sound mods and the brake squeal) and **OTHER**. The tab's settings are listed in the box on the right: scroll with the mouse wheel, the arrows or the scrollbar beside it. Hover over (or select) any setting to see what it does. Changes apply straight away. Press **SAVE** to keep them for next time. While something is changed, **CANCEL** undoes every change since you last saved. **DEFAULTS** puts everything back, after asking you to confirm.

Looking for a setting? Click **Search** above the tabs and type part of its name (or its section, like "shadow"): every matching setting from every tab is listed. **Esc** or the **x** clears it.

While you drag a **GRAPHICS** or **WEATHER** slider (or step it with the arrow keys), the menu fades out except for that slider, so you can see the whole picture change. You can turn this off on the **OTHER** tab.

### Presets

At the top of the menu, next to the logo, **PRESET 1 / 2 / 3 / 4 / 5** switches between five setting profiles, for example one for racing, one for replays and one for night riding. Each preset has its own copy of every setting on the FIRST PERSON, LOOK, GRAPHICS, WEATHER, REPLAY, SOUNDS and OTHER tabs (keybinds are shared). Changes you make stay with the preset you made them in, and **DEFAULTS** only resets the preset in use. Press **SAVE** to keep them all, and the game starts on whichever preset was in use when you saved. A preset you haven't set up yet starts as a copy of your current settings.

To rename a preset, click the one in use again (it shows a pencil; or press **F2** with the presets selected), type a name of up to 16 characters and press **Enter**. **Esc** leaves the old name, and an empty name goes back to the number. Press **SAVE** to keep it. Names are yours alone: pasting someone else's code never changes them.

### Preset codes

Share your setup without sending files. On the **FIRST PERSON** tab, **COPY CODE** copies your camera settings as one line of text (it starts with `MXS-REMASTERED-CAM-`); on the **GRAPHICS** tab, **COPY CODE** does the same for your graphics (`MXS-REMASTERED-GFX-`). Paste it in Discord or anywhere.

To load someone's code, copy it (the whole message is fine), open the menu and press **PASTE CODE**, from any tab. It loads into the preset you're on: a first person code changes only your first person settings and a graphics code only your graphics, and everything else stays yours (keybinds are never in a code). Press **SAVE** to keep it, or **CANCEL** (or quit without saving) to go back to how that preset was.

| Key | What it does |
|---|---|
| **E** | Opens and closes the menu |
| **F8** | Turns the first person mod on and off |
| **F7** | Turns the shaders on and off (for before / after comparisons) |

All three keys can be changed on the **KEYBINDS** tab: click a key, press the new one, then **SAVE**.

---

## First person camera

A dynamic first person view that rides with your bike: it leans, dips and turns like a rider's head would, without throwing you around. Everything is on the **FIRST PERSON** tab.

- **Two camera modes**: **GAME** improves the game's own first person camera. **CUSTOM** builds the view from your bike itself, with your own look-down angle. The first time you use CUSTOM, ride a few metres so it can find your bike; after that it recognises your bike as soon as you spawn, even standing at the gate.
- **Stabilization**:
  - **Horizon lock**: how much of the bike's lean is taken out of the view, from leaning with the bike to a perfectly level horizon.
  - **Pitch lock**: how much of the nose up / down tilt (wheelies, jump faces) is taken out. Riding on flat ground looks the same at any setting.
  - **Steadiness**: keeps the view calm. The quick jolts of bumps, landings and the suspension diving under braking don't reach it, while it still eases into a corner's lean or a wheelie. It goes up to 200%; above 100% the view follows the lean more and more slowly, and at 200% it hardly leans into a corner at all.
- **Head position**: move your eyes forward / back and up / down. **Hide torso & head** hides your own rider's body (arms and legs stay), so a high FOV doesn't show it from the inside. Your shadow still has them.
- **Look to apex**: in corners your head turns toward the apex, more the tighter the corner, and eases back to straight ahead on the exit.
- **Controller look**: on a controller, look left / right and behind (the d-pad by default). Bind each look to any button, trigger, stick direction or paddle on the KEYBINDS tab. The look angle and turn speed are adjustable on the LOOK tab (CUSTOM camera: with its own eye position for each). It works with the first person mod turned off too: then it only turns the game's own camera (replays are left alone). In third person the camera swings round your rider instead, so you stay in view: looking back, it's out in front of you, facing you.
- **In the air**:
  - **Looseness**: the view keeps pointing forward while the bike swings left and right under it in scrubs and whips.
  - **Whip lock**: holds the view straight along your direction of travel, horizon level, while the bike swings, leans and tilts under you. It eases back onto the bike after landing.
- **Smoothing**: how quickly the view's rotation follows the bike (sharper or floatier), and how far it can fall behind.
- **Neck and impacts**: your head sinks on landings and big hits and springs back up, with a forward nod as the neck compresses. Stiffness, bounce and how far it sinks are all adjustable.

Turn off **First person mod** (or press **F8**) to get the game's own cameras back untouched.

---

## Replay cameras

New cameras for watching your replays, on the **REPLAY** tab. They work while the first person mod is on.

- **GAME**: the replay's own camera, as normal.
- **3RD PERSON**: the camera locked behind your rider. Set how far behind, how high and how far it looks down.
- **HELMET**: a GoPro-style camera mounted on your rider:
  - **Mounts**: **TOP** of the helmet, its **LEFT** or **RIGHT** side, or the **CHEST** (it turns with the body instead of the head). Each mount keeps its own height, forward position and tilt.
  - **Stabilization**: smooths the camera's rotation like a GoPro, from every jolt of the head to silky smooth.
  - **Horizon lock**: takes the rider's lean out of the view, up to a perfectly level horizon.

---

## Other

Quality of life options, on the **OTHER** tab:

- **Hide HUD**: hides the game's HUD (timing, positions, chat) while you ride or watch a replay, for a clean view or clean videos. Move or click the mouse (or press Esc) and it comes back for a few seconds, so the game's menus still work. The game's menus are never hidden.
- **Hide cursor**: hides the game's mouse cursor while you ride or watch a replay. It comes back as soon as you move the mouse.
- **Fullscreen window**: with the game in windowed mode, its window covers the whole screen with no border (borderless fullscreen), so Alt+Tab stays instant. Turn it off and the window goes back to how it was. It's shared by all the presets.
- **Dab indicator**: a small rider icon at the bottom right, next to the speedometer: red and standing normally, green and sitting while you dab. It follows the game's own Sit/Dab key or button and its Dab When Stopped setting. When you turn it on, it asks whether **Sticky Sit/Dab** is on in the game's settings (with it on, one press sits you and the next stands you up); turn it off and on again to change your answer.
- **Fade while adjusting**: the menu fade described above (on by default).
- **Sound mods**: fuller engine sounds for the game's four stock engines (**125 two-stroke**, **250 two-stroke**, **250 four-stroke**, **450 four-stroke**). The stock sounds repeat a single recorded engine cycle over and over, which is why they sound flat. Each pack plays 16 cycles in a random order instead, made from the stock recordings with each cycle firing a little differently, the way a real engine does. Turn one on and it's installed as an `MXS_REMASTERED_engine_*.saf` file in your game folder; it's heard from the next time the game starts (the menu pops up a reminder to restart). Turn it off to go back to the stock sound. Bikes with their own custom sounds keep them. Needs the `MXS_REMASTERED_sounds` folder next to `opengl32.dll`.
- **Brake squeal**: your brakes squeal when you brake hard: the game's own brake controls pressed most of the way while you're moving and slowing down (not with the brake held in the air or standing still). The slider sets how loud it is, and it works with any bike, straight away. The mod makes the sound itself and plays it next to the game's own sound; it needs no files.

---

## Shaders

The mod also includes a free shader pack, built the same way as the camera: pure OpenGL, nothing in the game's files or memory is changed. Open the menu (**E**) and look at the **GRAPHICS** tab. **F7** turns all of it on and off, for before / after comparisons.

- **Time of day**: a slider from midnight to midnight. The sun moves across the sky and lights the bikes from where it is, sunsets turn the light and the sky orange, and at night the sky fills with stars and a moon lights the track (**Night brightness** sets how dark). The game's own lens flare is hidden while a time of day is set, since it would stay at the game's sun. **Game** keeps the game's own lighting.
- **Headlight**: a light on your bike's front plate for night riding. It turns with the bike and lights the track from the front wheel out, in first or third person. **Color** makes it white, amber, or rainbow (slowly going round every colour, like an RGB light). **Beam width** spreads it to the sides; **Beam aim** tilts it up to show more of what's ahead (it reaches further and lights jump faces) or down to keep it close in front of the bike. **Every bike's headlight** puts one on the other bikes around you too (any model, even ones waiting at the gate), so a night race is lit by the whole field.
- **Shadows**: real-time sun shadows from bikes, riders, trees and track objects (drawn from the sun, so even things off screen cast them), plus contact shadows from the track's own shape. **Shadow distance** sets how far around you they reach (up to 200 m): shadows near you stay sharp at any distance (they're drawn in steps, sharpest close to you), and only the far ones get softer. **Shadow resolution** (2048, 4096 or 8192) sharpens them all, if your graphics card can take it. In first person the game leaves out your helmet, your torso and your rear wheel, but your shadow still has them from the start: simple shapes stand in for them (a helmet with its chin bar and peak, a torso built on your rider's own shoulders and hips, so your arms and legs join it, a tire and hub, the swingarm, shock and rear fender). If you ride a few seconds in third person, your shadow also gets your real helmet, body and rear wheel for the rest of the session. (The rear wheel's spokes don't show: it's under you, off screen, and the shadow map is too coarse for anything that thin.)
- **Ambient occlusion**: soft contact shadows in ruts, corners and under the bikes.
- **Dirt detail**: fine, high-resolution grain, pebbles and clods on the track surface up close, only on the ground.
- **Tire marks** and **tire dirt**: your tires dig ruts into the track behind you, and your tires get caked with mud.
- **Sun rays**, **bloom**, **depth of field** (auto-focus), **motion blur** (your own bike stays sharp).
- **Anti-aliasing** (FXAA), **sharpening**, **anisotropic and trilinear texture filtering**.
- **Brightness**, **contrast**, **saturation**, **vignette**.

Only want the shaders? Turn off **First person mod** (FIRST PERSON tab, or F8): the game's own cameras come back and the shaders keep working.

The HUD and menus are drawn after the effects, so they stay sharp and readable at night. If something looks wrong, set the effect to Off in the menu and send `MXS_REMASTERED.log` along with the bug report.

---

## Weather

On the **WEATHER** tab (it needs the shaders on):

- **Snow**: turns snow on. The track turns white, with less snow on steep slopes and drifts across the ground, under a grey, overcast sky with soft light. Your ruts dig through it: pressed snow where a tire just rolled, grey slush deeper down and wet dirt at the bottom, with a lip of pushed-up snow along the edges. Only the ground gets snow, not the bikes or riders.
- **Snowfall**: how heavily it snows. Flakes fall all around you and fly past as you ride, and they haze the distance. At 0% the snow stays on the ground and nothing falls.
- **Wind**: how hard the wind blows the falling snow sideways.

---

## Discord, suggestions and bugs

Join the Discord for updates, help and mod discussion: **[discord.gg/YvkUHCMFy6](https://discord.gg/YvkUHCMFy6)**

Found a bug or have an idea? Post it in the Discord or DM me: **voaks**. Please attach your `MXS_REMASTERED.log` with bug reports.
