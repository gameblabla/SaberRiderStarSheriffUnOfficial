# PC Engine bosses, racing and presentation — 2026-10-04

Continues the stampede work recorded in
`../codex-session-01a1073a-2e9b-77a3-880c-c71cf5f77c57.md`.

The level 1/5 gunship now uses its original 208×112 art, anchored at (107,46).
Levels 3/4 use the original 130×108 Hyperjumper side view, anchored at (65,54).
Dedicated 32×32 VDC sprite cells preserve the complete art while leaving SAT
entries for the hero, HUD and lasers. The hull has its own reserved pattern
pages and palette, released when the encounter ends. The ordinary sprite
cache still serves Dark April and the other actors.

Boss one's main cannon uses source offsets (-95,50)/(90,50); its rider uses
(-85,-25)/(80,-25) for horizontal fire and (-75,10)/(70,10) for downward
fire. Both fire every 12 simulation steps, with the rider's source pause
between steps 160 and 184 retained. Lasers use the source beam art, baked for
horizontal, diagonal and vertical trajectories. Their speeds are Q8 1422
straight and 996 diagonally, matching 333.333 pixels/second and the source's
0.7 diagonal component. The old eight-enemy-shot cap is removed. Boss HP is
no longer drawn in the platform, racing or space HUDs.

The level 1 security cameras were spawned at x=6160/7504 before entering the
view. The generic offscreen actor check deleted them immediately. Static
props now survive their approach from the right; cameras play both source
frames, every 15 simulation steps, with their fixed source facing.

Stage completion loads the original victory painting for that stage and hero,
with the source MISSION / ACCOMPLISHED lettering. All 19 painting variants
are exported as 320×224 UI backgrounds. I/II/Run continues to the next stage;
stage 7 continues to credits. The new screen uses its own BAT, palettes and
VRAM rather than writing text into gameplay tiles.

World rumble is reset before platform dialogue submits its unshifted sprites
and background panel. The scroll remains zero throughout the frozen dialogue.
The dialogue regression explicitly reopens a panel from a rumble offset of
three pixels, covering the warning shown in the supplied `.mc0` state.

Horse spacing is 224 pixels instead of 256. The count, full-size art,
120-pixel/second galloping speed and animation timing remain the same.
The convoy's inter-horse range is 12.5% shorter. A 192-pixel trial failed
complete-sprite admission and is not used.

The race uses its existing baked perspective car sizes through the cached
sprite emitter. It redraws cars/HUD when a complete road snapshot is ready,
instead of repeatedly projecting against an unchanged road. Rival decisions
run every four simulation steps, with motion and timers adjusted for that
interval; player control and projectile simulation keep their elapsed-video-
frame timing. Road sampling resolution is unchanged.

In the seeded five-second race measurement, the live seven-car field improved
from 34 completed road images to 61 (6.8 to 12.2 images/second, about 79%
more). With the field removed, the counts were 63 and 74. This is an emulator
measurement of that scenario, not a claim of 60 Hz race rendering.

Verification artifacts:

- `build/pce/revision-boss-check.log`: natural early camera spawn, both camera
  frames cached, sustained level 1/3/4 bosses without essential sprite
  overflow, 320×224 hardware timing, and victory advancing to stage 5.
- `build/pce/revision-boss{1,3,4}.png`, `revision-camera{0,1}.png`,
  `revision-victory.png`: screenshots from the rebuilt native disc.
- `build/pce/herd-visibility-verification.json`: 1,024 draws over all four
  heroes and 256 horizontal phases; complete horses, fixed HUD and a maximum
  of 15 scanline units on the real 16-unit limit.
- `tools/pce/profile_race.py`: reproducible road-update profiling with and
  without the seven rivals; cycle dumps and screenshots are retained.

Run the rebuilt `build/pce/saber_rider.cue` from boot or an in-game checkpoint.
An emulator state restores its saved program RAM, so loading the old `.mc0`
restores the old executable as well. Physical-console behavior has not been
measured.

Final verification: `test-campaign` (including the native boss/camera/victory
checks), `test-presentation`, `test-herd` and `test-audio` all exit successfully
on the final disc. The native-disc regression also passed. All three shortened
convoys present 300 new frames over 300 video frames with held firing, zero
essential overflows and no firing-time pattern uploads. The camera screenshots
show distinct indicator lights, and the dialogue test passes after reopening
from a nonzero rumble offset. Stage initialization now resets the hero pose ID
before the next stage's opening dialogue, preventing use of an old stage's pose.
