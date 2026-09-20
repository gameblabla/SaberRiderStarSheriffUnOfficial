#!/usr/bin/env python3
"""Run the original Linux demo under Xvfb, record it, and feed scripted keys.
usage: orig_run.py out.mp4 total_seconds "t:key,t:key..."  (key = X keysym names: Return, space, Right, x, z, ...; 'key+' = press, 'key-' = release)
AUDIO=1 also records the game's sound into the mp4: it plays into a PulseAudio/PipeWire null sink "saber_cap"
(pactl load-module module-null-sink sink_name=saber_cap) whose monitor ffmpeg captures; the game's stream is moved
there with pactl, so the desktop stays quiet."""
import sys, os, subprocess, time, threading, re
out, total, script = sys.argv[1], float(sys.argv[2]), (sys.argv[3] if len(sys.argv) > 3 else "")
audio = os.environ.get("AUDIO") == "1"
xv = subprocess.Popen(["Xvfb", ":99", "-screen", "0", "852x480x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
env = dict(os.environ, DISPLAY=":99", SDL_VIDEODRIVER="x11"); env.pop("WAYLAND_DISPLAY", None)
if audio:
    if "saber_cap" not in subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True, text=True).stdout:
        subprocess.run(["pactl", "load-module", "module-null-sink", "sink_name=saber_cap"], stdout=subprocess.DEVNULL)
    env.update(SDL_AUDIODRIVER="pulseaudio", SDL_AUDIO_DRIVER="pulseaudio", PULSE_SINK="saber_cap")
else:
    env.update(SDL_AUDIODRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
rec = subprocess.Popen(["ffmpeg", "-loglevel", "error", "-y", "-f", "x11grab", "-framerate", "15", "-video_size", "852x480", "-i", ":99"]
                       + (["-f", "pulse", "-i", "saber_cap.monitor", "-c:a", "aac"] if audio else [])
                       + ["-c:v", "libx264", "-preset", "ultrafast", "-crf", "23", out], env=env)
cmd = os.environ.get("RUN_CMD", "./SaberRider.linux64").split(); cwd = os.environ.get("RUN_CWD", "/home/anonymous/Documents/DEV/Saber_Rider/SaberRider")
game = subprocess.Popen(cmd, cwd=cwd, env=env,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def mover():   # SDL3 ignores PULSE_SINK under PipeWire: move the game's sink inputs to the capture sink
    moved = set()
    while game.poll() is None:
        time.sleep(0.2)
        for blk in subprocess.run(["pactl", "list", "sink-inputs"], capture_output=True, text=True).stdout.split("Sink Input #")[1:]:
            sid = blk.split("\n", 1)[0].strip(); m = re.search(r'application.process.id = "(\d+)"', blk)
            if m and int(m.group(1)) == game.pid and sid not in moved:
                subprocess.run(["pactl", "move-sink-input", sid, "saber_cap"]); moved.add(sid)
if audio: threading.Thread(target=mover, daemon=True).start()
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
