# Minimal portal scene test

Current implementation: GLES, the backend used by the connected-phone workflow.

Active portal graph: hub -> forest / combined physics-parkour course -> hub.
The blue combined gate at X=5, Z=-4 replaces the two separate prototype gates.
Route: original 12 jumps -> crate/plate/gate/lift -> longitudinal shuttle ->
lateral shuttle -> bounded 10 m pool -> shallow exit stairs -> final return gate.
There are six ordered checkpoint milestones plus the start. Falls preserve the
solved mechanism and reset only player position/velocity; retry/new visit resets
the full course. Horizontal platform ground velocity carries standing players
and remains part of their takeoff momentum. Each shuttle dwells at both ends.
Water center/halfExtents bound swimming and GLES compositing to the actual pool.

The separate parkour/workshop files below remain regression fixtures; they are
no longer independent destinations in the hub.

- Original scene: stone gate at runtime X=0, Z=-4, ahead of the initial spawn.
- Walk through the gate opening to load scenes/forest_test.json.
- Forest: 24 x 24 m, 16-bit heightmap, 27 EZTree visual props, no enemy.
- Return gate: X=0, Z=3. Walk through to reload scenes/main.json.
- Original isolated workshop fixture: scenes/workshop.json. Push the lone
  crate forward onto the gold plate and keep it there for 0.6 seconds. The gate
  latches open; walk around the crate and board the lift beyond the gate. The
  lift travels 4 m at 1 m/s, dwelling one second at each end. Leave onto the
  upper deck and use its return gate. A return gate is also available at spawn.
- Workshop crate is a 12 kg dynamic Jolt box. Gate/lift are kinematic bodies;
  rendered transforms and shadows read their live positions/rotations. The
  character inherits lift ground velocity. Falling resets the puzzle without
  losing health/equipment; leaving/re-entering or retrying resets all mechanisms.
  gameplay.workshop references three solid cube IDs and defines plate/lift travel.
- Sky parkour has 13 floating platforms: straight ascending jumps, a staggered
  turn section, then narrow landing pads. Cyan/amber/violet edge lights and
  forward arrows identify the three sections. All 12 gaps are physically
  reachable with the existing sprint and jump speeds; no movement buff is used.
- Two broad rest islands save checkpoints. Falling below Y=1 returns to the
  latest checkpoint with zero velocity and retains inventory/health. Grounded
  arrival activates checkpoints in sequence; the finish freezes the timer.
  The HUD shows checkpoint progress, elapsed time and falls. Both the start and
  finish have a violet return gate. A new visit resets course progress.
- gameplay.portal remains supported; gameplay.portals adds up to eight gates.
  gameplay.parkour provides ordered checkpoint spawn/activation bounds and fallY.
  groundEnabled=false removes the legacy invisible floor for floating levels.
- Inventory, gold, settings, health, ammunition and active weapon survive a switch.
- Startup discovers the portal graph (up to eight scenes), prepares one GPU asset
  job per progress frame, and caches terrain collision shapes before showing the
  menu. Static models are shared by rawfile path; inactive terrain GPU handles
  stay resident. Portal crossings swap prepared resources without decoding files.
  Shared character, weapons, sky and UI stay resident. A running session must be
  restarted after editing packaged scenes so its immutable caches are rebuilt.

Recreate scene assets with the bundled Python (Pillow required):
`python tools/create_portal_test_scene.py`
`python tools/create_parkour_scene.py`
`python tools/create_workshop_scene.py`
`python tools/create_physics_parkour_scene.py` (run last to merge the hub entrances)

Build the signed application from ohos-project:
`hvigorw.bat --mode module -p product=default -p module=entry@default -p buildMode=release assembleHap --no-daemon`

Host collision regression (Visual Studio 2022 C++ tools):
`cmake -S ohos-project/tools/portal_tests -B ../portal-tests -G "Visual Studio 17 2022" -A x64`
`cmake --build ../portal-tests --config Release --parallel 1 -- /nr:false`
`../portal-tests/Release/portal_tests.exe ohos-project/entry/src/main/resources/rawfile`

Expected: 219 checks, including three cached collision-world round trips,
camera sphere sweeps, 12 actual Jolt platform jumps, checkpoint/fall/finish logic,
and restoration of the hub floor. Also tests sprint takeoff inertia, a real
character pushing the crate, plate latching, raised gate collision, a full
lift ascent/descent carrying the player, retry and workshop body cleanup.
Also verifies both moving shuttle rides through reversals, bounded swimming,
swimming up the exit stairs, and checkpoint respawn retaining the solved gate.
Warm switches must not read rawfiles.
Device acceptance additionally checks gates, course framing, jumping and fall
recovery, foliage, HUD continuity and equipment retention. Scene switching and
course HUD currently use GLES; Vulkan portal switching is not implemented.

Multi-touch regression (target touch_multitouch_tests): 229 checks exercise the
production XComponent snapshot action filter and TouchController together,
including SDL's cancel-before-duplicate-DOWN behaviour. Across 32 simultaneous
stick/action cycles, only callback.id owns DOWN/UP; other pressed snapshot points
become MOTION. Floating stick anchors stay captured until the left finger lifts.
Explicit surface CANCEL still releases every point.

Save-game regression (target save_store_tests): 29 checks cover independent
slots, restart/reload, inventory and ammunition, last-save timestamps, updates
without duplicates, ten slots, validation, failed writes, and recovery from a
truncated newest bank. The scene regression now has 226 checks, including save
point metadata and restoration of the solved gate after a physics reset.

Manual acceptance for the production GLES backend:
1. Start Game opens the save list; a fresh install offers Create New Game.
2. Create a game, equip items, and approach the cyan save pillar in the hub
   (near x=-3, z=3). Tap Save Game and check the success message.
3. Return to title, create another game, and verify both independent slots and
   their local last-save dates. Three entries fit on a page.
4. Fully close/reopen the app, continue the first slot, and check scene,
   safe-point position, health, gold, inventory, equipped weapons and ammo.
5. The forest spawn and all seven stationary parkour checkpoint islands have
   save pillars. Save after solving the gate, reopen, and verify gate progress
   and the course checkpoint. Lift/shuttle motion restarts from its initial
   phase; saving positions are on stationary ground.

Scene save points and markers can be reapplied idempotently after regenerating
the course: `python tools/add_save_points.py` from the repository root.
Save data is stored in application-private filesDir and survives replacement
installation. Uninstalling/clearing application data removes it.

The main scene is now a 24x20 metre white portal hub, with four solid walls,
two destination gates, no demo actors/props/terrain/sea/lights, and the existing
save position near x=-3,z=3. White GLB models author explicit box colliders;
other imported models still receive no implicit collision shapes.
Run `python tools/create_hub_scene.py` last after any prototype generators to
restore the minimal hub. Future routes extend gameplay.portals and their frames.
The scene suite now passes 232 checks, including hub floor/wall alignment,
sprint blocking, and absence of an invisible legacy floor outside the platform.

Arena extension: the hub now has a third (orange) gate to scenes/arena.json.
The player starts in the centre; four cardinal spawn doors feed four waves
of 1, 2, 3 and 4 enemies (10 total). Death animation/dissolve completes before
actor reuse. A violet return gate sits at x=-8,z=8, outside the central lanes.
Each enemy has an independent Jolt character, health, combat and animation
state. The production GLES renderer draws each enemy with its own skin palette.
The arena restarts on a fresh visit; death/retry uses the active saved game.
There are no arena save points during combat.

Regenerate with create_hub_scene.py followed by create_arena_scene.py. The
scene/Jolt suite passes 241 checks, including all four independent characters
chasing the centre and cleanup on leaving. Target arena_wave_tests passes 2617
checks for the 1/2/3/4 sequence, survivors/death animation blocking advancement,
completion without a fifth wave, and a fresh reset.

Manual arena acceptance: enter the orange hub gate, equip a weapon, wait for
wave 1, and verify simultaneous enemy counts rise to four on the last wave.
Check hit/death animations, individual health bars, waves/HUD, player death
before retry, and return-gate cleanup. No automated device play is required.

Updated parkour (layout revision 3): start -> three stationary jump bricks ->
first rest island -> longitudinal timing ferry -> second rest island -> three
independent lateral jump bricks -> crate/plate/gate -> lift -> pool -> finish.
The four moving bodies carry their light strips and arrows through motionParent
references. Seven checkpoints/save positions remain on stationary ground.
Completion grants 100 gold once per full run. Save format V2 retains the reward
claim and layout revision, reads V1 saves, and restarts obsolete course positions
at the new start while keeping inventory and gold.

Current validation: 262 scene/Jolt checks, including the static approach jumps,
near-end ferry boarding and far-end dismount, four platform riders, solved gate,
pool exit and reward idempotence; 31 save checks including V1 compatibility and
reward/revision persistence. Regenerate the combined scene, then add_save_points,
then create_hub_scene so the minimal hub retains its arena/forest/course gates.


## Vulkan / GLES gameplay synchronization (2026-11-02)

The default backend remains GLES on arm64 and x86_64. `MIKAN_RHI=vulkan` explicitly selects native Vulkan; do not use this override on the emulator whose Vulkan bridge is unstable.

Both renderers include `gameplay/save_session.inl` for save selection, multiple slots, save points and course-version recovery, and `gameplay/arena_combat.inl` for arena combat. Wave scheduling, course progress, rewards, physics, weapon state and inventory use shared gameplay types. Native Vulkan now implements resource preloading/cache switching, checkpoint respawn/rewards, independent animated arena enemies, sword attachment/poses, weapon UI, bounded swimming water and colored portal membranes. Static visibility and instance batches are built separately for camera and shadow views.

Validation: regenerated Vulkan SPIR-V; release builds target arm64-v8a and x86_64. Existing host binaries pass 463 scene, 31 save-store, 2617 arena-wave and 29 animation checks. These checks cover shared rules and scene fixtures; they do not validate native Vulkan rendering or device gameplay. Native Vulkan still requires manual acceptance on a real device, including portal transitions, save reload, all arena waves, sword/pistol switching, moving-platform riding and swimming.
