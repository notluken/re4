# How the engine is put together

A reading guide to `src/`. It says what each subsystem does and where its entry points are; the
per-function comments in the sources carry the detail. Names are the vendor's where the symbol files
or the PS2 debug symbols gave them (`docs/naming.md`).

## Frame and tasks

`main.cpp` boots the SDK, the SN debugger stub (`lib/__start`, `ppcdown`, `fileserver`) and the
scheduler (`scheduler.cpp`), then runs the game task. `game.cpp` is a state machine on
`GlobalWork::Rno0`: 0 init, 1 stage init (disc swap, save), 2 room init, 3 the main loop, 4 door
demo, 5 ending, 6 option. A room is loaded as one archive whose blocks are tagged `SMD`/`SMX` (scroll
objects), `LIT` (lights), `SHD` (shadows), `EFF`/`EAR` (effects), `SAR` (collision), `TEX`, `ITM`/`ETM`/`ETS`
(items, etc objects), `CAM`, `BLK`, `EVS`/`FSE`/`AEV`/`ITA` (scenario data), streamed by `block.cpp` over
`datactrl.cpp` (MRAM/ARAM units) over the `dvd.cpp` read queue. Everything that runs per frame is a
*task* registered with the scenario system (`sce_sys.cpp`: `SceExec`, `SceSleep`, priorities
`SCE_PRIORITY`) or a *collision-area task* (`sce_at.cpp`: `SceAtDataSet_exec(area, SCE_LEVEL, fn)` runs
`fn` while the player stands in `area`).

Difficulty is continuous: `GameAddPoint(LVADD_*)` (game.cpp) adds or subtracts points for kills,
damage taken, accuracy and deaths, scaled by rank tables and clamped to 0..11000; `Game_level =
point / 1000` drives enemy HP, damage, drops and QTE windows across the tree (`em_sub.cpp`
`LifeDownSet2`, `GetDropBullet`, ...). The RNG (`rnd.cpp`) is a 16-bit fixed-seed generator started
from `RndInit(0xD37)`.

## Objects

`cUnit` -> `cCoord` (matrices, `pos`/`ang`/`scale`, parent link) -> `cModel` (a `cModelInfo` chain of
bin+tpl models, the `cParts` list, `Motion`, `LightInfo`, routine bytes `r_no_0..3`) is the base of
everything drawn: `cEm` (enemies and the player: `cPlayer`, `cSubChar` overlay the same struct),
`cObj` (scenario objects), `cLight`, effects. Each class lives in a `cManager<T>` pool
(`cManager.h`) with alive lists; `be_flag` bits (`EM_STATUS` enum) say active/visible/hit-testable.
The routine encoding is the same everywhere: `r_no_0` picks the R0 table (0 init, 1 move, 2 damage,
3 die, 4 scenario-driven), `r_no_1` the entry of that table (move routines are `{branch check,
move}` pairs), `r_no_2` the step inside it, `r_no_3` a variant. `model.cpp`/`motion.cpp` implement
the model format (`RotMatrix` = Rz·Ry·Rx, contiguous parts when `be_flag & 0x2000`) and the motion
format (`FCV` entries of the character archives; `tools/motion_export.py` exports them to glTF/BVH
and verifies itself by byte round-trip and against the game in Dolphin, see `tools/motion/README.md`):
per joint a kind word (root pos/rot, rot, pos, scale, IK chain root) and per axis a Hermite key
stream in ten Fcc layouts (f32/s16/s8 values and tangents, ints in 1/10000), sequence frames in 10.6
fixed point,
`Mot_attr` bits (root speed, reverse, loop, flip), a second `MotionWork` blended through
`blend`/`Brate`, and leg IK per chain (`ik.cpp`: position keys on IK effectors — ankles, hands — are
targets in model space, so a pose needs the IK solve, not just the keys).

## Collision

Two managers (`atari.cpp`, `at_sub*.cpp`, `at_mod.cpp`): `SatMgr` holds the scenery the characters
walk on (`check`/`checkAir` push the `cAtariInfo` bodies, `getFloor`, line `hitCheck` with 24-bit
polygon attributes: `m_flag` 0x40 floor, 0x80 wall); `EatMgr` holds what bullets, thrown objects,
effects and cameras hit (attribute 0x400000 = object quads). Character-vs-character pushing is
`EmAtCheck` with push priority in `m_flag` bits 3-4. `area.cpp`/`flr_at.cpp` are the axis-aligned
trigger areas the scenario uses.

## Camera

`CamCtrl` (`cam_ctrl.cpp`) picks a cut from the room's `CAM` data (calm/battle variants, lerp
records) and maps cut types onto R0 routines; the default is the over-the-shoulder `CameraQuasiFPS`
(`cam_qfps.cpp`: per character/weapon offset tables, blending, floor tilt, collision pull-in,
overridable per room). Scope, binocular, push, look-down and motion-attached cameras are `cCamera`
extras (`cam_extra.cpp`). `CameraMove` writes the result to `pG->Cam` unless the debug camera
(`db_cam.cpp`, `DbgFlagChk(pG, DBG_DBG_CAM)`) owns it.

## Effects

`eff_sys.cpp` (`cEspSystem`) registers effect data per owner id (0 core, 1 room, EM/WEP/ET...,
`owner_name_tbl`). `esp.cpp` is the 0x150-byte `cEsp` pool (`PullEsp`/`PushEsp`, `EspMove`, `EspTrans`
into the OTs by `Tool_flg`/`Parts_no`); each `espNN.cpp` is one effect id with `Create`/`move`/
`SetFreeWork`/`Trans`. `EstSet(owner, id)` looks the id up in the est table and hands a sequence
(`EspGenWork` = the PS2 `cEspSeqTbl`) to generator 10 (`espgen10.cpp`), which plays records by
`Set_time`: kind 0 sprites (`EspSeqSet`, or effect models through obj04/05/09 for ids 0xFC..0xFF),
kind 1 generators (00 emitter, 01 lens flare, 02 path emitter, 42/45 water, 43 sand, 44 filter).
`EspInfo::Core_flg` decides deletion (bit 0 survives pauses, 0x800/0x1001/0x2001/0x3001 are
event-owned tiers). `Espgen42.cpp` (water height field) and `Espgen43.cpp` (deformable sand) publish
`GetWaterHeight`/`AddWaterPower`/`GetSandHeight`, gated by `Status_flg[0]` bits.

## Enemies (`src/em*`)

`em.cpp` is the `cEm` base (`move` = damage volumes `emNNDmCk` -> R0 table -> `EmAtCheck` / `atari.move`
/ `SatMgr.check`). `em10.cpp` is the Ganado (village, castle zealot, island soldier: `Em10Work::
Ganado` 0/1/2 scales damage ×1, ×0.556, ×0.4545) shared by the 16 modules em10..em20 whose own object
is an `emNN_set.cpp` (models, voice tables); the other `emNN` directories are one enemy each
(`cEmMgr::idName`: 0x21 dog, 0x22 Colmillos, 0x23 crow, 0x24 snake, 0x25 Plaga, 0x26 cow, 0x27
bass, 0x28 chicken, 0x29 bats, 0x2A traps, 0x2B El Gigante, 0x2C insect boss, 0x2D Novistador, 0x2E
spider, 0x2F Del Lago, 0x30.. the bosses and late enemies). Ganado weapon reactions dispatch on
`Em10DmSetWep_tbl[dmWep]` into five classes (melee, bullets, shotgun, heavy, flash) modulated by
`Em10Work::Be_flg` (down, dashing, carrying Ashley, on a ladder, headless...); head is part 5, arms
8/0xE, hands 9/0xF. Groups coordinate through the room's ctrl12 timers (`CTRL12_ID_EM10_ATK/THROW/
NOT_NEAR` lock all Ganados out of attacking for 30..120 frames by rank), `em10StayCk` limits how
many close in, `CNT_PARASITE` caps head parasites at two; a hit anywhere sets `Status_flg[1]` bit 29
+ `SeInfo.pos` and wakes every enemy within 15000..25000 units. Ashley targeting (`em10RouteTargetSet`,
flag 0x08000000; the catch chain `br_Catch` -> `NeckHang`/`Backhold`/`Bombhold`/`TakeAway`) drives
the `PlGacha*` button mashing. Room enemy-info (`EMI`) point types: 1 cover/perch/wander, 2 Del Lago
route, 5 take-away exit/waypoint, 0xA dog bark, 0xB partner route, 0xC return-to-post, 0xF ambush
start/goal/trigger, 0x10 no-climb.

## Player and weapons (`src/pl*`, `src/wep*`, `game/pl_*.cpp`)

`pl_leon.cpp` and friends implement `cPlayer` on top of `cEm`; `pl_wep.cpp` the weapon state
(`cPlWep`), `pl_dmg.cpp` damage and death, `act_btn.cpp` context actions (`cActionButton ActBtn`). Weapons
are REL modules `wepNN` each holding the weapon's `cObj` class plus the player routine object of its
class (`wep/pl_handgun.cpp`, `pl_shotgun.cpp`, `pl_rifle.cpp`, ...: ready/fire/reload states written
into the player's `Rno` bytes). `ItemWork::lv` packs the upgrade nibbles (fire << 12 | mag << 8 |
speed << 4 | ex), `bullet` = attribute << 13 | loaded rounds. The other player modules are the
playable/NPC characters (`pl0a` Krauser, `pl0d` Wesker, `pl0e`/`pl0f`/`pl11`/`pl14` Ashley, Ada and
the mercenaries).

## Items, merchant, sub screen

`item.cpp` (`cItemMgr`) keeps two inventory sets (`type` 0 Leon, 1 Ashley/Ada); `merchant.cpp`
merges stock and tune tables room by room on first pass (`RoomData.checkPassed`), prices are
table × 10 with a sell discount and 50% buy-back. The sub screen is the `Sscrn` REL (`ss_*.cpp`):
`SubScreenTask` builds Init/Main widget pairs per screen and links them; tabs are `menu_no` 0 key
items/treasures, 1 attache case (`ss_pzzl`: `pzlPlayer` pieces, PieceSelect/Command/Combine), 2 map,
3 files, 4 exit; `ss_shop` reuses the puzzle widgets to place bought items; `ss_cap` is the
bottle-cap collection; screens close through `scrn_out_func` and the next screen loads its
`SS/<lang>/ss_*.dat`.

## Rooms (`src/st*`)

One file per room, `RNNNInit` allocates the work, reads the room save flags (`RsfCheck(G_ROOM_ID, n)`:
one persistent bit per room; `Room_flg[0..2]` are the transient words, `Room_flg[0]` bit 31 = the
event was cancelled) and registers collision-area tasks, free tasks and item pickups
(`SceSetItemEvent` with open/opened callbacks). Cutscenes share one shape: `RsfSet` the once-flag ->
`SceEventStart` -> `SceSetEventCancel(1, EndProc)` -> `CamCtrl.CutCall` and motion waits ->
`EndProc`, which is also the cancel path and snaps every object to its final pose. Evd events run
through `EvtMgr.EvtReadExec` with an `Evt_RNNNSxx_Func(Event*)` callback on `funcMode` (0 setup, 1 per
cut, 2 end, 3 after). Enemies are spawned from the room's ESL by number (`cEmWrap::setEm`), in waves
gated by `SceCountEmAlive` and counters kept in room save flags. Chapter ends are `SceSetChapterEnd
(CHAPTER_X_Y)` in the room where the chapter ends (r106 1-1, r200 2-3, r204 3-1, r20b 3-2, r206 3-3/3-4,
r22b 4-1, r21d 4-2, r225 4-3, r22a 4-4, r316 5-2, r31c 5-3, r330 5-4). `st1_*` village, `st2_*` castle,
`st3_*` island (disc 2), `st4_0` Mercenaries / Assignment Ada. Shared helpers: `st/em_wrap.cpp`
(`cEmWrap` handle, `cEmPatrol`/`cEmGuard`/`cEmRouteRun` scripted movers) and `st/cSceObj.cpp`
(door/lever/lift movers).

## Events, messages, sound, video

`event.cpp` plays the packet-stream cutscenes (stream ids 0..0x20; a cut is one Cam/CamDammy packet;
`StatusFlag` bits: 0x40000000 tool fast-forward, 0x400 auto-fade, 0x200 died demo, 0x800 no player
reposition). `mes.cpp` renders messages with `LAYOUT_TYPE`. `snd*.cpp` is Capcom's AX-based sound
driver (SE requests `SND_REQ_WORK` with `prio/pan/vol/pitch`, BGM tables). Movies are the CRI stack in
`src/lib`: Sofdec (`sfd_*`, `mpv_*`, `mps_*`) demuxes and decodes MPEG video into a frame pool clocked
by the ADX audio track (`adx_*`, `sfd_adxt`, `sfd_tst` time stabiliser), fed from CVFS devices on the
DVD (`gcci`, `cri_cvfs`); the whole stack is single-threaded, driven by one `ADXM_ExecMain()` per frame.
ADX is only used for movie audio; the rest of the library is linked but dead.

## Debug tools (`src/tools`, `src/tools_mod`, `src/t_*`)

The debug build ships the developers' in-game editors as REL modules: every tool module ends with
`tools.cpp` (`ToolsTask` -> `DebugMenuSelected`), pauses the game through `TutilInitDefault`, draws
with `TprimDraw2D/3D`, and reads/writes its data on the host through the SN file server (`x:\soft`
server paths vs `d:\bio4` local). The room-area editors (`t_sce_at`, `t_sce_item`, `t_flr_at`, `t_se_at`,
`t_dr`, `t_block`, `t_esp_area`, `t_lightarea`) share one menu shape (area edit / data input / preview
live / load-save); `t_esp` is a windowed effect editor (`db_widget`/`db_window` on the pad, a 4×64
`TOOL_SEQ` record table, files `<model>_NN.EST`, `R<room>_NN.EST/.SST`); `t_camera`, `t_light`,
`t_event`, `t_id`, `t_movie`, `t_emlist` edit their namesakes. `cDbgToolMain<T>` (`dbg_tool.h`) is the
generic editor frame.

## Runtime (`src/lib`)

Nintendo SDK units are the dolsdk2004 sources. SN ProDG's runtime: `__start` (`.init`, calls
`SNDebugInit` when the boot block reports a debugger), the stub (`ppcdown`: exception vectors and the
EXI channel 2 host protocol, `proview`), host-file syscalls (`fileserver`, `dummy`, `FSasync`), a
DVD-drive emulation over the host (`sndvd`: DABR breakpoint on DICR), and `tealeaf` (CodeWarrior
runtime names — `__div2i`, `__cvt_fp2unsigned`, `__va_arg` — so the MWCC-built CRI objects link
against GCC's libgcc). `eabi`/`crt0`/`tors`/libgcc/libm are stock.
