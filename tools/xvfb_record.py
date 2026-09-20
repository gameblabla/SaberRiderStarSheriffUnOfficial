#!/usr/bin/env python3
"""Run the original Linux demo under Xvfb, record it, and feed scripted keys.
usage: orig_run.py out.mp4 total_seconds "t:key,t:key..."  (key = X keysym names: Return, space, Right, x, z, ...; 'key+' = press, 'key-' = release)"""
import sys, os, subprocess, time
out, total, script = sys.argv[1], float(sys.argv[2]), (sys.argv[3] if len(sys.argv) > 3 else "")
xv = subprocess.Popen(["Xvfb", ":99", "-screen", "0", "852x480x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
env = dict(os.environ, DISPLAY=":99", SDL_AUDIODRIVER="dummy", SDL_VIDEODRIVER="x11"); env.pop("WAYLAND_DISPLAY", None)
rec = subprocess.Popen(["ffmpeg", "-loglevel", "error", "-y", "-f", "x11grab", "-framerate", "15", "-video_size", "852x480", "-i", ":99",
                        "-c:v", "libx264", "-preset", "ultrafast", "-crf", "23", out], env=env)
cmd = os.environ.get("RUN_CMD", "./SaberRider.linux64").split(); cwd = os.environ.get("RUN_CWD", "/home/anonymous/Documents/DEV/Saber_Rider/SaberRider")
game = subprocess.Popen(cmd, cwd=cwd, env=env,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
from Xlib import display, X, XK
from Xlib.ext import xtest
d = display.Display(":99")
def key(name, down):
    ks = XK.string_to_keysym(name); kc = d.keysym_to_keycode(ks)
    xtest.fake_input(d, X.KeyPress if down else X.KeyRelease, kc); d.sync()
events = []
for item in script.split(","):
    if not item.strip(): continue
    t, k = item.split(":"); t = float(t)
    if k.endswith("+"): events.append((t, k[:-1], True))
    elif k.endswith("-"): events.append((t, k[:-1], False))
    else: events.append((t, k, True)); events.append((t + 0.08, k, False))
events.sort()
t0 = time.time()
for t, k, dn in events:
    while time.time() - t0 < t: time.sleep(0.01)
    key(k, dn)
while time.time() - t0 < total: time.sleep(0.1)
game.terminate(); rec.terminate(); rec.wait(); xv.terminate()
