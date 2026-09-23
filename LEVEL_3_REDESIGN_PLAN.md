# Level 3 redesign: a desert run-and-gun stage ending in Hyperjumper

Status: implementation plan, not an implemented fix. Audited on 2026-09-22 against `game/` commit `8f8f9a6ad77a50b5429711be2477ac82240ed62e` and the current, locally cleaned assets. Source line numbers below refer to that snapshot; use the named functions after edits move the lines.

The intended result is a complete night-time desert level: run, shoot ordinary enemies, negotiate visible and reachable terrain, reach a quiet approach, enter a sealed arena, and fight Hyperjumper through three distinct health-based phases. The boss must have a visible entrance and readable attacks. It must never attack during the traversal section or appear by teleporting into view.

This document supersedes the “platformer boss arena” description in `README.md` and the older stage-number/design proposals in `../PLAN.md` for Level 3 only. It preserves the requirements in `../codex-session-01a0c6b0-afc2-7bc3-b3e9-5a59ae3a49dd.md`: desert rather than town, dark sky and red moon, supplied Hyperjumper sprites, no Level 1 story dialogue, horses or stampede, and actual art assets for the environment. Debug collision rectangles remain appropriate; they are not shipping terrain art.

## 1. Findings that drive the work

| Finding | Current evidence | Required correction |
| --- | --- | --- |
| Boss begins at the start of the level. | `src/night.c:154–177`: `night_init()` sets `boss_alive=true`, `HYPER_READY` and `state_t=0.75`. `night_update():325–330` starts a sweep when this timer expires. | Initialize traversal only. Activate the boss from a once-only end-of-level entrance trigger. |
| No regular enemies are installed. | `src/game.c:67–88` resets the enemy system and excludes all imported objects when `night_on` is true. No replacement Stage 3 triggers are added. | Install an authored Stage 3 encounter list using the existing humanoid enemies. Keep the Level 1 event exclusion. |
| Collision does not describe the visible level. | `src/night.c:20–28,82–143`: separate hard-coded platforms/pits and copied/reordered visual strips. | Build gameplay art and collision from the same placed terrain definitions. |
| Main ground behaves as a drop-through platform. | `night.c:95,100` writes `4` for both ground and platforms. `character.c:69,364–391` interprets `4` as one-way and down+jump as a drop request. | Use solid ground (`15`), one-way surfaces (`4`) only where intended, and visible empty gaps (`0`). |
| Several mandatory gaps exceed a normal running jump. | Current pits are 130–240 px wide. The four hero CRHC records give run speed `100`, jump velocity `-290`; gravity is `480` in `game.c:57`. | Size mandatory gaps from measured movement, with a landing margin; do not compensate by changing hero physics. |
| Front attacks visibly teleport the boss. | `night.c:342–349` assigns `boss_x=cam_x+sw*0.52` and `boss_y=76` immediately after a side pass. | Fly out, reposition entirely offscreen, then visibly enter the front attack position. |
| Attack states are mistaken for phases. | `night.h:24` has ready/sweep/front/death; `night.c:344` changes attack every three passes, independently of HP. | Separate stage progression, boss health phase, and the current attack state. |
| Some boss collisions bypass normal protections. | `night.c:295–301` does not check `CF_HIT`; `enemies.c:214–225` shows that `player_damage()` itself does not enforce immunity. `night.c:258–277` checks shots against the hero but not terrain. | Respect immunity for every damage source; use visible hull/hurtboxes and terrain-aware projectile collision. |
| Gameplay continues in inappropriate states. | `night.c:313` updates damaging projectiles before the `live` guard. Boss shots survive the start of the death animation. | Explicitly gate combat, and clear attacks on defeat/retry/transition as specified below. Pause is already handled by an earlier return in `game_update()`. |
| Asset filtering is already nearest-neighbor. | `src/gfx.c:36–42`; integer presentation in `src/main.c:42`. | Fix native asset size, pixel clusters, palette and placements; a filter switch alone will not fix the art. |
| Scenery is stretched and too detailed. | `night.c:413–428` draws the 349×512 cactus at 64×150 and the 491×353 scrub at 86×105. Current generated props have roughly 68,000–92,000 unique RGBA colors each. | Produce reviewed native-resolution variants, preserve proportions, and draw each at its intended size. |

Baseline checks actually performed for this plan:

- `cmake --build build` succeeds; Ninja reports no work to do.
- Headless Level 3 runs succeed. A screenshot at 90 fixed steps shows Hyperjumper attacking at the starting screen.
- A collision-overlay capture at player x=800 shows a green platform cutting across scenery independently of its visible surfaces.
- With `SABER_SCRIPT='30:,1:DJ,49:'`, the stationary hero drops through the starting ground and enters `CS_DEAD`. This is a reproduced bug, not just a suspected input issue.
- Inspected the cleaned cactus/wreck, both Hyperjumper reference screenshots and the old panorama. The old `artifacts/stage3_full.png` predates the current cleaned assets and is not proof of the current level's quality.

No gameplay source or existing assets were changed while preparing this plan. The dirty `assets/april.png` and five cleaned scenery PNGs belong to the user. Preserve them as the input artwork; do not restore their Git versions.

## 2. Level layout and encounter pacing

Keep world width **6200 px**, logical height **240 px**, collision/death extent **256 px**, and the normal floor surface at **y=208**. World y increases downward. Support both 426×240 and 320×240. Gameplay is player-controlled horizontal scrolling, with no forced auto-run.

The world is about 14.6 wide screens or 19.4 narrow screens. At 100 px/s, moving from x=150 to the boss approach already takes roughly 57 seconds without stopping. Target a first successful traversal of **100–150 seconds**, followed by a **60–90 second Normal boss fight**. These are tuning targets to measure, not hard timers.

| World x | Area and terrain | Encounters and lesson | Exit/recovery |
| --- | --- | --- | --- |
| 0–800 | Open badlands; flat solid ground, scattered background rocks. Spawn x=150. Keep the first 250 px clear of hazards. | Two grunts, introduced separately around x=480 and 720. Establish run-and-shoot and clear enemy silhouettes. | Brief clear space before the first raised pad. |
| 800–1760 | Broken causeway. Optional 112 px platform at x=960–1072, top y=176. One visible 64 px pit at x=1248–1312; broad solid landing. | Three grunts and one kneeler, in two groups. Introduce duck/slide on solid ground before asking for a jump. | Last enemy stays at least 96 px beyond the pit's landing edge. |
| 1760–2880 | Canyon shelves. Route remains legible at ground level. Optional shelves at y=176 and y=144, reached by 32 px steps; 64 px pit at x=2288–2352. | Two grunts, one kneeler and one sniper. Teach jumping to gain an angle and using real solid rock as cover. | Safe checkpoint at x=2848 on continuous ground. |
| 2880–4000 | Damaged landing pads. One-way upper route over solid ground, then an 80 px pit at x=3760–3840. No overhead attack during the mandatory jump. | Three grunts and two kneelers in alternating groups, maximum three engaged at once. Practice drop-through versus slide with different, visible surface types. | At least 96 px of safe landing before the next threat. |
| 4000–5120 | Outrider perimeter. Low solid rock cover and an optional elevated firing position; no pits in the last 512 px. | Four grunts, one sniper and one kneeler, staggered. Final pair near x=4912 and 5056. Recombine learned actions without introducing a new enemy type. | All patrol bounds stop at or before x=5120. No mandatory “kill every enemy in the level” gate. |
| 5120–5680 | Quiet approach. Continuous ground, restrained scenery, landing-pad markings leading toward the final arena. | No new enemies. A short, non-damaging engine cue near the end can announce the encounter. No boss actor or shots yet. | Boss checkpoint at x=5600; ample room to release controls and read the arena. |
| 5680–6200 | End landing pad. Flat, clearly bounded fighting space; no pits, low ceilings or decorative foreground obstructions. | Position-gated entrance, camera settles, gates close, visible boss arrival, then three phases. | Boss defeat is the only Level 3 clear condition. |

Initial encounter budget: **21 humanoids** (14 grunts, 5 kneelers, 2 snipers), at most three actively threatening the player. These counts and the optional shelves are first-pass tuning values. Exact spawn coordinates must be finalized against the authored geometry and recorded in the encounter table, not selected at random at runtime.

### Movement constraints

The continuous approximation gives jump height `290²/(2×480) ≈ 87.6 px`, same-height airtime `2×290/480 ≈ 1.21 s`, and running distance `≈121 px`. The actual fixed-step simulation, hero extents and landing rules remain authoritative.

- Start with 64–80 px mandatory gaps; never require more than 88 px without a demonstrated safe replay. Align the initial authored geometry to the 16 px terrain art grid; collision remains 8 px.
- Required rises are 32–48 px. A y=144 shelf above the y=208 floor is optional unless approached through a y=176 step.
- Each pit needs an unobscured takeoff edge, visible landing edge and a clear landing zone at least 96 px long. Avoid enemy fire that forces a blind jump into a body hit.
- Put one-way shelves over solid terrain, not above death gaps. Down+jump there drops to safety; down+jump on the main floor slides.
- Do not create “slide tunnels” unless the physics body supports them. The current animation hurtboxes shrink for sliding, but the terrain physics body is not automatically a crouching silhouette. Sliding under boss fire is a hurtbox interaction and does not require tunnel geometry.

### Enemy placement rules

Reuse types `2/5` for grunts, `6/7` for snipers, `8/9` for kneelers (`enemies.c:131–138`). Use existing enemy sheets, AI, hit reactions and audio. No new flying enemy, cannon or extra miniboss is required for this repair.

Each encounter record should contain: stable ID, activation x-range, spawn anchor, enemy type, facing, initial fire delay, patrol bounds, checkpoint segment, and finite spawn count. Resolve the visual foot anchor to the existing `enemy_spawn()` body convention; that function adds half-extents to its input coordinates (`enemies.c:151`). Do not pass terrain top coordinates as character origins blindly.

Activate enemies before they are fully revealed, with validated ground at their positions; no visible pop-in next to the hero. Give the first attack at least 0.65 seconds after the enemy becomes visible. Add a Stage 3 patrol/ledge policy where required: the existing walker/grunt movement is not evidence of safe ledge-turning. Use stage-specific metadata so Level 1 behavior is preserved.

Route triggers fire once per checkpoint attempt, not continuously while standing in a zone. Sleeping/despawning an enemy must not accidentally count as a kill for any future local gate. Disable route spawns once the boss entrance starts; enemies remaining behind the closed entrance cannot fire into the arena or follow the hero inside.

## 3. Make terrain art and collision agree

Replace `PLATFORMS`, `PITS` and the eight-strip `ORDER` reshuffle with authored terrain placements in a new `src/night_level.c/.h` module. Reuse the original desert tile banks and the cleaned night scenery, but not arbitrary slices of Level 1's composition.

Use a small C data model, with no new runtime JSON dependency:

```c
/* Proposed shape, not existing code. */
typedef struct {
    int x, y;                  /* authored world anchor */
    int prefab_id;             /* complete visual + collision definition */
    int checkpoint_segment;
} Stage3TerrainPlacement;
```

Each prefab owns a complete visual stamp plus its matching 8 px collision mask: solid ground, ground edge, rock step, one-way pad, decorative prop, or arena gate. Both outputs come from the same placement. Use original 16×16 terrain cells/metatiles for walkable geometry, including edge caps and underside/body tiles. Draw no ground tiles over a pit and no continuous flat floor behind its opening in the gameplay plane. Parallax ground can remain only if its depth is unambiguous.

Collision contract:

- `0`: empty; `4`: explicitly one-way; `15`: solid, including main ground and cover. Fill solid ground from its top to the bottom of the collision grid, rather than installing a one-cell one-way row.
- Solidity means left/right/up/down blocking. One-way floors catch downward crossings from above and allow ascent/drop-through. Keep the existing 1 px resting convention unless a regression test justifies changing it.
- Author whole wrecks and rock formations as stamps. Never cut a car or mesa at an arbitrary source-strip boundary. Apply tiling only to tiles designed to tile.
- Every apparent gameplay foothold must have a matching collider. A visual-only cactus, fence or wreck belongs behind the play plane with quieter contrast and appropriate parallax; if promoted to climbable cover, it needs an explicit surface profile. Do not use its entire rectangular image canvas as collision.
- Store a y anchor per prop. Remove the assumption that every prop's base is `216` regardless of terrain (`night.c:428`).
- Validate collision grid dimensions, prefab bounds, tile indices, spawn support and arena limits during construction. Fail initialization clearly on allocation/asset failure; do not silently run the original collision with half of the new layout installed.
- Own replacement arrays in the Stage 3 runtime and release them exactly once on restart/exit. Never free the original pack-backed `Level` pointers. Keep the existing `night_dispose()` call-before-reset contract in `game.c:42–44`.

Modify `physics_step()` only where replay tests establish a real shared bug: for example, crossing an 8 px floor at high falling speed or landing on a one-way platform from the wrong side. Most of the current terrain failure is bad data, not evidence that the whole physics engine needs replacement.

## 4. Stage flow, arena entrance and retries

Use three separate state variables:

```text
Stage flow: TRAVERSE -> APPROACH -> ARENA_ENTRY -> BOSS -> DEFEAT -> CLEAR
Boss phase: PHASE_1 -> PHASE_2 -> PHASE_3                 (HP thresholds)
Boss attack: ENTER / TELEGRAPH / SWEEP / FRONT_ENTER / FRONT_FIRE /
             RECOVER / EXIT / PHASE_CHANGE / DYING       (attack timing)
```

During `TRAVERSE` and `APPROACH`, Hyperjumper is inactive: no boss draw, hit testing, damage, projectiles, inbound message, HP bar or boss timer. Merely waiting at the start must never activate it.

### Arena coordinates and trigger

The final 520 px contains the arena approach plus the fighting screen. On entry, calculate and store `arena_right=6200`, `arena_left=6200-screen_width`, `arena_cam_x=arena_left`. Thus the combat screen is x=5774–6200 in wide mode and x=5880–6200 in 4:3. This is a fixed world rectangle for that attempt; attacks subsequently use these stored bounds, never the changing follow camera.

1. Arm the entrance only in the final section. Trigger once when the live hero is grounded and its center passes `arena_left+64`; no timer alternative.
2. Stop new route encounters. Pan the camera to `arena_cam_x` over about 0.6 seconds while keeping the hero visibly inside the destination view. Cancel pending fire; do not teleport the hero to obtain the shot.
3. Close visible gates at the edges, clamp the physics world's min/max to the usable interior, and set `cam_locked`. Account for the hero's `body.ox/hx`; setting only `cam_locked` or zeroing velocity after crossing the edge is insufficient.
4. Show the boss name/HP bar and play an entrance cue. Hyperjumper flies from beyond the viewport to a visible staging position in 0.8–1.2 seconds. No contact damage or attack fire during entrance.
5. Restore control before the first telegraph. Start the boss pattern only after a further 0.6-second readable warning. The entrance is an in-world action beat, not a reused dialogue cutscene.

Use gate sprites/terrain art for shipping visuals. Use the available music/SFX provisionally; choose the final boss track by listening, not by assuming an undocumented numeric track ID. Keep the HP display present for the entire active fight, including full health, below the existing hero HUD.

### Checkpoint and lifecycle policy

- Checkpoints: start x=150, mid-level x=2848, boss approach x=5600. Resolve each y from its supporting floor and the hero's body offsets. Do not place a checkpoint on a one-way ledge or pit edge.
- On a route death, consume one life through the existing player lifecycle, restore the last checkpoint and rearm only that checkpoint segment's finite encounters. Earlier completed segments remain completed; clear all three projectile pools before resuming.
- On a boss death/retry, reset Hyperjumper to full HP/phase 1, reset pattern timers and hit cooldowns, open/reset gates, and return to the pre-boss checkpoint. Never respawn directly into a live pass or keep damaging projectiles from the previous attempt.
- Keep checkpoint state separate from the player's continuously updated `safe_x/safe_y`. `player_death_update()` can overwrite respawn positions from the death location; the Stage 3 policy must explicitly override that behavior at the death/respawn boundary.
- Use the boolean returned by `player_death_update()` and distinguish respawn from `game_over`. Never decrement lives in both systems. The existing CONTINUE restarts the stage; retain that behavior unless separately changed.
- Pause freezes entrance, gates, attacks and projectiles. A player death cancels attacks and any pending clear. If hero and boss receive fatal damage in the same simulation step, **player death wins** and the attempt resets.
- On boss defeat, disable damage immediately, clear projectiles, animate the wreck/explosions for about two seconds, then emit one clear event. Feed the existing mission-accomplished flow once; reaching x=6200 without a defeated boss cannot clear the stage.

## 5. Hyperjumper: three phases with learnable attacks

Preserve its identity from `../hyperjumper/screenshot_reference/`: alternating left/right boosted passes, firing from the side, a lower pass that can be ducked/slid under, and a front-facing spread. Preserve the native supplied boss sprites unless the art audit identifies an actual scale mismatch.

Proposed starting HP: Easy **30**, Normal **36**, Hard **42**. Use thirds of max HP for phase boundaries: Normal phase 1 above 24, phase 2 at 24 through 13, phase 3 at 12 through 1. Difficulty changes timings and shot count modestly; it does not remove the entrance, telegraphs or escape routes.

| Phase | Ordered attack cycle | What changes / how to respond | Damage opportunity |
| --- | --- | --- | --- |
| 1 — reconnaissance, 100–67% | Telegraph right, high pass right-to-left with one aimed shot; recover. Telegraph left, mirrored high pass; recover. Front entrance, one three-shot fan, recover and exit. Repeat. | Teaches side entry, locked aim and front spread separately. Walk away from the aimed line; move into a fan gap; aim diagonally/up while it slows. | About 1.0 s after each pass and 1.2 s after the front fan. |
| 2 — low strafing, 66–34% | Telegraph a low pass, cross once without simultaneous aimed fire; recover. High pass in the reverse direction with two separately telegraphed aimed shots; recover. Front entrance, two fan volleys with a safe gap; recover and exit. | Low pass introduces a real duck/slide clearance test. The follow-up is not fired until the low pass has cleared. Front shots add one deliberate lane change. | About 0.9 s after a pass and 1.0 s after the front sequence. |
| 3 — damaged engine, 33–1% | Low pass; recover; reverse high pass with a brief central firing hold; recover; front sequence of three volleys; long overheat recovery and exit. Repeat. | Recombines established attacks, with a visible damaged/overheated state. Shorter idle gaps, not unannounced new attacks or extra regular enemies. | At least 0.8 s between attacks; 1.4 s exposed after overheat. |

Do not replace an ongoing attack abruptly when crossing an HP threshold. Queue the next phase, finish/abort into a safe recovery, dissipate outstanding boss shots with a visible effect, play a 0.8-second phase-change cue and start the new phase. Advance monotonically; later damage cannot return to an earlier phase. Keep the firing sequence deterministic for a given attempt.

### Attack timing and geometry

All times below are starting values; the acceptance replays determine final tuning.

- **Telegraph:** at least 0.65 s on Normal; never below 0.5 s on Hard. Use an edge indicator plus booster animation/audio. Low/high attacks need visually distinct cues; a central text banner must not conceal bullets.
- **Pass movement:** accelerate outside the active fighting area at roughly 420–520 px/s, then cross at roughly 180–220 px/s. Scale duration for viewport width so 4:3 remains readable. The old constant 520 px/s low flyby is not a useful starting point for a narrow-screen dodge.
- **Low pass:** place the collidable hull bottom high enough for the standing-vs-ducking test to be real. Derive this from the current animation hurtboxes, not `boss_y > 150` plus blanket crouch immunity. Leave at least 4 px clearance above the tallest crouched/slide hurtbox being allowed to dodge. The hull should overlap standing height, and its art should show the same clearance. A stationary crouch is a valid alternative to sliding.
- **Aimed side shot:** reveal the muzzle charge, snapshot the player's position at the end of the warning, then fire a straight shot at that saved position. No tracking after launch. Start at 150–180 px/s and guarantee at least 0.35 s muzzle-to-target travel when released; defer a shot if the separation is too small.
- **Front approach:** move from completely offscreen above into the front hover position over roughly 0.75 s. Hold/charge for at least 0.65 s before firing. The projectile starts at the visible gun muzzle, not an arbitrary offset below the full image box.
- **Front fan:** use three lanes in phase 1 and up to five in later phases. Evaluate the gaps where shots reach player height: a safe gap must be at least the standing hurtbox width plus 16 px. Initial inter-volley spacing is at least 0.85 s. Any shifted opening must be reachable at 100 px/s with a reaction margin; alternating far corners is not acceptable.
- **Recovery:** decelerate/hover in a visible, reachable firing position and expose the pilot/core. A flash or color cue communicates vulnerability. Armored boost/travel states produce hit sparks without HP damage; a broad, generous exposed hitbox during recovery rewards ordinary aimed shots.
- **Exit/reorientation:** travel fully offscreen before changing between unrelated side/front poses. Re-entry follows a visible path. Changing facing or pose must not shift the hull or muzzle because of different PNG canvas centers.

No pattern may require jumping and sliding at the same time, crossing the entire screen faster than the hero can run, or accepting damage to escape a corner. Verify patterns starting at the left edge, middle and right edge, for both facings and both screen widths. A front volley followed by a low pass must leave enough time for the last projectile to clear.

### Hit detection and damage rules

Use local sprite metadata for a small set of hull rectangles, a vulnerable pilot/core box and muzzle anchors per side/front pose. Mirror local coordinates with facing and use the same origin for drawing and collision. Transparent canvas, booster flames and long non-solid antenna tips are not the hull. The current `66×48`/`85×57` half-extents are only rough placeholders.

Use the hero's current animation hurtbox, including its offsets. For all damage sources, reject dead, entrance-locked or invulnerable heroes. Consume a colliding projectile once and allow at most one successful damage event in a tick. Preserve the existing health semantics, including the last hit after the displayed heart counter reaches zero.

For moving projectiles, sweep from previous to next position against terrain and candidate hurtboxes, resolving the earliest collision. This matters because a player bullet moves about 8.3 px per 60 Hz step, enough to cross an 8 px surface. Share the collision helper between boss shots and regular bullets; do not run both old point-hit and new swept-hit paths on the same projectile. Solid `15` blocks bullets; explicitly one-way `4` does not, consistent with the current regular-bullet policy. No damage through rocks, terrain or a closed arena gate.

For the fast moving boss hull, use relative swept overlap against the hero or suitably bounded substeps; a frame-end point test alone can miss a crossing. Apply a short boss damage cooldown, initially 0.12 s, to avoid several overlapping bullets draining a phase in one tick. Distinguish armored hits, successful damage, and the phase-change event in traces.

## 6. Restore the pixel-art scale of the cleaned scenery

The source PNGs are working masters. Keep them unchanged and generate dedicated runtime versions under `assets/stage3/native/`. Add `tools/build_stage3_assets.py` and a manifest containing source path/hash, native output size, palette, anchor and collision/decorative role. Re-running the build must reproduce identical pixels and must never select the pre-cleanup Git versions.

Initial native targets, preserving aspect ratio to rounding precision:

| Asset | Current source | Current drawing | Proposed native runtime size |
| --- | --- | --- | --- |
| Night sky | 2172×724 | Height-scaled to 240; fractional width | 720×240, seamless horizontally, integer repeat width |
| Red moon | 480×480 | About 187×187 | 96×96 first pass; retain a single moon, tune composition at actual game size |
| Mesa | 505×487 | 160–174×158–166 | 160×154, behind the gameplay plane |
| Cactus | 349×512 | 64–66×150–154 | 64×94 |
| Scrub | 491×353 | 86–92×105–112 | 64×46 |
| Fence | 465×330 | 132–138×102–106 | 112×79 |
| Wreck | 512×298 | 170–175×106–108 | 128×75 |
| Pad fragment | 455×359 | 142–146×76–78 | 112×88; separate repeatable walkable pad tiles from this decorative cutout |
| Hyperjumper side / front | 130×108 / 201×140 | Native | Keep initially; audit next to the original hero and reference images |
| Hyperjumper projectiles | 10×11 / 6×12 | Native, runtime rotation | Keep size; consider reviewed fixed-angle variants if rotation causes pixel shimmer |

These are visual targets, not instructions to shrink every object by one universal factor. Cropping residual transparent margins may change the canvas sizes; preserve the intended visible-object proportions and foot/origin anchors in the manifest. A physically large boss can still have the correct pixel density.

Asset conversion workflow:

1. Compare at 1× native scale with original Fireball, Outriders and desert terrain. Reference sheets contain about 76, 40 and 131 colors respectively; the goal is matching pixel clusters and readable value groups, not imposing one artificial color count on the whole game.
2. Determine whether each cleaned source has a consistent underlying pixel grid. Use a nearest-neighbor reduction with an explicit sampling origin for aligned pixel clusters. For irregular/generated detail, compare a palette-aware block-vote reduction, then manually repair the contour and important clusters. Automatic shrinking alone is not the completion criterion.
3. Apply a shared night palette, initially 32–48 opaque colors per prop family, allowing up to 64 where a reviewed wreck needs it. Use 3–5 deliberate shades per material and reserve saturated red for moon rim-light, not noise across every edge. Disable diffusion dithering.
4. Clean alpha after reducing: binary alpha for solid scenery, no dark/purple matte fringe and no isolated semitransparent speckles. Keep deliberate translucent atmospheric effects separate. Inspect against black, mid-gray and the actual sky.
5. Repair isolated pixels, disappearing cactus arms, fence strands, roof bars and pad silhouettes at native resolution. Inspect at 1× and 4× nearest enlargement. Do not introduce smoothing, sharpening halos or a new art style.
6. Load the native outputs and draw at 1:1 logical pixels, snapping position to integers. Remove arbitrary per-instance width/height stretching from `STATIC_PROPS`. Pre-author a second size if needed instead of scaling the same prop slightly differently everywhere.
7. Keep nearest texture sampling and integer screen presentation. Align sky wrap positions to integer pixels; inspect seams while scrolling. Keep the single red moon composed across the route rather than letting `sw*0.64-cam_x*0.18` carry it permanently offscreen early in the stage. A very slow bounded parallax is sufficient.

Do not use the generated art for the player, enemies or boss. The supplied actor sprites remain the visual reference. Avoid another image-generation pass unless a specific missing static piece is identified during implementation.

## 7. File and line modification map

All paths below are relative to `game/`. The numbered spans are current source lines, not promised final line counts.

| Existing file / lines | Function or area | Planned modification |
| --- | --- | --- |
| `src/night.h:18–59` | `Night`, shot records, single boss enum, API | Add separate stage-flow and boss-phase state, checkpoint data, fixed arena bounds and clear/death events. Replace the embedded monolithic boss fields with the new boss module. Return initialization failure explicitly. |
| `src/night.c:16–28,82–143` | Platform/pit tables, `build_arena_collision`, `rearrange_tilemaps` | Replace with `night_level_build()` and complete terrain prefabs; remove strip reshuffling and mismatched geometry. |
| `src/night.c:40–79` | Sprite/background/static loaders | Read native runtime assets and metadata; fail clearly for required missing assets. Keep source masters separate. |
| `src/night.c:146–177` | `night_dispose`, `night_init` | Initialize traversal with a dormant boss; own/free authored arrays safely; install spawn/checkpoint/arena definitions. Remove the immediate inbound message/timer. |
| `src/night.c:180–303` | Shot creation, firing, damage and overlaps | Move to `hyperjumper.c`; introduce origins, pose boxes, swept collision, immunity, vulnerability and correct damage results. |
| `src/night.c:305–371` | `night_update` | Replace with the stage director. Drive the boss only in active arena states; coordinate entrance, checkpoints, retries and one-shot victory. |
| `src/night.c:373–393,432–461` | Boss/shot drawing, boss HUD | Move boss rendering to the boss module. Render only appropriate states, match anchors, show full HP on entry, remove obstructive attack banners. |
| `src/night.c:395–428` | Background and `STATIC_PROPS` | Native sky/props, bounded moon parallax, world y anchors, draw order and role-based placement. Use terrain placements for collidable objects. |
| `src/game.c:38–102` | `level_start` | Resolve player layer before installing Stage 3 actors. Check Stage 3 build success. Keep imported Level 1 triggers excluded, add Stage 3 encounters, spawn on real ground and preserve direct gameplay start. |
| `src/game.c:195–209,294,334–355` | Camera and world bounds | Support entrance pan and fixed arena bounds. Prevent the normal camera/min-x assignment from overwriting arena or checkpoint settings. |
| `src/game.c:289–340` | Update order, respawn, zones, clear | Capture lifecycle events, apply Stage 3 checkpoint policy, resolve projectile movement/collision coherently, cancel attacks on death, emit clear once. Use `player_layer`, not literal `11`, for Stage 3 effects. |
| `src/game.c:326–332` | Enemy release and victory signals | Prevent a route/legacy release from unlocking the boss arena, and make boss defeat the sole Stage 3 completion source. |
| `src/game.c:358–380,407–441` | Debug drawing and stage drawing | Add terrain/actor/shot hitboxes, encounter and checkpoint/arena markers. Draw authored Stage 3 visuals with correct occlusion. Keep the existing Level 1 tint restoration. |
| `src/game.h:23–36` | Camera, flow and Night ownership | Store any integration state outside `Night` only where needed; avoid two independently authoritative arena/checkpoint states. |
| `src/enemies.c:21–34,126–203` | Trigger setup and spawning | Add a typed Stage 3 encounter adapter with finite counts, valid anchors and safe activation. Bound waypoint copying to the source's three `wp` entries if constructing `LevelObject`s. |
| `src/enemies.h:18–54`; `src/enemies.c:278–293,297 onward,694–737` | Patrol metadata and humanoid updates | Support Stage 3 encounter IDs, visibility/fire delay, patrol bounds and ledge policy without altering Level 1 defaults. Reset by checkpoint segment. |
| `src/enemies.c:214–225,229–261,750–767` | Damage and projectile/entity hits | Centralize/consistently enforce immunity, and integrate the shared projectile collision result. Remove duplicate old hit processing where the new path applies. |
| `src/player.c:75–107`; `src/player.h:6–23` | Death/respawn interface | Expose or honor a checkpoint respawn override for Stage 3; preserve life accounting and invulnerability. Do not globally replace Level 1 safe-position behavior. |
| `src/character.c:59–76,303–333,364–391` | Ground type, slide/drop and hurtbox selection | Verify against corrected data. Change only if measured tests reveal a defect; do not patch down+jump to ignore one-way semantics. |
| `src/physics.c:12–75`; `src/level.h:40–48` | Terrain collision and grid | Validate masks and dimensions; add swept/crossing fixes only if demonstrated by the falling/edge tests. |
| `src/bullets.h:11–23`; `src/bullets.c:21–79` | Projectile storage/update | Retain previous positions or collision segments; resolve solid terrain and entity hits in time-of-impact order with the boss-shot helper. |
| `src/gfx.c:36–42,190–222` | Texture sampling and draws | Keep nearest sampling. Prefer integer-position, native-size calls for Stage 3; no global renderer filter change. |
| `src/main.c:53–109` | Script, screenshot and fixed-step hooks | Add deterministic review/test modes, exact-step captures and explicit exit results. Keep existing flags working and document output format. |
| `CMakeLists.txt:9–15`, `Makefile:9–24` | Source discovery/build | Register new modules/test targets and header dependencies. Reconfigure CMake after adding `.c` files because the existing glob is configure-time. Ensure Make notices generated metadata/header changes. |
| `README.md:19–20,75–94` | Stage 3 description and controls/debugging | Describe the route, checkpoint rules, arena trigger and phases accurately; replace the immediate-arena claim. |

New files proposed:

- `src/night_level.c/.h`: authored terrain/encounter data and builder/validation; no separate collision-only layout.
- `src/hyperjumper.c/.h`: boss state, patterns, metadata-aware rendering and collision.
- `src/projectile_collision.c/.h`: shared segment/box/grid collision helpers, kept small and independently testable.
- `tools/build_stage3_assets.py`, `assets/stage3/manifest.json`, `assets/stage3/native/`: reproducible native art outputs from the cleaned masters; generate a small C metadata include if runtime access needs it.
- `tools/stage3_review.py`: orchestrate actual engine captures and structured trace checks, plus panorama assembly. It must not implement an independent substitute renderer/layout.
- `tests/stage3/`: meaningful geometry, flow, collision and replay tests plus recorded input scripts, without a second copy of the boss implementation.

## 8. Implementation order and review gates

1. **Baseline and review hooks.** Record source/asset hashes, movement/hurtbox values, current failures and key reference frames. Add deterministic stage traces, hitbox overlay, native viewport capture and render-only overview mode. Gate: the current premature boss and ground-drop tests fail for the expected reasons.
2. **Terrain and traversal first.** Implement authored prefabs, solid ground, reachable gaps, proper death/respawn points and no active boss. Gate: a real input replay crosses the entire route in both screen widths, and terrain/collision overlays agree at every pit and platform.
3. **Regular encounters and checkpoints.** Add finite encounters, safe firing/spawning, checkpoint reset and the quiet approach. Gate: complete the route with live enemies, die/retry at every checkpoint, and wait at the start without a boss appearing.
4. **Native asset pass.** Build and manually review low-resolution scenery against original sprites; replace stretched placements. Gate: approve 1× frames and the full level overview before polishing boss effects.
5. **Arena entrance and lifecycle.** Implement camera/gates, visible arrival, no early combat and clean retries. Gate: cross the real trigger by walking; verify both widths, pause, death, continue and no premature clear.
6. **Boss patterns and collision.** Implement each phase's complete attack/recovery cycle and validate dodge paths before tuning HP/difficulty. Gate: every attack is avoidable from the specified starting positions without relying on invulnerability or damage trading.
7. **End-to-end tuning and regression.** Play from level start through a legitimate boss defeat; tune times/spacing from captures. Gate: meet the acceptance matrix below and check Stages 1/2 and Stage 2→3 handoff.

Do not call a build, an idle screenshot, a boss-HP shortcut, or a panorama alone a completed gameplay verification. Keep each gate's evidence with the implementation.

## 9. Verification commands and deliverables

### Existing commands usable now

Run from `game/`:

```bash
cmake -S . -B build -G Ninja
cmake --build build
env SABER_ASSETS=./assets ./build/saber_rider ../SaberRider/data --level 3

# 4:3: movement, visibility and arena geometry must work here too.
env SABER_ASSETS=./assets SABER_RATIO=-1 ./build/saber_rider ../SaberRider/data --level 3

# Save a baseline after 1.5 seconds; no automated player movement.
env SDL_AUDIO_DRIVER=dummy SDL_AUDIODRIVER=dummy SABER_ASSETS=./assets \
  SABER_WINDOW=852x480 SABER_SCRIPT='90:' \
  SABER_SHOT=/tmp/stage3-start.bmp,-1,90 \
  xvfb-run -a ./build/saber_rider ../SaberRider/data --level 3

# Reproduce the current down+jump-through-ground bug.
env SDL_AUDIO_DRIVER=dummy SDL_AUDIODRIVER=dummy SABER_ASSETS=./assets \
  SABER_TRACE=1 SABER_SCRIPT='30:,1:DJ,49:' \
  SABER_SHOT=/tmp/stage3-slide.bmp,-1,80 \
  xvfb-run -a ./build/saber_rider ../SaberRider/data --level 3

git diff --check
```

F1 toggles the current collision overlay; `SABER_DEBUG=1` enables it initially. `SABER_START=x` changes the initial player x. Existing `SABER_SCRIPT` uses `L R U D J S A P` for fixed-step input segments; add an explicit final neutral segment to release held input.

Limitations of the existing screenshot hook: it writes **BMP**, regardless of filename extension; its camera argument sets only the initial camera, not a persistent camera lock. The accumulator can also pass the requested step before rendering under a stall. Therefore do not use it as an exact replay assertion without fixing the step-boundary capture logic. The existing recorder captures at 15 fps and is useful for overview only; boss timing review needs a 60 fps or exact-step capture path.

### Proposed review interfaces — implement before using

The following are interface requirements for the new review tool, not commands that currently exist:

```bash
python3 tools/build_stage3_assets.py --check
python3 tools/stage3_review.py --suite traversal --ratio wide
python3 tools/stage3_review.py --suite traversal --ratio 4:3
python3 tools/stage3_review.py --suite boss --all-phases --all-difficulties
python3 tools/stage3_review.py --suite lifecycle
python3 tools/stage3_review.py --export-overview artifacts/stage3-review
ctest --test-dir build --output-on-failure
```

`--check` verifies reproducibility, native dimensions, palette/alpha rules and unchanged master hashes. Replays must seed randomness, exit after a fixed number of steps, assert outcomes and return failure status on errors. Boss-only fixture starts are useful for attack tests; they do not replace the full-stage traversal or natural HP-threshold test.

Trace each transition once with frame, stage-flow state, phase/attack, player position/HP/lives, checkpoint ID, boss HP, camera/bounds, projectile count, and damage/victory reason. Record build/asset hashes and screen ratio alongside captures. Disable wall-clock-dependent animation in exact comparison modes where it would make runs differ.

### Acceptance matrix

| Area | Test | Pass condition / evidence |
| --- | --- | --- |
| Start and pacing | Idle 20 s at spawn; walk through each traversal section. | No boss activation, shot, HP bar or boss message before the final trigger. Ordinary enemies appear in the authored groups. |
| Terrain | Traverse all three mandatory pits and every optional shelf using normal controls. | No impossible jump, invisible support, false floor, edge snag or collision beyond the visible surface. Same result in 426/320 modes and with each playable hero. |
| Slide/drop | Down+jump on solid floor; repeat on a one-way shelf; slide into an enemy; crouch beneath a low pass. | Floor slides safely; shelf drops to a lower solid surface; the chosen hurtbox, terrain and damage response agree. |
| Solids | Run/jump against rock sides and undersides; fall at maximum encountered velocity. | No penetration/tunneling; no one-way landing from below. Verify the deliberate 1 px rest offset in the overlay. |
| Enemy combat | Shoot enemies with/without intervening cover; approach from either side, linger, retreat, revisit after death. | Shots hit once, solids block them, no infinite trigger respawn, no patrol falls or visible spawn inside the player. |
| Checkpoints | Die before/after x=2848 and near x=5600; exhaust lives; continue. | Correct supported respawn, exactly one life loss, correct encounter reset, no bullets left over or camera pushing the player into a pit. |
| Entrance | Walk across the actual arena threshold, then attempt to retreat/jump past gates. | One entrance, camera settles, visible gates and boss arrival, no damage during entrance, bounds hold in both ratios. |
| Phase progression | Damage the boss naturally through both thresholds, including during a shot/recovery. | Phase sequence 1→2→3, coherent phase-change cue, no teleport or overlapping old/new attacks. |
| Dodge fairness | Run scripted dodges from left/center/right against every attack and phase, on all difficulties. | At least one reachable damage-free response for each tested starting condition; clear warning, no unavoidable corner combo. Record successful inputs and minimum clearance. |
| Hitboxes | Overlay standing/crouching/sliding hero, side/front hull/core, muzzles and swept projectile paths. | Hits match visible material; no damage from transparent canvas; crouch works through geometry, not a blanket immunity exception. |
| Immunity | Force body+shot overlap in one tick and within the respawn/hit window. | At most one accepted hit; no boss contact bypass of `CF_HIT`. |
| Pause/death/clear | Pause in entrance and every attack; die with shots active; defeat boss with shots active; simultaneous fatal hits. | Correct freeze/resume, complete retry reset, no post-defeat damage, death-priority rule, mission clear once after the death animation. |
| Art | View at 1× and integer 2×/3×/4×; scroll whole route; inspect on alternate alpha backdrops. | Consistent pixel density, no distortion/halos/shimmer, no clipped wreck, no sky/tile seams, readable bullets and landing edges. |
| Complete stage | Uncut normal-controls run from x=150 to boss victory, without HP/position cheats. | Target traversal/fight durations are measured; no softlock, required damage trade or impossible jump. Include a death-and-retry run. |
| Regression | Stage 1 movement/one-way platforms/enemies/horse boss; Stage 2 race/boss; Stage 2→3 transition and retry. | Existing behavior survives shared changes; lives and selected hero carry correctly; no Stage 1 triggers or assets are altered. |

Deliver these artifacts with the implementation:

- `stage3-route.png`: an actual-engine overview of the authored 6200×240 gameplay plane, with no clipped tail or missing pieces. Export parallax scenery separately or with an explicitly documented overview convention; a stitched panorama is not a single camera view of parallax.
- `stage3-route-collision.png`: matching terrain/collision view, plus separately labeled encounter/checkpoint/arena annotations. Overlays should not be present in the clean art view.
- Native-size screenshots at the start, each pit/shelf, checkpoint, quiet approach, boss entrance, and one telegraph/attack/recovery frame per phase, in both screen widths.
- A side-by-side native art contact sheet containing original hero/enemy/terrain pixels and the new scenery; include transparent-edge samples.
- A 60 fps full normal-play video and structured traces from the same run, plus the repeatable test report. Exact-step frame sequences can substitute for lossy video when judging hitbox/timing defects.

Completion means the player can read and traverse the level, fight ordinary enemies along the way, reliably reach the arena, learn and defeat all three boss phases, and retry fairly. The terrain, collision and native-resolution art must all describe that same playable space.
