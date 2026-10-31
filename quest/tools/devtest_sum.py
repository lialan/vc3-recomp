#!/usr/bin/env python3
"""devtest_sum.py <log>...: one line per run of quest/devtest.sh -- the
frame rates, frame-time spread, headset repeats, GPU and executor time for
the mission intro and for combat.

The intro is the seconds with over 10000 fixed-function draws and a frame
rate under 50 (or, on a fast build, the first 15 s of such draws); combat is
the last 20 seconds of the run."""
import re, sys


def nums(pattern, lines):
    out = []
    for l in lines:
        m = re.search(pattern, l)
        if m:
            out.append(m.groups())
    return out


def summary(path):
    lines = [l for l in open(path, errors='replace') if ' vc3 ' in l or '[' in l]
    fps = [float(g[0]) for g in nums(r'\[FPS\] ([\d.]+)', lines)]
    pace = [(float(a), float(b)) for a, b in nums(r'\[pace\] game \d+ frames: ms p50 ([\d.]+) p95 ([\d.]+)', lines)]
    rep = [tuple(int(x) for x in g) for g in
           nums(r'\[pace\] headset \d+ frames: game frames shown 1x (\d+), 2x (\d+), 3x (\d+), 4\+x (\d+)', lines)]
    gpu = [(float(a), int(b)) for a, b in nums(r'\[GPUTIME\] ([\d.]+) ms/frame over \d+ frames \(fixed-function (\d+) draws', lines)]
    walk = [float(g[0]) for g in nums(r'\[PROF\] walk (\d+) ms', lines)]
    fence = [float(g[0]) for g in nums(r'\[fence\] \d+ waits/s, \d+ slept, ([\d.]+) ms waiting', lines)]
    gcpu = [float(g[0]) for g in nums(r'\[THREADS\].*guest main cpu (\d+)', lines)]
    gobj = [float(g[0]) for g in nums(r'\[THREADS\].*guest main cpu \d+ obj (\d+)', lines)]
    ecpu = [float(g[0]) for g in nums(r'\[THREADS\].*nv2a exec cpu (\d+)', lines)]
    kick = [float(g[0]) for g in nums(r'\[THREADS\].*waited <= ([\d.]+) ms', lines)]
    late = [tuple(float(x) for x in g) for g in nums(r'\[FRAMES\] (\d+) flips, (\d+) late \| on time: arrived ([\d.-]+) ms after the vblank, executed in ([\d.-]+) \| late: arrived ([\d.-]+), executed in ([\d.-]+)', lines)]
    intro_gpu = [g for g, d in gpu if d >= 600]
    combat = slice(-20, None)

    def avg(x):
        return sum(x) / len(x) if x else float('nan')
    intro_fps = sorted(fps[20:60])[:10] if len(fps) > 30 else []
    r = rep[combat]
    shown = [sum(x) for x in zip(*r)] if r else [0, 0, 0, 0]
    tot = sum(shown) or 1
    return ('%-24s fps intro-low10 %.1f combat %.1f min %.1f | pace combat p50 %.1f p95 %.1f ms | '
            'shown 1x/2x/3x/4x %d/%d/%d/%d%% | GPU intro %.1f combat %.1f ms | walk %.0f ms/s | '
            'fence wait %.2f ms/s | game thread cpu %.0f blocked %.0f, executor cpu %.0f,'
            ' kick-off wait <= %.0f ms/s | late flips %.0f/s: arrived %.1f ms after the vblank,'
            ' executed in %.1f (on time: %.1f, %.1f)' % (
                path.split('/')[-1], avg(intro_fps), avg(fps[combat]), min(fps[combat]) if fps else 0,
                avg([a for a, b in pace[combat]]), avg([b for a, b in pace[combat]]),
                100 * shown[0] / tot, 100 * shown[1] / tot, 100 * shown[2] / tot, 100 * shown[3] / tot,
                avg(intro_gpu), avg([g for g, d in gpu[combat]]), avg(walk[combat]),
                avg(fence), avg(gcpu[combat]), avg(gobj[combat]), avg(ecpu[combat]),
                avg(kick[combat]), avg([x[1] for x in late[combat]]),
                avg([x[4] for x in late[combat] if x[1]]), avg([x[5] for x in late[combat] if x[1]]),
                avg([x[2] for x in late[combat]]), avg([x[3] for x in late[combat]])))


for p in sys.argv[1:]:
    print(summary(p))
