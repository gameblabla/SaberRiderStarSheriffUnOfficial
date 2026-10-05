# Pursuit banking and static camera repair

The pursuit escort spawner runs in overlay bank $72. It previously called
`rnd` directly in bank $7a, although both banks execute at $6000. The call
therefore executed unrelated instructions from the mapped bank. Race and
pursuit now share `race_random` in fixed resident code. A linker assertion
prevents placing it in the overlay execution window again.

Stage 1 security cameras are composited into the background at their authored
waypoints, with their wall pixels matched to the building below. Camera sprite
assets and runtime animation are removed; the actor ID is 255, so the renderer
skips its sprite. Camera trigger behavior remains.

Validation: retail image builds and passes the ELF/memory checks. The linked
RNG is at $4ceb in fixed bank $68. Generated stage 1 assets have no camera
sprites. Gameplay verification is pending: the initial seeded retail restart
fixture timed out before entering phase 2; a revised fixture was interrupted
at the user's request to build/package the release instead. No first-race or
full-campaign tests were run, and no physical hardware was tested.
