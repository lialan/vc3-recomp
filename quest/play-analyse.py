#!/usr/bin/env python3
"""quest/play-analyse.py <playrec-dir> [--all] [--thresh n]: the E.S.
stretches of a quest/play-record.sh recording, frame by frame, both eyes.

The clock: each segment's start is a "PLAYREC segment <k> start" line in
logcat.txt, on the same clock as the game's lines. E.S. is where the game
logged qualifying effect-target batches ("[FX] <n> frames: <m> qualifying
batches", once a second with VC3_GL_EFFECT_LOG=1): those seconds, merged,
padded by a second each side (--all: every frame instead).

Each segment's frames in those stretches are cut out with ffmpeg (516 wide,
both eyes side by side) and compared, per eye, on a grid of samples:
  spike: a frame unlike both neighbours while they are like each other --
         min(d(prev), d(next)) - d(prev, next), mean absolute 0-255;
  eyes:  the two eyes unlike each other, beyond the stretch's usual
         (wrong content in one eye).
A frame is flagged when its spike in either eye, or its eyes figure, is
over --thresh (default 6). Flagged frames are saved as PNG beside their
neighbours in <dir>/flagged/ (<segment>_<frame>_<time>.png: prev | frame |
next, stacked); report.txt lists every E.S. frame's figures. The cut-out
frames are deleted as each segment is done.
"""
import os, re, shutil, struct, subprocess, sys, tempfile
from datetime import datetime

args = [a for a in sys.argv[1:] if not a.startswith('--')]
ALL = '--all' in sys.argv
TH = 6.0
if '--thresh' in sys.argv:
    TH = float(sys.argv[sys.argv.index('--thresh') + 1])
    args = [a for a in args if a != sys.argv[sys.argv.index('--thresh') + 1]]
D = args[0]
STEP = 4


def ts(line):
    m = re.match(r'(\d\d-\d\d \d\d:\d\d:\d\d\.\d+)', line)
    if not m:
        return None
    return datetime.strptime('2000-' + m.group(1), '%Y-%m-%d %H:%M:%S.%f').timestamp()


seg_start, es_sec = {}, []
for line in open(os.path.join(D, 'logcat.txt'), errors='replace'):
    m = re.search(r'PLAYREC.*segment (\d+) start', line)
    if m and ts(line):
        seg_start[int(m.group(1))] = ts(line)
    m = re.search(r'\[FX\] \d+ frames: (\d+) qualifying', line)
    if m and int(m.group(1)) > 0 and ts(line):
        es_sec.append(ts(line))
# E.S. stretches: a second before each [FX] line (it covers the 60 frames
# before it), merged, padded a second each side
spans = []
for t in sorted(es_sec):
    a, b = t - 2.0, t + 1.0
    if spans and a <= spans[-1][1]:
        spans[-1][1] = max(spans[-1][1], b)
    else:
        spans.append([a, b])
print('segments: %d; E.S. stretches: %s' % (len(seg_start), ', '.join(
    '%.1f s' % (b - a) for a, b in spans) or 'none'))
if not spans and not ALL:
    print('no E.S. in the log (VC3_GL_EFFECT_LOG=1 in env.txt?); --all checks every frame')
    sys.exit(0)


def load(p):
    d = open(p, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0] // 8
    row = (w * bpp + 3) & ~3
    h = abs(h)
    eyes = ([], [])
    for y in range(0, h, STEP):
        base = off + y * row
        for x in range(0, w, STEP):
            i = base + x * bpp
            eyes[0 if x < w // 2 else 1].append((d[i] + d[i + 1] + d[i + 2]) // 3)
    return eyes


def dist(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


flagdir = os.path.join(D, 'flagged')
os.makedirs(flagdir, exist_ok=True)
rep = open(os.path.join(D, 'report.txt'), 'w')
rep.write('segment frame time(s) Lmean Rmean spikeL spikeR eyes flag\n')
nflag = ntot = 0
for k in sorted(seg_start):
    mp4 = os.path.join(D, 'seg_%d.mp4' % k)
    if not os.path.exists(mp4):
        continue
    t0 = seg_start[k]
    tmp = tempfile.mkdtemp(prefix='playan_')
    r = subprocess.run(['ffmpeg', '-loglevel', 'info', '-i', mp4, '-vf', 'scale=516:-2,showinfo',
                        '-fps_mode', 'passthrough', os.path.join(tmp, 'f%05d.bmp')],
                       capture_output=True, text=True)
    pts = [float(x) for x in re.findall(r'pts_time:([\d.]+)', r.stderr)]
    names = sorted(os.listdir(tmp))
    frames = []                         # (name, abs time)
    for n, p in zip(names, pts):
        t = t0 + p
        if ALL or any(a <= t <= b for a, b in spans):
            frames.append((n, t))
        else:
            os.remove(os.path.join(tmp, n))
    vec = {n: load(os.path.join(tmp, n)) for n, _ in frames}
    eyes_all = sorted(dist(v[0], v[1]) for v in vec.values())
    eyes_med = eyes_all[len(eyes_all) // 2] if eyes_all else 0
    for i, (n, t) in enumerate(frames):
        v = vec[n]
        sp = [0.0, 0.0]
        if 0 < i < len(frames) - 1:
            p, q = vec[frames[i - 1][0]], vec[frames[i + 1][0]]
            for e in (0, 1):
                sp[e] = min(dist(v[e], p[e]), dist(v[e], q[e])) - dist(p[e], q[e])
        ey = dist(v[0], v[1]) - eyes_med
        flag = max(sp) > TH or ey > TH
        ntot += 1
        rep.write('%d %s %.2f %.1f %.1f %.1f %.1f %.1f %s\n' % (
            k, n, t - t0, sum(v[0]) / len(v[0]), sum(v[1]) / len(v[1]), sp[0], sp[1], ey,
            'FLAG' if flag else ''))
        if flag:
            nflag += 1
            trio = [frames[j][0] for j in (i - 1, i, i + 1) if 0 <= j < len(frames)]
            out = os.path.join(flagdir, 'seg%d_%s_%.2fs.png' % (k, n[:-4], t - t0))
            pngs = []
            for j, f in enumerate(trio):
                pj = os.path.join(tmp, 'x%d.png' % j)
                subprocess.run(['sips', '-s', 'format', 'png', os.path.join(tmp, f), '--out', pj],
                               capture_output=True)
                pngs.append(pj)
            subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + sum([['-i', x] for x in pngs], [])
                           + ['-filter_complex', 'vstack=inputs=%d' % len(pngs), out],
                           capture_output=True)
    shutil.rmtree(tmp)
    print('segment %d: %d E.S. frames checked' % (k, len(frames)))
rep.close()
print('%d frames, %d flagged (threshold %.1f): %s, report.txt' % (ntot, nflag, TH, flagdir))
