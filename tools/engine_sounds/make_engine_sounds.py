"""MXS Remastered engine sound packs.

MX Simulator plays a bike's engine from a soundtable: a grid of RPMs by throttle levels, each cell a
recording (.sw: raw signed 16-bit mono) of the engine's cycle there. Almost every cell holds a single
cycle, looped, which is why the engines sound bland: every cycle is exactly the same. The format takes
more ("-1 <cycles> @file", then the order to play them in); only the stock idle loops use it.

This makes, for each stock engine, a pack whose soundtable gives every single-cycle cell 16 cycles
instead: the stock recording, varied cycle by cycle the way a real engine varies (how hard each one
fires, its timing, the rasp of the exhaust, the intake's hiss, the odd pop off the throttle), with
the cycles' starts and ends left exactly as recorded so any of them follows any other seamlessly.
Cells that already have many cycles (the idle loops) are kept as they are.

The packs override the stock soundtables by name (default450f.soundtable, ...) from an archive of
their own; no stock file is touched. The mod's panel (OTHER tab, SOUND MODS) installs and removes
them.

    python make_engine_sounds.py "C:\\Games\\MX Simulator" [out_dir]

The 250 four-stroke, 250 two-stroke and 125 two-stroke packs are made from recordings instead:
make_250f.py (a sound mod's), make_250.py (a real 250 two-stroke's; --engine default125 for the 125).

Writes <out_dir>/MXS_REMASTERED_engine_<engine>.pack (a .saf, named so the game never loads it by itself)
and preview_<engine>.wav: each RPM's full-throttle sound, first stock, then the pack's, to listen to.
"""
import glob
import os
import sys
import wave

import numpy as np
from scipy import signal

import saf

ENGINES = {  # stock soundtable -> what it is (for the logs and previews)
    'default450f': '450 four-stroke',
}  # (default250f: make_250f.py; default250 and default125: make_250.py)
CYCLES = 16          # cycles in each new patch
SEQUENCE = 128       # how many of them the play order lists before it repeats
SEED = 20261107      # the same packs every time it's run


# ---- The stock files --------------------------------------------------------------------------

def stock_files(game_dir):
    """{name: bytes} of every stock engine soundtable, .braap and .sw in the game's archives (the
    first archive alphabetically that has a name wins, as the game's own patches are named)."""
    files = {}
    for path in sorted(glob.glob(os.path.join(game_dir, '*.saf'))):
        if os.path.basename(path).lower().startswith(('mxsr_', 'mxs_remastered_')):
            continue  # (a pack of ours, installed: not stock)
        want =lambda n: any(n == e + '.soundtable' or n.startswith(e + '/') for e in ENGINES) and n not in files
        try:
            files.update(saf.extract(path, want))
        except ValueError:
            pass  # (not an archive we can read: skipped)
    return files


def parse_soundtable(text):
    """(rpms, loads, cells): cells in the file's order (RPM by RPM, each load in turn), each
    {'lead': its first number, 'cycles': n, 'file': name without the @, 'order': [ints]}."""
    tokens = text.split()
    at = tokens.index('SOUNDTABLE')
    rows, cols = int(tokens[at + 1]), int(tokens[at + 2])
    i = at + 3
    rpms = [float(t) for t in tokens[i:i + rows]]
    i += rows
    loads = [float(t) for t in tokens[i:i + cols]]
    i += cols
    cells = []
    while len(cells) < rows * cols:
        lead, cycles, name = tokens[i], int(tokens[i + 1]), tokens[i + 2]
        i += 3
        # Its play order, if it has one: the whole numbers up to the next cell (which starts "-1").
        order = []
        while i < len(tokens) and tokens[i].isdigit():
            order.append(int(tokens[i]))
            i += 1
        cells.append({'lead': lead, 'cycles': cycles, 'file': name.lstrip('@'), 'order': order})
    return rpms, loads, cells


# ---- Varying a cycle ----------------------------------------------------------------------------

def edge_window(length, fraction=0.08):
    """1 through the middle, easing to 0 over the first and last fraction: the variations stay off
    the cycle's ends, so every varied cycle starts and ends exactly as the recording does."""
    w = np.ones(length)
    n = max(8, int(length * fraction))
    ramp = 0.5 - 0.5 * np.cos(np.linspace(0.0, np.pi, n))
    w[:n] = ramp
    w[-n:] = ramp[::-1]
    return w


def looped_filter(b, a, x):
    """A filter run over the cycle as the loop it is (three times round, the middle kept)."""
    y = signal.lfilter(b, a, np.concatenate([x, x, x]))
    return y[len(x):2 * len(x)]


def random_walk(rng, n, keep):
    """n values around 0 (spread about 1), each mostly the last one: cycles drift, they don't jump."""
    out, v = [], 0.0
    for _ in range(n):
        v = keep * v + np.sqrt(1.0 - keep * keep) * rng.standard_normal()
        out.append(v)
    return np.array(out)


class Variation:
    """How each of the CYCLES cycles differs, the same in every cell of an engine's soundtable. The
    game plays neighbouring cells (the RPMs either side, two throttle levels) together, blended, all
    on the same cycle of the play order: given each cell variations of its own (and an order of its
    own), cycle 3 of one fired early while cycle 9 of the next fired late, the two firings no longer
    lined up and the blend stuttered. Shared, they all fire alike at the same moment."""

    def __init__(self, rng, engine):
        self.fire = random_walk(rng, CYCLES, 0.55)    # how hard each cycle fires
        self.rasp = random_walk(rng, CYCLES, 0.45)    # how bright the exhaust is
        self.timing = random_walk(rng, CYCLES, 0.30)  # the firing a touch early or late
        # (Each centred on nothing: over 16 cycles the draws leaned dark or soft, and the pack with
        # them. Varied around the recording, it averages to the recording.)
        for walk in (self.fire, self.rasp, self.timing):
            walk -= walk.mean()
        self.pop = [rng.random() < 0.10 for _ in range(CYCLES)]  # off the throttle: this one pops
        self.pop_size = [0.9 + 0.6 * rng.random() for _ in range(CYCLES)]
        self.noise_seeds = [[SEED, sum(map(ord, engine)), k] for k in range(CYCLES)]
        self.order = play_order(rng)


def vary(cycle, shared, strength, closed_throttle, rasp_scale=1.0):
    """CYCLES versions of one recorded cycle, each a little different (as shared says)."""
    length = len(cycle)
    t = np.arange(length, dtype=np.float64)
    w = edge_window(length)
    rms = np.sqrt(np.mean(cycle * cycle)) + 1e-9
    # Its low end (the firing's thump) and the rest (the exhaust's rasp, the mechanical noise).
    b, a = signal.butter(2, 0.06)
    low = looped_filter(b, a, cycle)
    high = cycle - low
    # How loud it is through the cycle (where the firing is), 0..1.
    envelope = looped_filter(*signal.butter(1, 0.02), np.abs(cycle))
    envelope = np.clip(envelope / (envelope.max() + 1e-9), 0.0, 1.0)

    # Where the cycle is near its peak (the firing itself) it's left as recorded: varied there, a
    # cycle came out louder than the recording, the whole patch had to be turned down to fit, and
    # the engine sounded quieter and duller. (0 at the peak, 1 where it's well under it.)
    peak = np.abs(cycle).max() + 1e-9
    near = signal.convolve(np.abs(np.concatenate([cycle[-16:], cycle, cycle[:16]])), np.ones(33) / 33.0, mode='valid')
    near = np.maximum(near, np.abs(cycle))
    calm = np.clip((0.8 * peak - near) / (0.3 * peak), 0.0, 1.0)
    # (Eased in and out over a few percent of the cycle: switched sharply, it was a fast change of
    # level, heard as a faint crackle on the two-strokes, whose recordings have almost no top end.)
    ease = np.hanning(max(9, int(len(cycle) * 0.06)) | 1)
    calm = signal.convolve(np.concatenate([calm[-len(ease):], calm, calm[:len(ease)]]), ease / ease.sum(), mode='same')[len(ease):-len(ease)]
    fire, rasp, timing = shared.fire, shared.rasp, shared.timing
    inner = []
    for k in range(CYCLES):
        # Timing: the inside of the cycle shifted by up to ~0.5%, its ends where they are.
        warp = t + timing[k] * 0.005 * strength * length * np.sin(np.pi * t / length)
        # (Only the thump is moved. Read between samples, the rasp lost its top end: the shift is a
        # few samples, and interpolating that much dulled every patch.)
        lo = np.interp(warp, t, low)
        hi = high
        # (How hard it fires stays within ~15%: the stock recordings are already at full scale, and
        # every cycle of a patch is turned down to fit its loudest one.)
        y = lo * (1.0 + 0.10 * strength * np.clip(fire[k], -1.5, 1.5) * w * calm) +             hi * (1.0 + 0.20 * rasp_scale * strength * np.clip(rasp[k], -1.5, 1.5) * w * calm)
        # (Nothing added that the recording doesn't have: noise for the intake's hiss, and pops off
        # the throttle, crackled. The stock two-stroke recordings have next to nothing above 4 kHz,
        # and the hiss put thousands of times more there.)
        inner.append(y)
    # The ends exactly as recorded (any cycle follows any other with no click).
    out = [cycle * (1.0 - w) + y * w for y in inner]
    # Never louder than the recording's own peak (the stock ones already reach full scale). All the
    # cycles turned down together just enough, rather than their peaks squashed: a limiter on 1-5%
    # of the samples was distortion, heard as crackling. (Together, so they still all meet.)
    headroom = np.abs(cycle).max() / (max(np.abs(y).max() for y in out) + 1e-9)
    return [y * min(1.0, headroom) for y in out]




def play_order(rng):
    """SEQUENCE cycle numbers, no cycle twice in a row."""
    order, last = [], -1
    while len(order) < SEQUENCE:
        k = int(rng.integers(0, CYCLES))
        if k != last:
            order.append(k)
            last = k
    return order


def to_sw(x):
    return np.clip(np.round(x), -32768, 32767).astype('<i2').tobytes()


def from_sw(data):
    return np.frombuffer(data[:len(data) // 2 * 2], '<i2').astype(np.float64)


def make_pack(engine, files, out_dir):
    rng = np.random.default_rng([SEED, sum(map(ord, engine))])
    shared = Variation(rng, engine)
    table = files[engine + '.soundtable'].decode('latin1')
    rpms, loads, cells = parse_soundtable(table)
    folder = 'mxsr_' + engine
    made = {}         # stock file -> (new name, its cycles)
    pack_files = []
    lines = ['SOUNDTABLE %d %d' % (len(rpms), len(loads))]
    lines += ['%d' % r for r in rpms]
    lines.append(' '.join('%g' % l for l in loads))
    for index, cell in enumerate(cells):
        rpm = rpms[index // len(loads)]
        if cell['cycles'] != 1 or cell['file'] not in files:
            # (An idle loop of many cycles already, or a file we don't have: kept as it is.)
            lines.append('%s %d @%s' % (cell['lead'], cell['cycles'], cell['file']))
            if cell['order']:
                lines.append(' '.join(str(o) for o in cell['order']))
            continue
        if cell['file'] not in made:
            cycle = from_sw(files[cell['file']])
            suffix = os.path.splitext(cell['file'])[0][-1]  # a: on the throttle ... c: off it
            closed = suffix == 'c'
            # (Its start left sharp: the recordings start on the firing, and the jump where the loop
            # starts again is the exhaust pulse's leading edge. Eased in, every firing was blunted and
            # the engine sounded muffled.)
            # Low revs vary more (a real engine's cycles do); a two-stroke more than a four.
            strength = {'a': 1.0, 'b': 0.85, 'c': 0.75}.get(suffix, 0.85)
            strength *= 1.25 if rpm < 3500 else 1.0 if rpm < 7000 else 0.8
            strength *= 1.15 if not engine.endswith('f') else 1.0
            versions = vary(cycle, shared, strength, closed)
            name = '%s/%s-%02d-v.sw' % (folder, os.path.splitext(os.path.basename(cell['file']))[0], CYCLES)
            pack_files.append((name, to_sw(np.concatenate(versions))))
            made[cell['file']] = (name, versions)
        name, _ = made[cell['file']]
        lines.append('%s %d @%s' % (cell['lead'], CYCLES, name))
        order = shared.order  # (every cell the same, see Variation)
        for row in range(0, len(order), 32):
            lines.append(' '.join(str(o) for o in order[row:row + 32]))
    soundtable = ('\n'.join(lines) + '\n').encode('latin1')
    archive = [(engine + '.soundtable', soundtable)] + pack_files
    pack_path = os.path.join(out_dir, 'MXS_REMASTERED_engine_%s.pack' % engine)
    saf.write(pack_path, archive)
    write_preview(engine, files, cells, rpms, loads, made, os.path.join(out_dir, 'preview_%s.wav' % engine))
    return pack_path, len(made), sum(len(d) for _, d in archive)


def write_preview(engine, files, cells, rpms, loads, made, path):
    """Each RPM's sound on the throttle (the last load column), then off it (the first): a second of
    the stock cycle looped, then a second of the pack's cycles in turn. (At the recordings' own
    pitch, 44.1 kHz.)"""
    rate = 44100
    out = []
    rng = np.random.default_rng(1)
    for column in (len(loads) - 1, 0):  # on the throttle, then off it
        for r in range(len(rpms)):
            cell = cells[r * len(loads) + column]
            if cell['file'] not in made:
                continue
            stock = from_sw(files[cell['file']])
            _, versions = made[cell['file']]
            reps = max(1, rate // len(versions[0]))
            out.append(np.tile(stock, max(1, rate // len(stock))))
            out.append(np.zeros(rate // 10))
            seq = play_order(rng)[:reps]
            out.append(np.concatenate([versions[k % len(versions)] for k in seq]))
            out.append(np.zeros(rate // 4))
    audio = np.concatenate(out) * 0.8
    with wave.open(path, 'wb') as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(to_sw(audio))

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    game_dir = args[0]
    out_dir = args[1] if len(args) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'MXS_REMASTERED_sounds')
    os.makedirs(out_dir, exist_ok=True)
    files = stock_files(game_dir)
    for engine, what in ENGINES.items():
        if engine + '.soundtable' not in files:
            print("%s (%s): its soundtable isn't in the game's archives, skipped" % (engine, what))
            continue
        path, patched, size = make_pack(engine, files, out_dir)
        print('%s (%s): %d patches made %d cycles long -> %s (%.1f MB)' % (engine, what, patched, CYCLES, path, size / 1e6))


if __name__ == '__main__':
    main()
