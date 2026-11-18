"""The 250 two-stroke engine sound pack, from a recording of a real 250 two-stroke.

The riding sound's recording ("Pro Studio Sound YZ250 Idle & Rev Stock.mp3", 4.5 min): a YZ250 on the
stand, held at one rev level after another (~2500-8300 rpm) and blipped (to ~10700; the revs snapping up
on the throttle, winding down off it). So every moment of it is the engine at some RPM, on the throttle
(the revs held or rising) or off it (falling). (Built before from "250 2 stroke sfx 1.mp3", 46 s of
blips: "it still doesnt really sound like the mp3".) The idle has its own recording, and so does the
rev limiter (the YZ's blips' peaks).

MX Simulator plays an engine from a soundtable (see make_engine_sounds.py): a row of recordings for
each of a set of RPMs, each cell's pulses - its power strokes - played one per power stroke: for a
two-stroke, one per revolution. (The stock 250's cells each hold one firing, stored over two
revolutions' worth of samples - at half speed - which the game plays in one. Built with two firings to
a pulse, as a four-stroke's engine cycle, it played everything twice as fast: in game "super duper
high pitched and completely different sounding".) So for every row:
  - its firings are gathered from the recording: every firing where the engine was at that row's RPM
    (within a few percent; wider where the recording has few), the revs rising for the on-throttle
    cell, falling for the off-throttle one;
  - each is stretched to the stock's pulse length at the row's RPM, one firing a pulse (the pitch
    heard is the engine's own: a real 250's onboard video rides at 130-200 Hz, its RPM's),
    lined up so every firing's pulse falls at the same point, and the runs of them evened in loudness;
  - and averaged, each lined up on the others by its shape, into ONE pulse: the 450's way
    (make_450f.py). (Played as recorded, one after another - riding_cell, kept for comparison - it
    was "super choppy and not smooth at all" driving.) That's the cell's body, below XOVER, each
    harmonic as strong as in the real firings; above it the rasp, real firings from the recording's
    steady pull - the same ones in every row - turned to the row's spectrum; plus ROUGH of the pull's
    firing-to-firing roughness (see averaged_cells). Taken from FORMANT higher RPM, its tone lowered.

The idle rows play the idle recording (IDLE_SOURCE) as recorded at the game's idle, a seamless loop of it.

    python make_250.py "C:\\Games\\MX Simulator" "V:\\mx sim sound mods\\250 2t new" [out_dir] [--engine default125]

(--engine default125: the same sound for the 125 two-stroke, on the 125's own soundtable - its idle row,
its rows up to ~13300 rpm; see ENGINES.)

Writes <out_dir>/MXS_REMASTERED_engine_default250.pack (installed from the mod's panel like the others),
and sweep_default250.wav: the pack played as the game would (idling, revving to the top on the throttle,
off it back down).
Needs soundfile and imageio-ffmpeg (pip install soundfile imageio-ffmpeg).
"""
import glob
import os
import subprocess
import sys
import wave

import numpy as np
from scipy import signal

import saf

RATE = 44100
ENGINE = 'default250'       # the stock soundtable replaced (--engine: another of ENGINES)
# The 250's riding tone, on and off the throttle (Hz as heard, dB), "when he revs the bike it should sound
# more like that": a real 250 two-stroke blipped on the stand, filmed ("alfredo 250 2t sounds.mov"). Its revs
# replayed through the pack, band by band against it: on the throttle the pack's 800-3200 Hz stood 5-10 dB
# over (his is scooped there), its 9-13 kHz 5-8 dB under; off it, 300-400 Hz and 1.6 kHz a few dB over.
# (Not below 150 Hz: a phone's microphone.) And his revs fall 9 dB off the throttle, the pack's 7: OFF_GAIN.
ALFREDO_ON = [(150, 0.0), (240, 0.0), (480, -2.5), (950, -4.8), (1350, -4.8), (1900, -9.0), (2700, -6.8),
              (3800, -5.0), (5400, 1.3), (7600, 1.4), (10800, 5.5), (15200, 7.5)]
ALFREDO_OFF = [(150, 0.0), (240, 1.5), (340, -6.0), (480, -3.8), (670, -1.6), (950, 0.5), (1900, -4.0),
               (2700, -1.2), (3800, -3.6), (5400, -2.0), (7600, -2.4), (10800, 0.4), (15200, 4.5)]
ENGINES = {                 # each engine's own: where the game idles (its stock idle loop's row), its top row,
                            # the rows its rev limiter plays from, and its cells' names
    'default250': dict(IDLE_GAME_RPM=1687.0, TOP_RPM=13600.0, LIMITER_ROWS_FROM_RPM=10600.0, CELL='real250',
                       FEWEST=15, LOUD_TOP_RPM=11093.0, GAIN_DB=-8.7, PITCH=79 / 96, LIMITER_PITCH=84 / 96, LIMITER_MADE_WITH=84 / 96,
                       RIDING_BRIGHTEN=[(700, 0.0), (1500, 1.5), (3000, 3.0)],   # ("unmuffle the riding sound
                                                                                # slightly")
                       TONE_ON=ALFREDO_ON, TONE_OFF=ALFREDO_OFF, OFF_GAIN=0.35, PITCH_RAMP=(8940.0, 8950.0),
                       POPS=3, IDLE_BELOW_RPM=2000.0),
}
# The 125: "make the current 250 2t sound the sound for the 125 as well" - the 250's, every setting, on the 125's
# soundtable (its idle where the 125 idles, its stock idle loop's row; as loud as the 250, SOUND_LIKE). (Before:
# the 250's first sound on its own rows - TOP_RPM 13600, LIMITER_ROWS_FROM_RPM 12400, no PITCH, tone or pops.)
ENGINES['default125'] = dict(ENGINES['default250'], IDLE_GAME_RPM=1500.0, CELL='real125', SOUND_LIKE='default250')
# (Kept apart for a while - the 250's riding a semitone lower - then "make the current 250 2t the 125 sound as
# well" again.)
# (FEWEST: a cell with no more firings than this from round its RPM takes the recording's highest instead -
# rows past the recording's top, ~10800 rpm. The 250's rows went to 11093 rpm; "when the rev limiter gets
# higher the pitch needs to increase with it": the game, past a soundtable's top row, seems to hold the pitch
# there - now to ~13500, as the 125's. LOUD_TOP_RPM: no row louder than the one at this RPM (the 250's rows
# above its old top as loud as it was). GAIN_DB: the pack turned at least this much (the 250's turn since its
# limiter was made steady), so it keeps its loudness.)
# PITCH: the riding rows' pitch, of the RPM's; LIMITER_PITCH the limiter's (the 250's: "the riding sound and
# limiter need to be lower pitched": 81 firings to 96 revolutions, ~3 semitones down; then "put the riding
# sound up 1 semitone and the limiter up 2 semitones": 86, and 91; then both 89 - "right around the rev
# limiter i still hear the slight double pitch": wherever the pitch steps the game mixes a row of each, and
# the 250's limit seems to lie about where it stepped, 9482-9862; then "put the riding sound down 1
# semitone": 84, the limiter with it, so nothing steps; then "keep the rev limiter at what it was before":
# 89 again, stepping a row lower than before - 8767 to 9118 - clear of where it bounces on its limit; then
# "make the riding sound 1 more semitone lower (just the WEHHHH). dont touch the idle or limiter": 79; then
# "make the limiter 1 semitone lower": 84).
# PITCH_RAMP: the RPMs over which the riding pitch goes to the limiter's (the 250's done by 9862, its rows
# above all the limiter's: the stock's top row is 10476 - the game's limit about there - and "when the rev
# limiter is at max theres like this weird like double layered sound": it sat between a row at each pitch,
# 10256 and 10666. In one step, between two rows: eased over several, rows a firing or two apart in 96
# beat against each other slowly where the game mixes them - a 3 dB wobble over 8600-9800 rpm.)
# POPS: crackles from the idle's stretch sprinkled into the riding rows from POP_FROM_RPM up ("it sounds a
# bit stale at the higher rpms. could you maybe sprinkle in a bit of the kettle pop noises that are in the
# idle?") - their cells POP_CYCLES times as long, so the pops come round only every few seconds (a few in
# 96 revolutions, round again every half second, were "a rock in my engine"). Then "it should only do the 3
# pop thing once when you get to that rev amount, not repeat it over and over": the game picks a cell's
# part from its count of revolutions, not from when the revs got there, so a sound can't start on arriving -
# one burst in a long cell instead (960 revolutions: once in ~6 s at 10000 rpm), only near the limit.
POPS = 0                    # ... this many bursts in a cell's POP_CYCLES x 96 revolutions, at full (POP_FULL_RPM up)
POP_FROM_RPM = 8500.0       # (9300, one burst: "im pretty much not hearing it at all now" - three, from here)
POP_FULL_RPM = 8500.0
POP_CYCLES = 10
POP_PEAK = 5.0              # ... each pop's peak this much of its cell's RMS (at most; each 0.6-1 of it)
POP_BURST = 3               # ... each pop a burst of this many ("make it like 3 repeating at once really fast")
POP_GAP = (0.040, 0.065)    # ... each this long after the last (seconds, as heard; "increase the interval
                            # between the 3 pops a little": was 25-45 ms)
POP_FADE = 0.85             # ... each this much as loud as the last
POP_HZ = 700.0              # ... their sound above this
POP_BEFORE = 0.002          # ... from this before their peak (seconds)
POP_AFTER = 0.030           # ... to this after it
POP_CRACKLE = 8.0           # ... taken from firings crackling at least this much (Firings.crackle)
POP_SEED = 250
# IDLE_BELOW_RPM: the stock's rows below this play the idle; above, up to RIDING_FROM_RPM, the riding
# sound at the stock's RPMs (the 250's: "the low end when im riding is a bit glitchy" - its idle, a hold at
# ~4550 rpm played at ~80 Hz, with its pops, took the rows up to 2403, the riding's starting at ~40 Hz at
# 2703: dropping that low riding, the game mixed the two).
IDLE_BELOW_RPM = 1e9
RIDING_BRIGHTEN = []        # the riding rows' (below the limiter's) top end lifted so (Hz as heard, dB; see tone)
LIMITER_MADE_WITH = None    # the riding pitch the limiter's rows were made alongside, if not PITCH (each row is lined
                            # up with the one below it: the riding's moving shifted the limiter's sound a little -
                            # "dont touch the... limiter")
SOUND_LIKE = None           # an engine whose stock loudness (and off-throttle columns) a pack takes, not its own
FEWEST = 0
PITCH = 1.0
LIMITER_PITCH = 1.0
PITCH_RAMP = (1e9, 1e9 + 1)
LOUD_TOP_RPM = None
GAIN_DB = None
PULSE_REVS = 1              # the game plays each pulse (a power stroke) over this many revolutions: one, a two-stroke
STORE_REVS = 2              # ... stored, as the stock 250's are, over this many revolutions' worth of samples
SOURCE = 'Pro Studio Sound YZ250 Idle & Rev Stock.mp3'   # the revving, in the folder given (a YZ250 on the stand)
IDLE_SOURCE = SOURCE        # the idle, in the same folder: the YZ held steady at ~4450-4700 rpm, 3:02-3:17 ("use the
                            # idle sound (first like 15 seconds) from compare_riding_yz_vs_pack... i think its
                            # better than the current"; before, "250 2 stroke idling 1.mp3" 1.6-9.8 s without
                            # 5.15-6.7 s, a real idle at ~2330 rpm)

# Where things are in the recording (seconds).
IDLE_FROM, IDLE_TO = 182.3, 197.5  # the idle in IDLE_SOURCE
IDLE_GAME_RPM = 1687.0      # the RPM the game idles at (the stock's lowest row: its idle loop's): the idle loop
                            # plays as recorded there, its pops as often, its pitch the mp3's
IDLE_STEADY = False         # ... made steady from it, as the riding cells are (see main), rather than looped as recorded
IDLE_CUT = None             # ... a stretch left out of it (seconds, from, to), if any
IDLE_FADE = 0.04            # the loop's joins cross-faded over this (seconds)
IDLE_SEAM_REACH = 0.6       # ... its end and start looked for within this of the idle's ends
IDLE_JOIN_REACH = 0.3       # ... the cut's ends within this of it
IDLE_CREST = 8.0            # the idle cells' peaks held to this much of their RMS (its pops ~7x: kept)
RIDE_FROM, RIDE_TO = 122.0, 258.0   # the revving: held at one rev level after another (2:02-2:41 ~3000-5500 rpm,
                                    # 2:50-3:20 and 3:24-3:31 ~2500-5000, 3:52-4:05 ~4000-8300), and blipped
                                    # (4:05-4:17, to ~10700: the limiter's); before it near silence
RIDE_SKIP = [(161.3, 170.3), (200.2, 204.0), (211.0, 232.5)]   # ... but not these: held high, their pitch can't be
                                    # told (a line at ~215-230 Hz, another ~80 Hz under it); and the idling between

# Pitch tracking: the firing rate looked for between these (Hz), moving at most TRACK_MOVE a frame.
TRACK_LO, TRACK_HI = 25.0, 230.0
TRACK_MOVE = 1.06
FALLING = -0.25             # the revs falling faster than this (of themselves a second): off the throttle; else on
TREND_WINDOW = 0.4          # ... measured over this (seconds). (Over 70 ms, held revs - the YZ recording's - read
                            # +-0.8 a second from the pitch track's wobble: half of every hold counted as falling.
                            # Over 0.4 s holds read within +-0.15, the revs winding down off the throttle -0.45.)

# Gathering a row's firings: within BAND of its RPM, widened by BAND_WIDEN up to BAND_MOST until there
# are at least LEAST of them; at most MOST (the nearest in RPM kept).
BAND = 1.02
BAND_WIDEN = 1.01
BAND_MOST = 1.15
LEAST = 32
MOST = 256
SMOOTH = 0.25               # each riding row's average eased with each neighbouring row's this much
ROUGH = 0.1                 # each riding pulse: plus this much (of its body's RMS) of a real firing's difference
                            # from its neighbours - the same in every row (see rough_sequence)
ROUGH_PULSES = 96           # ... a cell of this many pulses, each its own
ROUGH_LOCAL = 8             # ... each firing's difference from the average of this many either side
ROUGH_EDGE = 0.1            # ... eased in and out over this much of the pulse at both ends
ROUGH_SPAN = 600            # ... looked for among this many firings round the middle of that run
CRACKLE_HZ = 3000.0         # a firing whose top end (above this) peaks more than CRACKLE times the firings' around it
CRACKLE = 4.0               # ... crackles (the YZ's: a third of its firings, 6-30x; a steady firing's ~3.2): left out of
                            # the roughness and rasp, and of every row's strength
STEADY_DB = 1.5             # a steady firing: within this of the firings around it in loudness,
STEADY_ALIKE = 0.75         # ... and at least this like their average in shape (and not crackling)
ROUGH_FALLING = -0.2        # ... from the longest run whose revs never fall faster than this (the YZ's: a hold
                            # at ~4700 rpm, 3:08)
XOVER = 800.0               # each riding cell's body (the average's shape) below this (Hz), its rasp above: the
                            # pull's real firings, turned to the row's spectrum (the mp3's harmonics stand clear
                            # of the noise up to ~700 Hz; above it's mostly rasp)
HIGH = 0.85                 # ... the rasp this much as strong as in the recording
RASP_NOISE = 0.0            # ... this much of it steady hiss, shaped as the real firings' top end (see rasp)
RASP_BLEND = 0.5            # ... each pulse this much the average of the pulses either side of it
RASP_EVEN_HZ = 3000.0       # ... (RASP_EVEN below and above this each on its own)
RASP_CREST = 4.0            # ... its peaks held to this much of its RMS
RASP_EVEN = 0.95            # ... each pulse's loudness taken this much of the way (in dB) to the typical pulse's
                            # ("still has slightly too much roughness": this, ROUGH 0.1, RASP_BLEND 0.5; before
                            # 0.85, 0.15 and 0.35:)
                            # ("needs to be smoothed out slightly": 0.6, firing-to-firing jumps 0.42 dB -> 0.29;
                            # "needs to be smoother... a bit rough sounding": this, ROUGH 0.15, HIGH 0.85 and
                            # RASP_BLEND 0.35 - jumps 0.17 dB, firings alike 0.90)
HIGH_EDGE = 0.04            # ... eased in and out over this much of the pulse at both ends
ALIGN = 0.15                # each firing turned at most this much of itself to sit on the average
JOIN_EASE = 0.08            # a riding pulse's ends brought to zero over this much of it, either side
SNAP = 0.03                 # a riding cell's start moves at most this much of the pulse to an upward zero crossing
FIRE_AT = 0.2               # where each firing's pulse is put (a fraction of the firing)
SPIKE = 3.0                 # a firing peaking more than this over the others gathered is left out (a backfire,
                            # looping round with the cell every second or so, would stand out)
CREST = 4.0                 # every cell's peaks held to this much of its RMS (12 dB). Unheld (the idle's pops
                            # ~25x) the pack, turned so its loudest cell fits full scale, came out 16 dB quiet;
                            # at 4x it's about as loud as the 250F pack (top rows ~8000 RMS)

FORMANT = 1.08              # each riding row built from the firings recorded this much higher in RPM, slowed to the
                            # row's: the engine's pitch stays the row's, its tone (resonances) drops as much ("make
                            # it a little lower pitched")

LIMITER_SOURCE = 'Pro Studio Sound YZ250 Idle & Rev Stock.mp3'   # the rev limiter, in the same folder
LIMITER_FROM, LIMITER_TO = 245.0, 257.0   # ... from its revving here (4:05-4:17: blips, six peaking ~9500-10700 rpm)
LIMITER_LEAST_RPM = 9400.0  # ... its firings above this (the blips' peaks), one after another, a loop
LIMITER_ROWS_FROM_RPM = 10600.0   # ... played by the on-throttle cells of the rows from here up (the game holds the
                            # engine at the top rows on the limiter; their pitch the game's RPM, as every row's)
LIMITER_STEADY = True       # ... or made steady instead, as every riding row is (from the same firings - the blips'
                            # peaks are the recording's top), one constant "EHHHH": the loop as recorded popped
                            # ("its supposed to just be a constant sound")

RIDING_FROM_RPM = 2600.0    # the stock rows below this are idle rows (the loop); from here up, riding rows
RIDING_STEP = 1.04          # each riding row this much higher than the one before
TOP_RPM = 11500.0           # ... up to here (above the recording's ~10200 its top firings, stretched)
IDLE_OFF_GAIN = 0.5         # the idle rows' off-throttle cell this much as loud as on
OFF_GAIN = 0.45             # the riding rows' off-throttle cells this much as loud as on (-7 dB; see ENGINES)
TONE_ON = []                # the riding cells' tone on the throttle and off it (see tone, ENGINES)
TONE_OFF = []
RISE_DB_PER_OCTAVE = 6.0    # the riding rows' loudness rise (the 450's, fitted to a video of the YZ450 mod)


# ---- Reading and measuring ------------------------------------------------------------------------

def read(path):
    """The recording, mono, at RATE (decoded with ffmpeg: any format)."""
    import imageio_ffmpeg
    out = subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), '-v', 'error', '-i', path, '-ac', '1', '-ar', str(RATE),
                          '-f', 's16le', '-'], capture_output=True, check=True).stdout
    return np.frombuffer(out, '<i2').astype(np.float64) / 32768.0


def pitch_track(x, win=4096, hop=256):
    """The firing rate (Hz) through the recording, one value a sample: in each window the pitch whose
    harmonics stand highest above the gaps between them, taken as the best path through the
    recording that moves at most TRACK_MOVE a window (so it never jumps an octave). Also each window's
    time and how clear the pitch was there."""
    f, t, Z = signal.stft(x, RATE, nperseg=win, noverlap=win - hop, nfft=4 * win)
    L = np.log(np.abs(Z) + 1e-9)
    cands = np.geomspace(TRACK_LO, TRACK_HI, 360)
    k = np.arange(1, 9)
    on = (k[None] * cands[:, None]).ravel()
    off = ((k[None] + 0.5) * cands[:, None]).ravel()
    score = np.empty((L.shape[1], len(cands)))
    for j in range(L.shape[1]):
        score[j] = (np.interp(on, f, L[:, j]) - np.interp(off, f, L[:, j])).reshape(len(cands), -1).mean(axis=1)
    reach = int(np.ceil(np.log(TRACK_MOVE) / np.log(cands[1] / cands[0])))
    T, C = score.shape
    acc = score[0].copy()
    back = np.zeros((T, C), int)
    for j in range(1, T):
        best = np.full(C, -1e9)
        arg = np.zeros(C, int)
        for mv in range(-reach, reach + 1):
            src = np.clip(np.arange(C) - mv, 0, C - 1)
            v = acc[src] - 0.02 * abs(mv)
            better = v > best
            best[better] = v[better]
            arg[better] = src[better]
        acc = best + score[j]
        back[j] = arg
    path = np.zeros(T, int)
    path[-1] = int(np.argmax(acc))
    for j in range(T - 1, 0, -1):
        path[j - 1] = back[j, path[j]]
    f0 = cands[path]
    return np.interp(np.arange(len(x)), t * RATE, f0), t, f0


def follow_firings(x, rate_hz, start, end, search=0.12, size=256):
    """Where each firing starts, followed from `start` to `end` (samples): each mark the point about a
    firing on from the last whose waveform is most like the last firing's and like the average of the
    recent ones. Returns the marks (fractional samples)."""
    s = float(start)
    template = None
    marks = [s]
    while True:
        p = RATE / rate_hz[int(s)]
        w = int(round(p))
        lo, hi = int(s + p * (1 - search)), int(s + p * (1 + search))
        if hi + w + 2 >= min(end, len(x)):
            break
        prev = x[int(s):int(s) + w] - x[int(s):int(s) + w].mean()
        best = None
        for c in range(lo, hi + 1):
            seg = x[c:c + w] - x[c:c + w].mean()
            score = np.dot(prev, seg) / (np.linalg.norm(prev) * np.linalg.norm(seg) + 1e-12)
            if template is not None:
                r = np.interp(np.arange(size) * w / size, np.arange(w), seg)
                score += np.dot(template, r) / (np.linalg.norm(template) * np.linalg.norm(r) + 1e-12)
            if best is None or score > best[0]:
                best = (score, c)
        c = best[1]
        r = np.interp(np.arange(size) * w / size, np.arange(w), x[c:c + w] - x[c:c + w].mean())
        template = r if template is None else 0.8 * template + 0.2 * r
        marks.append(float(c))
        s = float(c)
    return np.array(marks)


def pops(x, at_least=0.06):
    """The idle's top end's loudness (its pops stand out), and where its pops are (samples)."""
    hp = signal.sosfiltfilt(signal.butter(2, 1500.0 / (RATE / 2), 'high', output='sos'), x)
    env = signal.filtfilt(*signal.butter(2, 200.0 / (RATE / 2)), np.abs(hp))
    p, _ = signal.find_peaks(env, height=0.6 * np.percentile(env, 99), distance=int(at_least * RATE))
    return env, p


# ---- Shaping cells ---------------------------------------------------------------------------------

def best_join(x, quiet, fade, ends, starts):
    """Where to go from x (at one of `ends`) on to x (at one of `starts`): the pair whose last `fade`
    samples are most alike, neither near a pop (quiet). Returns (how alike, end, start)."""
    best = None
    for s in starts:
        if not quiet[s]:
            continue
        a = x[s - fade:s]
        for e in ends:
            if not quiet[e]:
                continue
            b = x[e - fade:e]
            score = np.dot(a, b) / (np.sqrt(np.dot(a, a) * np.dot(b, b)) + 1e-12)
            if best is None or score > best[0]:
                best = (score, e, s)
    return best


def idle_loop(x):
    """The idle recording (all of it, x) as one long seamless loop: IDLE_FROM to IDLE_TO without
    IDLE_CUT, each join (the cut's, and the loop's end back to its start) cross-faded over IDLE_FADE
    from what led up to the one end into what led up to the other, where those are most alike and no
    pop is near (a pop cut by the fade would come out doubled or halved). Returns (loop, how alike the
    cut's ends were, how alike the loop's)."""
    env, p = pops(x)
    fade = int(IDLE_FADE * RATE)
    clear = int(0.04 * RATE)
    quiet = np.ones(len(x), bool)
    for q in p:
        quiet[max(0, q - fade - clear):q + clear] = False
    a, b = int(IDLE_FROM * RATE), min(len(x), int(IDLE_TO * RATE))
    join, reach = int(IDLE_JOIN_REACH * RATE), int(IDLE_SEAM_REACH * RATE)
    loop_alike, e0, s0 = best_join(x, quiet, fade, range(b - reach, b, 32), range(a + fade, a + fade + reach, 32))
    ramp = np.sin(np.linspace(0.0, np.pi / 2, fade)) ** 2
    if IDLE_CUT is None:
        loop = x[s0:e0].copy()
        loop[-fade:] = loop[-fade:] * (1.0 - ramp) + x[s0 - fade:s0] * ramp
        return loop, None, loop_alike
    c0, c1 = int(IDLE_CUT[0] * RATE), int(IDLE_CUT[1] * RATE)
    cut_alike, e1, s1 = best_join(x, quiet, fade, range(c0 - join, c0, 32), range(c1 + fade, c1 + fade + join, 32))
    first, second = x[s0:e1].copy(), x[s1:e0].copy()
    first[-fade:] = first[-fade:] * (1.0 - ramp) + x[s1 - fade:s1] * ramp
    second[-fade:] = second[-fade:] * (1.0 - ramp) + x[s0 - fade:s0] * ramp
    return np.concatenate([first, second]), cut_alike, loop_alike


def tone(y, shape):
    """A riding cell (as a loop) turned by `shape`: [(Hz as heard, dB)], eased between them on a log scale,
    flat beyond its ends. (A cell plays STORE_REVS / PULSE_REVS times as fast as it's stored.)"""
    if not shape:
        return y
    hz = np.fft.rfftfreq(len(y), 1.0 / RATE) * STORE_REVS / PULSE_REVS
    at, db = np.log(np.array([h for h, _ in shape])), np.array([d for _, d in shape])
    gain = 10.0 ** (np.interp(np.log(np.maximum(hz, 1.0)), at, db) / 20.0)
    return np.fft.irfft(np.fft.rfft(y) * gain, len(y))


def to_length(loop, length):
    """The loop resampled to `length` samples (as a loop: it still runs on into itself)."""
    return signal.resample(loop, length)


def stretch(x, a, b, n):
    """x from a to b (fractional samples) stretched to n samples."""
    pos = a + (b - a) * np.arange(n) / n
    i = np.floor(pos).astype(int)
    fr = pos - i
    # (Cubic between the samples: the stretch is small, a few percent.)
    xm, x0, x1, x2 = x[i - 1], x[i], x[i + 1], x[i + 2]
    return x0 + 0.5 * fr * (x1 - xm + fr * (2 * xm - 5 * x0 + 4 * x1 - x2 + fr * (3 * (x0 - x1) + x2 - xm)))


def warp_to_zero(y, pieces, most=0.05):
    """Each inner piece boundary (where the game goes from one engine cycle to the next) moved on to
    the nearest zero crossing within `most` of a piece, the waveform between eased to fit."""
    n = len(y)
    size = n / pieces
    j = np.flatnonzero(np.sign(y[:-1]) != np.sign(y[1:]))
    zc = j + y[j] / (y[j] - y[j + 1])
    src, dst = [0.0], [0.0]
    for k in range(1, pieces):
        b = k * size
        if len(zc):
            z = zc[np.argmin(np.abs(zc - b))]
            if abs(z - b) <= most * size:
                src.append(z)
                dst.append(b)
    src.append(float(n))
    dst.append(float(n))
    return np.interp(np.interp(np.arange(n), dst, src), np.arange(n + 1), np.append(y, y[0]))


def zero_ends(y, cycles, width=24):
    """Every boundary between cycles brought to exactly zero."""
    length = len(y) // cycles
    bump = np.hanning(2 * width + 1)
    y = y.copy()
    for k in range(cycles):
        b = k * length
        v = y[b]
        idx = (b + np.arange(-width, width + 1)) % len(y)
        y[idx] -= v * bump
    return y


def limit(y, crest=CREST, ms=1.0):
    """Its peaks held to `crest` times its RMS: a gain under 1 wherever they'd pass it, eased in and out
    over ~ms either side (on the cell as a loop)."""
    most = crest * np.sqrt(np.mean(y ** 2))
    g = np.minimum(1.0, most / (np.abs(y) + 1e-12))
    w = max(3, int(RATE * ms / 1000.0)) | 1
    pad = np.concatenate([g[-w:], g, g[:w]])
    from scipy.ndimage import minimum_filter1d
    pad = minimum_filter1d(pad, 2 * w + 1)
    win = np.hanning(2 * w + 1)
    pad = np.convolve(pad, win / win.sum(), 'same')
    return y * np.minimum(1.0, pad[w:-w] if w else pad)


def pulse_length(rpm):
    """A pulse's samples in a cell at rpm (the stock 250's: two revolutions' worth, at half speed)."""
    return int(round(STORE_REVS * 60.0 * RATE / rpm))


def pulse_time(rpm):
    """How long the game takes to play a pulse at rpm (samples of output)."""
    return int(round(PULSE_REVS * 60.0 * RATE / rpm))


def to_sw(x):
    return np.clip(np.round(x), -32768, 32767).astype('<i2').tobytes()


def from_sw(data):
    return np.frombuffer(data[:len(data) // 2 * 2], '<i2').astype(np.float64)


def stock_tables(game_dir, engine=None):
    """An engine's stock soundtable and recordings (ENGINE's, unless given), from the game's archives."""
    ENGINE = engine or globals()['ENGINE']
    files = {}
    for path in sorted(glob.glob(os.path.join(game_dir, '*.saf'))):
        if os.path.basename(path).lower().startswith(('mxsr_', 'mxs_remastered_')):
            continue
        try:
            files.update(saf.extract(path, lambda n: (n == ENGINE + '.soundtable' or n.startswith(ENGINE + '/')) and n not in files))
        except ValueError:
            pass
    return files


# ---- The firings -----------------------------------------------------------------------------------

class Firings:
    """Every firing of the revving, in order: where it starts and ends, its RPM, whether the revs were
    rising (on the throttle) or falling, how loud it is, and where its pulse falls in it."""

    def __init__(self, x, rate_hz, track_t, track_f0, start=RIDE_FROM, end=RIDE_TO):
        self.x = x
        marks = follow_firings(x, rate_hz, int(start * RATE), int(end * RATE))
        self.a, self.b = marks[:-1], marks[1:]
        mid = (self.a + self.b) / 2
        # RPM from the pitch track (smooth), the trend from it too (d ln f / dt, over TREND_WINDOW).
        dt = track_t[1] - track_t[0]
        trend = signal.savgol_filter(np.log(track_f0), int(TREND_WINDOW / dt) | 1, 2, deriv=1, delta=dt)
        self.rpm = 60.0 * np.interp(mid, track_t * RATE, track_f0)
        self.trend = np.interp(mid, track_t * RATE, trend)
        self.on = self.trend >= FALLING
        length = self.b - self.a
        self.even = np.abs(length / (60.0 * RATE / self.rpm) - 1.0) < 0.15   # (the follow kept to the pitch)
        self.level = np.array([np.sqrt(np.mean(x[int(a):int(b)] ** 2)) for a, b in zip(self.a, self.b)])
        self.peak = np.array([np.abs(x[int(a):int(b)]).max() for a, b in zip(self.a, self.b)])
        env = signal.filtfilt(*signal.butter(2, 400.0 / (RATE / 2)), np.abs(x))
        self.pulse = np.array([np.argmax(env[int(a):int(b)]) / max(1, int(b) - int(a)) for a, b in zip(self.a, self.b)])
        # How much each firing crackles: its top end's peak over the RMS of the firings' top end around it.
        top = signal.sosfiltfilt(signal.butter(4, CRACKLE_HZ / (RATE / 2), 'high', output='sos'), x)
        peak = np.array([np.abs(top[int(a):int(b)]).max() for a, b in zip(self.a, self.b)])
        rms = np.array([np.sqrt(np.mean(top[int(a):int(b)] ** 2)) for a, b in zip(self.a, self.b)])
        self.crackle = peak / (signal.medfilt(rms, 31) + 1e-12)
        # How steady each firing is: its loudness against the firings' around it (dB), and how like their
        # average its shape is. (Held at light load on the stand the YZ misfires: a quarter of its firings
        # bang 3-8 dB over the ones around them, shaped otherwise.)
        loud = 20 * np.log10(self.level + 1e-12)
        self.dev = loud - signal.medfilt(loud, 15)
        shapes = np.array([stretch(x, a, b, 256) / (l + 1e-12) for a, b, l in zip(self.a, self.b, self.level)])
        near = signal.convolve(shapes, np.ones((2 * 8 + 1, 1)), 'same') - shapes
        near /= 16.0
        sc = shapes - shapes.mean(1, keepdims=True)
        nc = near - near.mean(1, keepdims=True)
        self.alike = (sc * nc).sum(1) / (np.sqrt((sc ** 2).sum(1) * (nc ** 2).sum(1)) + 1e-12)
        self.steady = (self.crackle <= CRACKLE) & (np.abs(self.dev) <= STEADY_DB) & (self.alike >= STEADY_ALIKE)

    def pick(self, rpm, on):
        """The firings for a cell at rpm (on or off the throttle), as runs: [[index, ...], ...] in the
        recording's order; and the band they came from."""
        band = BAND
        while True:
            ok = (self.on == on) & self.even & (self.rpm >= rpm / band) & (self.rpm <= rpm * band)
            if ok.sum() >= LEAST or band >= BAND_MOST:
                break
            band *= BAND_WIDEN
        idx = np.flatnonzero(ok)
        if len(idx):
            idx = idx[self.peak[idx] / self.level[idx] <= SPIKE * np.median(self.peak[idx] / self.level[idx])]
        if len(idx) <= FEWEST:  # (past the recording's top, or its bottom: the nearest in RPM it has)
            same = np.flatnonzero((self.on == on) & self.even)
            idx = same[np.argsort(np.abs(np.log(self.rpm[same] / rpm)))[:LEAST]]
        if len(idx) > MOST:
            idx = np.sort(idx[np.argsort(np.abs(np.log(self.rpm[idx] / rpm)))[:MOST]])
        idx = np.sort(idx)
        runs = [[int(idx[0])]]
        for i in idx[1:]:
            if i == runs[-1][-1] + 1:
                runs[-1].append(int(i))
            else:
                runs.append([int(i)])
        return runs, band

    def cut(self, i, turn):
        """Firing i's start and end, moved `turn` of a firing (the marks moved, so a run stays joined)."""
        a = self.a[i] + turn * (self.b[i] - self.a[i])
        b = self.b[i] + turn * ((self.b[i + 1] - self.a[i + 1]) if i + 1 < len(self.a) else (self.b[i] - self.a[i]))
        return a, b


def riding_cell(fir, rpm, on):
    """A cell at rpm: the recording's firings there (see Firings.pick) one after another, each stretched
    to a firing at rpm, every run turned so its pulses fall FIRE_AT in and turned to one loudness, each
    run eased into the next from what led into it in the recording (and the last into the first), the
    cell started on an upward zero crossing and its pulses' boundaries on zero crossings.
    Returns (samples, pulses, firings, band)."""
    runs, band = fir.pick(rpm, on)
    y, count = runs_cell(fir, runs, rpm)
    return y, count, count, band


def runs_cell(fir, runs, rpm):
    """A cell at rpm of these runs of firings ([[index, ...], ...]), as riding_cell's. Returns (samples,
    pulses)."""
    lf = pulse_length(rpm)   # (one firing a pulse)
    count = sum(len(r) for r in runs)
    bounds = np.round(np.arange(count + 1) * lf).astype(int)
    target = np.median(fir.level[[i for r in runs for i in r]])
    out = np.zeros(bounds[-1])
    k = 0
    pieces = []  # (where each run starts in the cell, its first firing, its turn, its gain)
    for run in runs:
        turn = float(np.median(fir.pulse[run])) - FIRE_AT
        gain = target / (np.median(fir.level[run]) + 1e-12)
        pieces.append((bounds[k], run[0], turn, gain))
        for i in run:
            a, b = fir.cut(i, turn)
            out[bounds[k]:bounds[k + 1]] = gain * stretch(fir.x, a, b, bounds[k + 1] - bounds[k])
            k += 1
    # Each run eased in: over the end of what's before it (the cell's last firing, for the first run),
    # what led into this run in the recording - which runs on into its first firing without a join.
    for at, first, turn, gain in pieces:
        if first == 0:
            continue
        n = int(round(lf))
        a, b = fir.cut(first - 1, turn)
        lead = gain * stretch(fir.x, a, b, n)
        h = n // 2
        ramp = np.sin(np.linspace(0.0, np.pi / 2, h)) ** 2
        idx = (at - h + np.arange(h)) % len(out)
        out[idx] = out[idx] * (1.0 - ramp) + lead[-h:] * ramp
    total = len(out)
    j = np.flatnonzero((out < 0) & (np.roll(out, -1) >= 0))
    if len(j):
        off = np.where(j > total // 2, j - total, j)
        near = off[np.argmin(np.abs(off))]
        if abs(near) <= 0.05 * lf:
            out = np.roll(out, -int(near))
    if count > 1:
        out = warp_to_zero(out, count)
    return zero_ends(out, count), count


def row_average(fir, rpm, on, row, ref=None, formant=FORMANT, play_rpm=None):
    """A riding row's firing, the 450's way: the average of the recording's firings there (see
    Firings.pick), each stretched to a pulse and lined up on the others by its shape; turned to sit
    best on `ref` (the row below's, or this row's on-throttle one: the game mixes neighbouring rows, and
    on with off - lined up each on its own they matched at 0.43 typically, some opposite: half
    cancelling as the revs moved). Also how strong each harmonic is in those firings (RMS, each over its
    loudness, weighted by it: the average's are weaker - what isn't the same from firing to firing cancels, most of all
    up high), and how far it was turned (samples). Returns (average, firings, band, strength, turned).

    (Every firing there in a row - riding_cell - was "super choppy and not smooth at all" driving: the
    game mixes the two rows around the RPM, and changes rows as it moves, and each row went through
    different firings. One firing a row fires the same shape at every RPM, only easing from row to row.)"""
    runs, band = fir.pick(rpm * formant, on)
    n = pulse_length(play_rpm or rpm)   # (a firing's length: play_rpm, the firings' rate, where not one a pulse)
    stack = []
    for run in runs:
        turn = float(np.median(fir.pulse[run])) - FIRE_AT
        for i in run:
            a, b = fir.cut(i, turn)
            stack.append(stretch(fir.x, a, b, n) / (fir.level[i] + 1e-12))
    stack = np.array(stack)
    loud = np.array([fir.level[i] for run in runs for i in run]) ** 2
    calm = np.array([fir.crackle[i] <= CRACKLE for run in runs for i in run])
    if calm.sum() >= 8:
        loud = loud * calm   # (the strength - the rasp's too - of the firings that don't crackle)
    # Lined up by shape: each firing turned (round itself, at most ALIGN of a firing) to sit best on
    # the average, twice over - so the average keeps the sharp pulse instead of smearing it.
    most = int(ALIGN * n)
    lags = np.concatenate([np.arange(0, most + 1), np.arange(n - most, n)])
    for _ in range(2):
        M = np.conj(np.fft.rfft(stack.mean(axis=0)))
        for k in range(len(stack)):
            corr = np.fft.irfft(np.fft.rfft(stack[k]) * M, n)
            stack[k] = np.roll(stack[k], -int(lags[np.argmax(corr[lags])]))
    clean = stack.mean(axis=0)
    # (Each firing's harmonics counted by how loud it is, as they're heard: the quiet ones are mostly
    # their 2nd harmonic - counted alike, at 5900 rpm it came out 7 dB over the recording's.)
    strength = np.sqrt(np.sum(loud[:, None] * np.abs(np.fft.rfft(stack, axis=1)) ** 2, axis=0) / loud.sum())
    shift = 0
    if ref is not None:
        r_ = np.interp(np.arange(n) * len(ref) / n, np.arange(len(ref)), ref)
        corr = np.fft.irfft(np.fft.rfft(clean) * np.conj(np.fft.rfft(r_)), n)
        shift = int(np.argmax(corr))
        clean = np.roll(clean, -shift)
    return clean, len(stack), band, strength, shift


def resampled(y, n):
    """y (one pulse, as a loop) at n samples."""
    return signal.resample(y, n)


def rough_sequence(fir, size=512):
    """The roughness every riding cell shares: ROUGH_PULSES firings in a row from the recording's
    longest unbroken run on the throttle, each one's difference from the firings around it (the
    average of ROUGH_LOCAL either side, all lined up by shape at `size` samples) - how a real engine
    varies from one firing to the next - over that average's RMS, eased to nothing at both ends of
    the pulse (every pulse of a cell then starts and ends as its average does: joins stay clean).
    Returns [ROUGH_PULSES arrays of `size`], the firings they are (indices), and where (seconds)."""
    good = fir.even & (fir.trend > ROUGH_FALLING)   # (the pull's revs dip a little now and then: still the pull)
    best, start = (0, 0), None
    for i in range(len(good) + 1):
        if i < len(good) and good[i]:
            start = i if start is None else start
        elif start is not None:
            if i - start > best[1] - best[0]:
                best = (start, i)
            start = None
    a, b = best
    middle = (a + b) // 2
    a, b = max(a, middle - ROUGH_SPAN // 2), min(b, middle + ROUGH_SPAN // 2)
    idx = list(range(a, b))
    stack = np.array([stretch(fir.x, fir.a[i], fir.b[i], size) / (fir.level[i] + 1e-12) for i in idx])
    most = int(ALIGN * size)
    lags = np.concatenate([np.arange(0, most + 1), np.arange(size - most, size)])
    for _ in range(2):
        M = np.conj(np.fft.rfft(stack.mean(axis=0)))
        for k in range(len(stack)):
            corr = np.fft.irfft(np.fft.rfft(stack[k]) * M, size)
            stack[k] = np.roll(stack[k], -int(lags[np.argmax(corr[lags])]))
    # The firings used: those that don't crackle (CRACKLE), nearest the run's middle, in order. (Each pulse
    # is eased in and out on its own, so they needn't follow on from each other. With every firing - a
    # third of the YZ's crackle - the same few pops came round every 96 revolutions: "kettle popping".)
    # (And only steady ones - Firings.steady: with the misfires' bangs in it, a quarter of the firings,
    # the same few came round every 96 revolutions: "the same popping sound thats in the idle... like a
    # rock is in my engine".) Each one's difference from the steady firings around it.
    clean = [k for k in range(len(stack)) if fir.steady[idx[k]]]
    keep = sorted(sorted(clean, key=lambda k: abs(idx[k] - middle))[:ROUGH_PULSES])
    seq = []
    for k in keep:
        around = [j for j in clean if j != k and abs(j - k) <= 4 * ROUGH_LOCAL]
        local = stack[around].mean(axis=0)
        seq.append((stack[k] - local) / (np.sqrt(np.mean(local ** 2)) + 1e-12) * edges(size, ROUGH_EDGE))
    return seq, [idx[k] for k in keep], (fir.a[idx[keep[0]]] / RATE, fir.b[idx[keep[-1]]] / RATE)


def edges(n, part):
    """1 through a pulse of n, eased to 0 over `part` of it at both ends."""
    e = max(2, int(part * n))
    w = np.ones(n)
    w[:e] = 0.5 - 0.5 * np.cos(np.linspace(0.0, np.pi, e))
    w[-e:] = w[:e][::-1]
    return w


def split(n, rpm):
    """For each harmonic of a pulse of n at rpm (rfft bins 0 .. n//2: one firing a pulse), how much of
    it the body plays and how much the rasp (amplitudes; their powers add to 1), crossing at XOVER."""
    r = (np.arange(n // 2 + 1) * rpm / 60.0 / XOVER) ** 4
    return 1.0 / np.sqrt(1.0 + r * r), r / np.sqrt(1.0 + r * r)


def third_octave_gain(want, have, width=2.0 ** (1.0 / 6.0)):
    """Per harmonic, the gain taking `have` to `want` (both per-harmonic amplitudes) over the third of
    an octave around it: the spectrum's shape, not each harmonic."""
    cw = np.concatenate([[0.0], np.cumsum(want ** 2)])
    ch = np.concatenate([[0.0], np.cumsum(have ** 2)])
    g = np.zeros(len(want))
    for k in range(1, len(want)):
        lo, hi = max(1, int(k / width)), min(len(want) - 1, int(np.ceil(k * width)))
        g[k] = np.sqrt((cw[hi + 1] - cw[lo]) / (ch[hi + 1] - ch[lo] + 1e-20))
    return g


def rasp(fir, idx, n, rpm, turn, strength):
    """A riding cell's top end (above XOVER): the pull's firings idx one after another - the same
    firings in every cell, so the rows rasp together as the game mixes them - each cut `turn` of a
    firing on from having its pulse FIRE_AT in (as the row's average was, before it was turned),
    stretched to a pulse of n, over its loudness; the whole turned to the row's own firings' strength
    (third_octave_gain: the spectrum of the recording at this RPM, on or off the throttle), and eased to
    nothing at both ends of every pulse (the joins stay on the body's zero crossings).
    Returns [len(idx)] pulses of n."""
    t0 = float(np.median(fir.pulse[idx])) - FIRE_AT + turn
    t0 -= np.floor(t0 + 0.5)
    pulses = np.array([stretch(fir.x, *fir.cut(i, t0), n) / (fir.level[i] + 1e-12) for i in idx])
    if RASP_NOISE > 0:
        # Steady hiss in its place (RASP_NOISE of it), shaped through each firing as the real firings' top
        # end is on average. The YZ's own firings, held at light load on the stand, spit and burble firing
        # to firing - "the same popping sound thats in the idle... like a rock is in my engine". The hiss
        # of pulse k is the same in every cell (made at one length, stretched to each), so rows mix as one.
        top = np.fft.irfft(np.fft.rfft(pulses, axis=1) * split(n, rpm)[1], n, axis=1)
        shape = np.sqrt(np.mean(top ** 2, axis=0))
        w = max(3, int(0.03 * n)) | 1
        shape = np.convolve(np.concatenate([shape[-w:], shape, shape[:w]]), np.hanning(w) / np.hanning(w).sum(), 'same')[w:-w]
        made = 4096
        hiss = np.array([np.interp(np.arange(n) * made / n, np.arange(made),
                                   np.random.default_rng(1000 + k).standard_normal(made)) for k in range(len(idx))]) * shape
        hiss *= np.sqrt(np.mean(top ** 2) / (np.mean(hiss ** 2) + 1e-20))
        pulses = (1.0 - RASP_NOISE) * pulses + RASP_NOISE * hiss
    have = np.sqrt(np.mean(np.abs(np.fft.rfft(pulses, axis=1)) ** 2, axis=0))
    g = third_octave_gain(strength, have) * split(n, rpm)[1] * HIGH
    spec = np.fft.rfft(pulses.ravel())
    y = np.fft.irfft(spec * np.interp(np.arange(len(spec)) / len(idx), np.arange(len(g)), g), len(idx) * n)
    y = limit(y, RASP_CREST)   # (no crackle left poking out: with the crackly firings left out, still 6-9x at 3500 rpm)
    y = y.reshape(len(idx), n)
    if RASP_BLEND > 0:
        # Each pulse blended with the pulses either side of it (as a loop), its loudness kept: the grain
        # changes more gently firing to firing.
        level = np.sqrt(np.mean(y ** 2, axis=1, keepdims=True))
        y = (1.0 - RASP_BLEND) * y + RASP_BLEND * 0.5 * (np.roll(y, 1, axis=0) + np.roll(y, -1, axis=0))
        y *= level / (np.sqrt(np.mean(y ** 2, axis=1, keepdims=True)) + 1e-12)
    # Each pulse's loudness evened (RASP_EVEN), below and above RASP_EVEN_HZ each on its own.
    upper = (np.arange(n // 2 + 1) * rpm / 60.0 >= RASP_EVEN_HZ).astype(float)
    Y = np.fft.rfft(y, axis=1)
    out = np.zeros_like(y)
    for mask in (1.0 - upper, upper):
        part = np.fft.irfft(Y * mask, n, axis=1)
        level = np.sqrt(np.mean(part ** 2, axis=1))
        out += part * ((np.median(level) / (level + 1e-12)) ** RASP_EVEN)[:, None]
    return out * edges(n, HIGH_EDGE)


def pops_from(fir):
    """The pops sprinkled into the high riding rows: POPS of the idle stretch's crackles (the "kettle pop
    noises that are in the idle"), each as it sounds - its crack from POP_BEFORE before its peak to
    POP_AFTER after, as recorded (not fitted to a firing: squeezed into one, cut off at its end, they
    "dont sound good"), above POP_HZ (the engine's own sound under it left out: it would be another
    pitch) - where it falls in the cells (a fraction of them), from what RPM, and how loud (of POP_PEAK).
    Returns [(fraction, from RPM, [(seconds after, loudness, samples as heard)] - a burst)]."""
    held = np.flatnonzero((fir.a / RATE >= IDLE_FROM) & (fir.b / RATE <= IDLE_TO) & fir.even & (fir.crackle >= POP_CRACKLE))
    held = held[np.argsort(fir.crackle[held])][len(held) // 4:]          # (the crackliest three quarters)
    rng = np.random.default_rng(POP_SEED)
    pick = rng.choice(held, size=min(POPS * POP_BURST, len(held)), replace=False)
    a, b = int(IDLE_FROM * RATE), int(IDLE_TO * RATE)
    top = signal.sosfiltfilt(signal.butter(4, POP_HZ / (RATE / 2), 'high', output='sos'), fir.x[a:b])
    before, after = int(POP_BEFORE * RATE), int(POP_AFTER * RATE)
    shape = np.ones(before + after)
    rise, fall = int(0.001 * RATE), int(0.012 * RATE)
    shape[:rise] = np.sin(np.linspace(0.0, np.pi / 2, rise)) ** 2
    shape[-fall:] = np.cos(np.linspace(0.0, np.pi / 2, fall)) ** 2
    cracks = []
    for i in pick:
        lo, hi = int(fir.a[i]) - a, int(fir.b[i]) - a
        peak = lo + int(np.argmax(np.abs(top[lo:hi])))
        crack = top[peak - before:peak + after] * shape
        cracks.append(crack / (np.abs(crack).max() + 1e-12))
    bursts = len(cracks) // POP_BURST
    # Where: spread over the cells' revolutions, unevenly (each in its own share, anywhere in its middle half).
    where = (np.arange(bursts) + 0.25 + 0.5 * rng.random(bursts)) / bursts
    rng.shuffle(where)
    # From what RPM: one after another between POP_FROM_RPM and POP_FULL_RPM, more of them the higher.
    start = POP_FROM_RPM + (POP_FULL_RPM - POP_FROM_RPM) * np.arange(bursts) / bursts
    rng.shuffle(start)
    out = []
    for k in range(bursts):
        # A burst: POP_BURST cracks, each POP_GAP after the last (unevenly), a little quieter each time.
        loud = (0.75 + 0.25 * rng.random()) * POP_FADE ** np.arange(POP_BURST) * (0.9 + 0.2 * rng.random(POP_BURST))
        at = np.concatenate([[0.0], np.cumsum(POP_GAP[0] + (POP_GAP[1] - POP_GAP[0]) * rng.random(POP_BURST - 1))])
        out.append((float(where[k]), float(start[k]),
                    [(float(t), float(g), c) for t, g, c in zip(at, loud, cracks[k * POP_BURST:(k + 1) * POP_BURST])]))
    return out


def add_pops(y, rpm, popping):
    """A high riding cell (as stored) with the pops in: each at its place, as heard at the row's RPM (stored
    at the cells' half speed), its peak its loudness x POP_PEAK x the cell's RMS, eased in over a row above
    its RPM."""
    rms = np.sqrt(np.mean(y ** 2))
    y = y.copy()
    slow = STORE_REVS / PULSE_REVS
    for where, start, burst in popping:
        g = float(np.clip((rpm - start) / (start * (RIDING_STEP - 1.0)), 0.0, 1.0))
        if g <= 0:
            continue
        for after, loud, crack in burst:
            w = signal.resample(crack, int(round(len(crack) * slow)))
            at = (int(where * len(y)) + int(round(after * RATE * slow)) + np.arange(len(w))) % len(y)
            y[at] += g * loud * POP_PEAK * rms * w
    return y


def averaged_cells(fir, rows, formant=FORMANT, pitch=None, riding_pitch=None):
    """Every riding row's cells (on and off the throttle): {(row index, 'on' / 'off'): (samples, pulses,
    note)}. rows: [(row index, rpm)], low to high. Each cell is ROUGH_PULSES pulses, each:
      - the body (below XOVER): the row's average (row_average), eased with its neighbours' (SMOOTH of
        each, of its own the rest - few firings to a row, the shape wobbled row to row), each harmonic
        as strong as in the real firings (the average's phases), started on an upward zero crossing
        within SNAP;
      - plus ROUGH of the shared roughness (rough_sequence) below XOVER;
      - plus the rasp (above XOVER, see rasp).
    The roughness and rasp of pulse k are the same firing in every cell: the game plays the same pulse
    of every cell at once (its count round the cells' pulses), so the rows vary together and easing
    between them stays smooth. (One averaged firing alone was "way smoother now but a little too
    clean"; with a little roughness "it still doesn't really sound like the mp3 at all": its harmonics
    peaked - the 2nd up to 12 dB over the mp3's share - and it lacked the mp3's top end, 3-13 kHz down
    5-10 dB: what differs firing to firing cancels in the average.)"""
    seq, idx, where = rough_sequence(fir)
    popping = pops_from(fir) if POPS and pitch is None else None
    print('roughness and rasp: %d firings without crackle, held on the throttle, %.2f-%.2f s' % (len(seq), where[0], where[1]))
    # PITCH: a cell's pulses hold this many firings, not one each - every firing (and so its pitch) this much
    # of a revolution's rate - made at that rate, then the cell stretched to its pulses.
    pulses = len(seq)
    def firings(rpm, kind):
        # (On and off the throttle alike: the game mixes the two as the limiter cuts in and out - a row's
        # on cell at the limiter's pitch and its off cell at the riding's played both at once, "this weird
        # like double layered sound". The pitch steps up over PITCH_RAMP.)
        if pitch is not None:
            p = pitch
        else:
            a, b = PITCH_RAMP
            low = PITCH if riding_pitch is None else riding_pitch
            p = low + (LIMITER_PITCH - low) * float(np.clip((rpm - a) / (b - a), 0.0, 1.0))
        return int(round(pulses * p))
    avg = {}
    below = None
    for r, rpm in rows:
        on = row_average(fir, rpm, True, r, below, formant, rpm * firings(rpm, 'on') / pulses)
        off = row_average(fir, rpm, False, r, on[0], formant, rpm * firings(rpm, 'off') / pulses)
        avg[(r, 'on')], avg[(r, 'off')] = on, off
        below = on[0]
    cells = {}
    for k, (r, rpm) in enumerate(rows):
        for kind in ('on', 'off'):
            clean, count, band, strength, shift = avg[(r, kind)]
            n = len(clean)
            fired = firings(rpm, kind)
            rate = fired / pulses
            near = [rows[j][0] for j in (k - 1, k + 1) if 0 <= j < len(rows)]
            eased = clean * (1.0 - SMOOTH * len(near))
            strong = strength * (1.0 - SMOOTH * len(near))
            for rr in near:
                other = avg[(rr, kind)]
                eased = eased + SMOOTH * resampled(other[0], n)
                s = other[3][:len(strength)] * n / len(other[0])
                strong = strong + SMOOTH * np.concatenate([s, strength[len(s):]])
            low = split(n, rpm * rate)[0]
            E = np.fft.rfft(eased)
            body = np.fft.irfft(strong * low * np.exp(1j * np.angle(E)), n)
            at = 0
            j = np.flatnonzero((body < 0) & (np.roll(body, -1) >= 0))
            if len(j):
                o = np.where(j > n // 2, j - n, j)
                near_at = o[np.argmin(np.abs(o))]
                if abs(near_at) <= SNAP * n:
                    at = int(near_at)
            body = np.roll(body, -at)
            level = np.sqrt(np.mean(body ** 2))
            pos = np.arange(n) * len(seq[0]) / n
            rough = np.array([np.interp(pos, np.arange(len(d)), d) for d in seq[:fired]])
            rough = np.fft.irfft(np.fft.rfft(rough, axis=1) * low, n, axis=1) * edges(n, ROUGH_EDGE)
            top = rasp(fir, idx[:fired], n, rpm * rate, (shift + at) / n, strong)
            y = body[None] + ROUGH * level * rough + top
            reps = POP_CYCLES if popping and rpm >= POP_FROM_RPM else 1
            y = np.tile(y, (reps, 1)).ravel()
            # Every firing's ends brought to zero gently, over JOIN_EASE of it: the body's start is on a zero
            # crossing only where one lay within SNAP (31 of the YZ's 74 cells); elsewhere a 24-sample notch
            # up to 1.45x the cell's RMS ticked every firing, more or less from row to row ("kettle popping").
            y = zero_ends(y, fired * reps, width=max(24, int(JOIN_EASE * n)))
            if fired != pulses:
                # (The pulses' joins then fall mid-firing: every cell alike there - the rows lined up - and
                # the game's mix of them always adds to one, so they meet without a step.)
                y = signal.resample(y, pulses * reps * pulse_length(rpm))
            if reps > 1:
                y = add_pops(y, rpm, popping)
            cells[(r, kind)] = (y, pulses * reps, '%3d firings within %4.1f%%%s' % (
                count, 100 * (band - 1), ' +pops' if reps > 1 else ''))
    return cells


# ---- Played as the game plays it (for the sweep) ---------------------------------------------------

def play(table, cells_of, rpm_at, load_at, seconds):
    """The pack played as the game plays a soundtable (as far as we know it): every engine cycle, the
    cells of the two rows around the RPM and the two load columns around the load mixed by how near
    each is, each cell's cycle (the engine's cycle count, round its cycles) stretched to the engine
    cycle's length. table: [rpm], cells_of(row, column) -> (samples, cycles)."""
    rpms = np.array(table)
    out, t, k = [], 0.0, 0
    loads = np.array([0.05, 0.25, 0.75, 0.95])
    while t < seconds:
        rpm, load = rpm_at(t), load_at(t)
        n = pulse_time(rpm)
        i = int(np.clip(np.searchsorted(rpms, rpm) - 1, 0, len(rpms) - 2))
        wr = float(np.clip((rpm - rpms[i]) / (rpms[i + 1] - rpms[i]), 0.0, 1.0))
        j = int(np.clip(np.searchsorted(loads, load) - 1, 0, len(loads) - 2))
        wl = float(np.clip((load - loads[j]) / (loads[j + 1] - loads[j]), 0.0, 1.0))
        mix = np.zeros(n)
        for r, a in ((i, 1 - wr), (i + 1, wr)):
            for c, b in ((j, 1 - wl), (j + 1, wl)):
                if a * b <= 0:
                    continue
                y, count = cells_of(r, c)
                size = len(y) / count
                piece = y[int(round((k % count) * size)):int(round((k % count + 1) * size))]
                mix += a * b * np.interp(np.arange(n) * len(piece) / n, np.arange(len(piece)), piece)
        out.append(mix)
        t += n / RATE
        k += 1
    return np.concatenate(out)


# ---- The pack ---------------------------------------------------------------------------------------

def main():
    if '--engine' in sys.argv:
        at = sys.argv.index('--engine')
        engine = sys.argv[at + 1]
        del sys.argv[at:at + 2]
        if engine not in ENGINES:
            sys.exit('--engine: one of %s' % ', '.join(ENGINES))
        globals()['ENGINE'] = engine
    globals().update({k: v for k, v in ENGINES[ENGINE].items() if k != 'CELL'})
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    game_dir, src_dir = sys.argv[1], sys.argv[2]
    out_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'MXS_REMASTERED_sounds')
    os.makedirs(out_dir, exist_ok=True)
    from make_engine_sounds import parse_soundtable

    for name in (SOURCE, IDLE_SOURCE, LIMITER_SOURCE):
        if not os.path.exists(os.path.join(src_dir, name)):
            sys.exit('no %s in %s' % (name, src_dir))
    x = read(os.path.join(src_dir, SOURCE))
    x = x / (np.abs(x).max() + 1e-12)
    print('%s: %.1f s' % (SOURCE, len(x) / RATE))

    # (Pitch tracked over the revving only - the whole recording takes minutes - its times then the recording's.)
    rate_hz, track_t, track_f0 = pitch_track(x[int(RIDE_FROM * RATE):int(RIDE_TO * RATE)])
    track_t = track_t + RIDE_FROM
    rate_hz = np.interp(np.arange(len(x)), track_t * RATE, track_f0)
    fir = Firings(x, rate_hz, track_t, track_f0)
    mid = (fir.a + fir.b) / 2 / RATE
    for a, b in RIDE_SKIP:
        fir.even &= ~((mid >= a) & (mid < b))
    print('revving %.1f-%.1f s: %d firings followed (%d kept to the pitch), %.0f-%.0f rpm; on the throttle %d, off %d' % (
        RIDE_FROM, RIDE_TO, len(fir.a), fir.even.sum(), fir.rpm[fir.even].min(), fir.rpm[fir.even].max(),
        (fir.on & fir.even).sum(), (~fir.on & fir.even).sum()))

    xi = read(os.path.join(src_dir, IDLE_SOURCE))
    xi = xi / (np.abs(xi).max() + 1e-12)
    loop, cut_alike, seam = idle_loop(xi)
    # As many pulses (revolutions, one a pulse) as the game turns over in the loop's time at IDLE_GAME_RPM.
    loop_revs = int(round(len(loop) / RATE * IDLE_GAME_RPM / 60.0))
    print("%s %.1f-%.1f s%s: loop of %.2f s, its ends alike %.2f; %d revolutions at %.0f rpm" % (
        IDLE_SOURCE, IDLE_FROM, IDLE_TO, '' if IDLE_CUT is None else ' without %.2f-%.2f s (ends alike %.2f)' % (
            IDLE_CUT[0], IDLE_CUT[1], cut_alike), len(loop) / RATE, seam, loop_revs, IDLE_GAME_RPM))
    if IDLE_STEADY:
        # Steady instead: that stretch's engine made as a riding cell is (averaged_cells: the averaged
        # firing, the steady firings' rasp), at its RPM, as heard - one constant "EHHHH", without its
        # misfires' pops ("for the idle can you remove the popping its supposed to just be a constant
        # sound"). Repeated so the game's revolutions at IDLE_GAME_RPM come out nearly whole.
        held = (fir.a / RATE >= IDLE_FROM) & (fir.b / RATE <= IDLE_TO) & fir.even
        hold_rpm = float(np.median(fir.rpm[held]))
        made_idle = averaged_cells(fir, [(-3, hold_rpm / RIDING_STEP), (-2, hold_rpm), (-1, hold_rpm * RIDING_STEP)],
                                   formant=1.0, pitch=1.0)   # (its own tone, not the riding rows' lowered one)
        cell = made_idle[(-2, 'on')][0]
        one = signal.resample(cell, int(round(len(cell) * PULSE_REVS / STORE_REVS)))
        revs = lambda k: k * len(one) / RATE * IDLE_GAME_RPM / 60.0
        reps = min(range(1, 6), key=lambda k: abs(revs(k) - round(revs(k))) / revs(k))
        loop = np.tile(one, reps)
        loop_revs = int(round(len(loop) / RATE * IDLE_GAME_RPM / 60.0))
        print('idle made steady: the hold at %.0f rpm as a riding cell, %.2f s, x%d: %d revolutions at %.0f rpm' % (
            hold_rpm, len(one) / RATE, reps, loop_revs, IDLE_GAME_RPM))

    # The limiter: the high revving's firings above LIMITER_LEAST_RPM (pitch tracked from a little before).
    xl = read(os.path.join(src_dir, LIMITER_SOURCE))
    xl = xl / (np.abs(xl).max() + 1e-12)
    lead = 2.0
    xl = xl[int((LIMITER_FROM - lead) * RATE):int((LIMITER_TO + 1.0) * RATE)]
    lfir = Firings(xl, *pitch_track(xl), start=lead, end=lead + LIMITER_TO - LIMITER_FROM)
    high = np.flatnonzero(lfir.even & (lfir.rpm >= LIMITER_LEAST_RPM))
    high = high[lfir.peak[high] / lfir.level[high] <= SPIKE * np.median(lfir.peak[high] / lfir.level[high])]
    limiter_runs = [[int(high[0])]]
    for i in high[1:]:
        if i == limiter_runs[-1][-1] + 1:
            limiter_runs[-1].append(int(i))
        else:
            limiter_runs.append([int(i)])
    print('%s %.0f-%.0f s: limiter loop of %d firings (%d runs) at %.0f-%.0f rpm' % (
        LIMITER_SOURCE, LIMITER_FROM, LIMITER_TO, len(high), len(limiter_runs), lfir.rpm[high].min(), lfir.rpm[high].max()))

    files = stock_tables(game_dir)
    rpms, loads, cells = parse_soundtable(files[ENGINE + '.soundtable'].decode('latin1'))
    # (Loudness, and which columns play off the throttle, from SOUND_LIKE's stock table where it's set.)
    like = SOUND_LIKE or ENGINE
    lfiles = files if like == ENGINE else stock_tables(game_dir, like)
    lrpms, lloads, lcells = parse_soundtable(lfiles[like + '.soundtable'].decode('latin1'))

    def stock_level(rpm):
        """How loud the stock row nearest rpm is on the throttle (RMS of its single-cycle 'a' cells)."""
        r = int(np.argmin(np.abs(np.log(np.array(lrpms) / rpm))))
        levels = [np.sqrt(np.mean(from_sw(lfiles[c['file']]) ** 2)) for c in lcells[r * len(lloads):(r + 1) * len(lloads)]
                  if c['cycles'] == 1 and c['file'] in lfiles and os.path.splitext(c['file'])[0].endswith('a')]
        return np.median(levels) if levels else None

    def stock_kinds(rpm):
        """The stock row nearest rpm: which of its columns play the off-throttle sound."""
        r = int(np.argmin(np.abs(np.log(np.array(lrpms) / rpm))))
        return ['off' if os.path.splitext(c['file'])[0].endswith('c') else 'on' for c in lcells[r * len(lloads):(r + 1) * len(lloads)]]

    # The rows: the stock's rows below RIDING_FROM_RPM (the idle's, from IDLE_BELOW_RPM up riding ones at
    # the stock's RPMs), then riding rows RIDING_STEP apart up to TOP_RPM.
    stock_idle = [rpm for rpm in rpms if rpm < RIDING_FROM_RPM]
    grid = [min(rpm for rpm in rpms if rpm >= RIDING_FROM_RPM)]
    while grid[-1] * RIDING_STEP < TOP_RPM * 1.001:
        grid.append(grid[-1] * RIDING_STEP)
    table = stock_idle + grid
    idling = lambda rpm: rpm < min(IDLE_BELOW_RPM, RIDING_FROM_RPM)   # (a row that plays the idle)

    # Loudness: riding rows along a curve through the stock riding rows' median at their middle RPM;
    # the idle at the stock's level, turned so its top row meets the first riding row.
    riding_stock = [rpm for rpm in lrpms if rpm >= RIDING_FROM_RPM]
    middle_level = np.median([stock_level(rpm) for rpm in riding_stock])
    middle_rpm = np.sqrt(riding_stock[0] * riding_stock[-1])
    def curve(rpm):
        rpm = min(rpm, LOUD_TOP_RPM or rpm)
        return middle_level * 10.0 ** (RISE_DB_PER_OCTAVE * np.log2(rpm / middle_rpm) / 20.0)
    idle_turn = curve(grid[0]) / stock_level(stock_idle[-1])

    folder = 'mxsr_' + ENGINE
    made = {}     # (row, kind) -> (name, cycles, samples, note)
    cell_of = {}  # (row, column) -> key into made
    lines = (['SOUNDTABLE %d %d' % (len(table), len(loads))] + ['%d' % round(rpm) for rpm in table] +
             [' '.join('%g' % l for l in loads)])
    riding_rows = [(r, rpm) for r, rpm in enumerate(table) if not idling(rpm)]
    riding = averaged_cells(fir, riding_rows)
    if LIMITER_MADE_WITH is not None and LIMITER_MADE_WITH != PITCH:
        # The limiter's rows as they were made, lined up (row on row) with the riding at LIMITER_MADE_WITH.
        kept = averaged_cells(fir, riding_rows, riding_pitch=LIMITER_MADE_WITH)
        riding.update({key: cell for key, cell in kept.items() if table[key[0]] >= PITCH_RAMP[1]})
    for r, rpm in enumerate(table):
        kinds = ['off'] + ['on'] * (len(loads) - 1) if idling(rpm) else stock_kinds(rpm)
        for column, kind in enumerate(kinds):
            key = (r, kind)
            if key not in made:
                if idling(rpm):
                    # Idle: the loop, as many pulses as revolutions at IDLE_GAME_RPM (the game plays
                    # each over a revolution, whatever the row: at IDLE_GAME_RPM, as recorded), each
                    # pulse's ends on zero crossings.
                    count = loop_revs
                    y = to_length(loop, count * pulse_length(rpm))
                    y = zero_ends(warp_to_zero(y, count), count)
                    level = stock_level(rpm) * idle_turn * (IDLE_OFF_GAIN if kind == 'off' else 1.0)
                    note = 'idle loop x%d' % count
                elif kind == 'on' and rpm >= LIMITER_ROWS_FROM_RPM and not LIMITER_STEADY:
                    y, count = runs_cell(lfir, limiter_runs, rpm)
                    level = curve(rpm)
                    note = 'limiter loop x%d' % count
                else:
                    y, count, note = riding[(r, kind)]
                    y = tone(y, TONE_ON if kind == 'on' else TONE_OFF)
                    if rpm < PITCH_RAMP[1]:
                        y = tone(y, RIDING_BRIGHTEN)   # (the riding's rows, not the limiter's)
                    level = curve(rpm) * (1.0 if kind == 'on' else OFF_GAIN)
                y = limit(y, IDLE_CREST if idling(rpm) else CREST)
                y = y * level / (np.sqrt(np.mean(y ** 2)) + 1e-9)
                name = '%s/%s-%05d-%s-%03d.sw' % (folder, ENGINES[ENGINE]['CELL'], int(round(rpm)), kind, count)
                made[key] = (name, count, y, note)
            cell_of[(r, column)] = key
        lines += ['-1 %d @%s' % (made[cell_of[(r, c)]][1], made[cell_of[(r, c)]][0]) for c in range(len(kinds))]
    # The whole pack turned down together where its loudest cell would pass full scale.
    peak = max(np.abs(y).max() for _, _, y, _ in made.values())
    overall = min(1.0, 32000.0 / peak, 10.0 ** (GAIN_DB / 20.0) if GAIN_DB is not None else 1.0)
    pack_files = []
    for key, (name, count, y, note) in made.items():
        made[key] = (name, count, y * overall, note)
        pack_files.append((name, to_sw(y * overall)))
    for r, rpm in enumerate(table):
        print('  %5.0f rpm  on: %-26s off: %s' % (rpm, made[(r, 'on')][3], made[(r, 'off')][3] if (r, 'off') in made else '-'))
    print('  the pack turned %+.1f dB overall (its loudest cell to full scale)' % (20 * np.log10(overall)))
    soundtable = ('\n'.join(lines) + '\n').encode('latin1')
    archive = [(ENGINE + '.soundtable', soundtable)] + pack_files
    pack_path = os.path.join(out_dir, 'MXS_REMASTERED_engine_%s.pack' % ENGINE)
    saf.write(pack_path, archive)
    print('%s: %d rows, %d recordings -> %s (%.1f MB)' % (
        ENGINE, len(table), len(pack_files), pack_path, sum(len(d) for _, d in archive) / 1e6))

    # The sweep: idling, revving up on the throttle to the top, off the throttle back down, idling.
    def cells_of(row, column):
        _, count, y, _ = made[cell_of[(row, column)]]
        return y, count
    low, top = table[0] * 1.05, TOP_RPM * 0.96
    def rpm_at(t):
        if t < 1.5:
            return low
        if t < 7.5:
            return low * (top / low) ** ((t - 1.5) / 6.0)
        if t < 8.5:
            return top
        if t < 13.5:
            return top * (low / top) ** ((t - 8.5) / 5.0)
        return low
    def load_at(t):
        return 0.05 if t < 1.4 or t >= 8.5 else 0.95
    sweep = play(table, cells_of, rpm_at, load_at, 15.0)
    with wave.open(os.path.join(out_dir, 'sweep_%s.wav' % ENGINE), 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(to_sw(sweep * min(1.0, 32000.0 / (np.abs(sweep).max() + 1e-9))))


if __name__ == '__main__':
    main()
