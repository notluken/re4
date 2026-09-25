# Phase 3: endianness (docs/port.md, "Boot progress" continuation)

Read-only survey plus the first real fixes, 2026-09-24. Strategy (user decision, recorded in the
task brief this session started from): **option B, big-endian accessor types**. On-disc/relocated
data stays exactly as it is on disc (big-endian) in host memory at all times; struct fields that
are plain integers/floats are declared `BE<T>` (`include/port/be.h`), which swaps on every
read/write. Pointer fields keep `Ptr32<T>` (`include/port/ptr32.h`, Phase 2) unchanged. Rationale
(from the task brief): the engine relocates/unrelocates/slides data in place and round-trips it
through ARAM, and `cDvd` is format-agnostic, so a swap-at-load design would need to know every
buffer's format and guarantee exactly-once conversion; `BE<T>` makes that irrelevant by construction
(every read/write swaps, so it is always correct regardless of how many times the same bytes are
touched), and keeps on-disk/save data byte-for-byte GameCube-compatible.

## 1. `BE<T>` / `Ptr32<T>` design, as built

`include/port/be.h`: `BE<T>` wraps a `T`, physically stored in disc (big-endian) byte order;
`operator T()` swaps back to host order on every read, the `T` constructor/`operator=` swap on
every write, `+=`/`-=`/`*=`/`/=`/`&=`/`|=`/`^=`/`++`/`--` all round-trip through `T` rather than
touching the swapped storage directly. `sizeof(BE<T>) == sizeof(T)`, same alignment, trivially
copyable (checked by `static_assert`s in the header and re-checked in `tests/port/test_be.cpp`,
wired into `ctest`). Aliases `be_u8`/`be_s8`/.../`be_f64`. **No custom `operator==`/`!=`** (against
`T`, another `BE<T>`, or a free-function form) -- tried first, and found to create a genuine overload
ambiguity the moment `T` and a plain-integer-literal comparison's promoted type differ (`u32` is the
host's 8-byte `long` under `RE4_U32_32=OFF`, so `tpl->numDescriptors == 0` is ambiguous between a
member `operator==(T)` and the built-in `==` reached through the already-present `operator T()`
conversion) -- exactly the same class of regression `include/port/ptr32.h`'s own `operator[]`
comment already documents for `Ptr32<T>`. Fixed by not declaring a second candidate at all: the
implicit conversion operator alone lets every `==`/`!=` call site already in this codebase resolve
through the built-in operator, unambiguously.

**Pointer fields (Ptr32 vs. a new BEPtr32): kept `Ptr32<T>` exactly as Phase 2 left it, no new
type.** Verified first: every current `Ptr32<T>` use *is* an on-disc pointer field (`grep -rl
"Ptr32<" include/*.h` -- `model.h`, `tpl.h`, `cam_ctrl.h`, `game.h`, `room_jmp.h`, `atari.h`,
`main_mem.h`, `global.h`, all `// 0xNN`-commented on-disc struct members). But a pointer field's
4 bytes mean two different things at two different times, and only one of those times is a genuine
"read this on-disc big-endian value" case: **before** relocation it is a big-endian file-relative
byte offset straight off disc (never itself swapped -- only the header's plain integer fields are
`BE<T>`'d, the buffer holding the pointer field is not swapped in bulk); **after** relocation
(`cTexSys::CalcTplAddr`-shaped code overwrites the field in place with a real pointer, via
`Ptr32<T>`'s constructor computing a runtime, host-native compressed arena handle, Phase 2) it is a
value *computed at runtime*, already host-native, never itself loaded from disk again. Making
`Ptr32<T>`'s storage itself swap-on-every-access (a `BEPtr32<T>`) would swap the pre-relocation raw
offset correctly on its first read, but would then also swap the post-relocation, already-correct,
host-native handle on every subsequent read -- corrupting it. Concretely hit and fixed this exact
bug this session (section 3 below): `MessageControl::loadSystemFont()`'s non-Japanese branch calls
`MessageFont::create()` a **second time** on the *same, already-relocated* buffer (slot 0's), and a
naive "always swap the raw offset before checking/using it" design corrupts the relocated pointer on
that second call, because the same 4 bytes are being read for two different purposes (guard vs.
arithmetic) that need different treatment depending on whether relocation has already happened.
`Ptr32<T>` instead gained one new, explicit, read-only accessor for this one legitimate use:
`raw_handle_be()` (`include/port/ptr32.h`) -- swaps `m_handle`'s bytes on demand, for the specific
call sites that need the *numeric value* of a still-unrelocated raw offset for pointer arithmetic
(`(u8*) addr + field.raw_handle_be()`). The **sign-bit "already relocated?" guard** that gates
whether relocation code runs at all must stay on the *unswapped* `raw_handle()` (or the existing
explicit `u32`/`int` cast) -- a small on-disc offset's top disc-order byte is always 0 regardless of
which order it's read in, and a relocated arena handle's top bit is set in its actual (host-native,
unswapped) storage; swapping that check would misread a live pointer's top byte and re-trigger
relocation on it. This is the one non-obvious wrinkle of option B for pointer fields; it did not
show up until the second real call site was reached (see section 3).

Tests: `tests/port/test_be.cpp` (round-trips u8/u16/u32/s32/u64/f32/f64 through raw big-endian byte
buffers, compound assignment/increment/decrement, an `OnDisc`-shaped struct's `offsetof()`s), wired
into `CMakeLists.txt` next to `test_ptr32`/`test_arena`. `ctest`: 4/4 pass under `RE4_U32_32=ON`
(`test_ptr32`, `test_arena`, `test_be`, `re4_port_static_asserts`); 3/3 under the default
`RE4_U32_32=OFF` (`re4_port_static_asserts` is gated off there, by design -- see
`tools/port/gen_static_asserts.py`'s own comment, unchanged this pass).

## 2. Survey: on-disc structs used by the boot path first

| Format | Struct(s) | Header | Endianness status before this pass | This pass |
|---|---|---|---|---|
| TPL texture/palette | `CLUTHeader`, `TEXHeader`, `TEXDescriptor`, `TEXPalette` | `include/tpl.h` | Pointer fields `Ptr32<T>`'d (Phase 2); plain fields (`numEntries`, `format`, `height`/`width`/`wrapS`/`wrapT`/`minFilter`/`magFilter`/`LODBias`, `version`, `numDescriptors`) still raw, unswapped | **Converted to `BE<T>`** (all except the single-byte fields, which need no swap) |
| Font container | `MesFontFile` (`tplOfs`, `widthOfs`) | `src/game/mes.cpp` (local to the .cpp, not a shared header) | Raw, unswapped -- not even in Phase 2's `Ptr32<T>` inventory (small enough it was missed) | **Converted to `BE<u32>`**; `gen_static_asserts.py` extended to extract a struct body straight out of a `.cpp` (`INLINE_STRUCTS`) rather than only `#include`-ing a shared header, so this gets the same `offsetof()` check as the rest |
| Room table | `CRoomInfo` | `include/room_jmp.h` | Pointer fields (`name`, `person`, `person2`) `Ptr32<T>`'d (Phase 2); **TO VERIFY** whether it has any plain integer/float fields left unswapped -- not read this pass, out of the immediate crash chain |
| DVD size table | (name/layout still **TO VERIFY**, docs/port-boot.md section 3) | -- | Not converted, not reached by any run this pass (`etc/sizetbl.dat`'s read reports "DVD: Read Error!!!" many times before eventually succeeding on the boot log -- **TO VERIFY** whether that is itself an endianness-adjacent bug or an unrelated retry-loop artifact; not chased this pass, table format itself unread) | Not touched |
| Title archive (model BIN + TPL, `ARC_PTR`) | `cModelData` (`include/model.h`) + the TPL structs above | `include/model.h`, `include/tpl.h` | `cModelData`'s pointer fields `Ptr32<T>`'d (Phase 2); plain integer/float fields (`weight_palette_num`, `nParts`, `displist_num`, `flags`, `nTex`, `shift`, `weight_ext_num`, `nVtx`, `nNrm`, `version`) **not yet BE<T>'d** -- not reached this pass (crash chain stopped at `SndBgmTblInit`, before the title archive's model data is touched), left for whoever reaches that point next |
| Sound BGM table | `SndBgmTbl`, `SndBgmRoom`/`SndRoomSave` (`src/game/snd.cpp`, `list_ofs`/`room_ofs` raw offsets, `bgm`/`str` arrays) | `src/game/snd.cpp`/`include/snd_drv.h` (not fully read this pass) | Not converted -- moot for now: `SndMem.bgm_tbl` itself is null because `SndInit()` is stubbed under `TARGET_PC` (docs/port-boot.md section 15, Phase 5 deferral); the new crash this pass (section 3) is that stub's consequence reaching a second call site (`SndBgmTblInit()`, called from `systemRestartInit()`, not gated the same way `SndInit()` is), not a byte-swap bug -- **TO VERIFY**/Phase 5 territory, not fixed this pass |

## 3. Milestones reached this pass, with fixes (one line each)

1. **`TEXPalette::numDescriptors` reads as 0** (docs/port-boot.md section 21's blocker,
   `MessageFont::create()`'s loop never running, `t->pTex` staying null) -- `include/tpl.h`'s TPL
   structs converted to `BE<T>` for every plain integer/float field.
2. **Past that, into a null/garbage `TEXHeader*`** -- `MesFontFile::tplOfs`/`widthOfs`
   (`src/game/mes.cpp`, not in Phase 2's original `Ptr32<T>` inventory) were still raw u32; converted
   to `BE<u32>`.
3. **Past that, into `Ptr32<TEXHeader>::raw_handle_be()` dereferencing garbage** -- the actual
   pointer-arithmetic offsets (`descriptorArray`, `textureHeader`, `CLUTHeader`, `data`) were being
   read with the unswapped `raw_handle()`/implicit cast; added `Ptr32<T>::raw_handle_be()`
   (section 1) and used it at each arithmetic call site in `MessageFont::create()`
   (`src/game/mes.cpp`, `#ifdef TARGET_PC` only).
4. **Past that, into the same corruption on the *second* `create()` call** (the non-Japanese system
   font reusing slot 0's already-relocated buffer) -- the sign-bit "already relocated?" guard must
   stay on the unswapped `raw_handle()`, not `raw_handle_be()` (section 1's design note); fixed by
   splitting the two reads.
5. **`MessageControl::init()` now completes end to end** -- both font slots load, `systemStartInit()`
   returns, `main_game()` reaches `systemRestartInit()`.
6. **New blocker, `SndBgmTblInit()` dereferencing a null `SndBgmTbl*`** (`src/game/snd.cpp:206`,
   called from `SndInit2()`/`systemRestartInit()`) -- consequence of `SndInit()` being stubbed under
   `TARGET_PC` (Phase 5 deferral, docs/port-boot.md section 15): `SndMem.bgm_tbl` is never populated.
   Not fixed this pass -- Phase 5 (sound) territory, not an endianness bug; stopping here per the
   "stop at a design decision" instruction (un-stubbing sound, or stubbing `SndBgmTblInit()` too, is
   a call for whoever picks up Phase 5, not a Phase 3 byte-swap fix).

No window opens this session (the crash chain still stops before any GX/VI frame submission, per
every prior boot-progress entry) -- no screenshot.

## 4. Rough counts across the whole tree (heuristic, not exhaustive)

- **On-disc structs** (headers with at least one `// 0xNN`-commented field, `include/*.h` +
  `include/charPipeline/*.h`): **161 files**. This is a file count, not a struct count (several
  headers define more than one on-disc struct, e.g. `tpl.h`'s four); no per-struct tally attempted
  this pass -- **TO VERIFY** exact struct count, would need the same body-extraction
  `gen_static_asserts.py` already does, generalized to *find* candidate structs rather than being
  told their names.
- **Bitfields inside on-disc structs**: grepping for C bitfield syntax (`: N;`) across
  `include/*.h`/`include/charPipeline/*.h` found 7 raw hits; 5 of those are `chk(u32 bit)`/
  `chkFlag(u32 b)`-style **methods** taking a bit *mask* argument (not C bitfields at all -- a
  false-positive grep match on the colon). The remaining 2 (`include/db_cam.h`'s `pad_bits`/
  `info_disp`/`pad_bits2`/`along_xyz`/`pad_bits3`, all in one struct) belong to a **debug-tool
  struct that lives only in memory** (the in-game camera-debug tool's own state), not an on-disc
  format -- PPC-vs-arm64 bitfield bit-order does not apply to it. **No genuine on-disc-format C
  bitfield found this pass.** **TO VERIFY**: this was a text-pattern grep across headers only, not a
  semantic scan of every on-disc struct's body, and did not look inside `.cpp`-local structs
  (`MesFontFile`'s sibling case, section 2) or REL-module headers.
- **Unions in headers**: 27 files contain a `union` (`grep -rl "^\s*union\b"`); none inspected this
  pass for whether the union is itself part of an on-disc format (vs. an in-memory reinterpretation
  helper) -- **TO VERIFY**, open work.
- **Raw pointer-cast reads** (`*(u32*)`/`*(u16*)`/`*(s32*)`/`*(s16*)`-shaped dereferences,
  `src/game/*.cpp` + `src/lib/*.c` + `src/st*/*.cpp`): **53 hits** (heuristic pattern, will both
  over- and under-count -- doesn't catch `memcpy`-into-local reads or `u8*` arithmetic followed by a
  separate dereference, and may double-count multi-line expressions). Not triaged file-by-file this
  pass.
- **Files that call `DvdReadN`/`DVDReadAsync`/`DVDRead(` directly**: 19 (`src/game/*.cpp`) -- every
  one of these is a candidate "load-time" site where a per-format swap (if this project ever adds
  one, on top of the `BE<T>`-at-every-access approach) would need to run exactly once; not needed
  under option B (every access already swaps), listed here only because it's the complete list of
  "a on-disc buffer enters memory here" call sites, useful for whoever audits format coverage next.

## 5. Aurora GX texture/vertex payload endianness (survey question, answered for textures)

**Textures/palettes: Aurora's own GX reimplementation already expects and un-swaps GameCube
big-endian texel/CLUT data internally** -- confirmed by reading `../aurora/lib/gfx/texture_convert.cpp`
directly: `decode_texel()` and its CMPR/IA/RGBA-family sibling functions call `bswap()` on every
raw texel/CLUT-index read (`bswap(in[x])`, `bswap(*reinterpret_cast<const uint16_t*>(src))`, a
"CMPR difference: Big-endian color1/2" comment on the two 16-bit color fields inside each CMPR
block). This means: **pixel/palette payload bytes must NOT be pre-swapped** by this port's own
code -- `TEXHeader::data`/`CLUTHeader::data` (the `Ptr32<u8>` fields pointing at that payload) are
left exactly as Phase 2 built them, and `include/tpl.h`'s new comment says so explicitly. Only the
*header* fields (`width`/`height`/`format`/`numEntries`/...) needed `BE<T>` this pass, never the
payload itself -- pre-swapping it would double-swap once Aurora's own texture-conversion path runs.

**Vertex data: unresolved, marked TO VERIFY.** No `bswap`/bit-swap call found in
`../aurora/lib/dolphin/gx/GXDraw.cpp`/`GXVert.cpp`/`GXManage.cpp` (grepped this pass) the way
`texture_convert.cpp` has for texels -- but that is inconclusive by itself: on real hardware, vertex
submission through `GX_Position3f32`-family macros is a plain big-endian CPU store straight into
`GXWGFifo` (the FIFO's own bytes are big-endian by construction, no separate swap step exists on
real hardware to find). This port's `GXWGFifo` is currently a 1-element-array stub
(docs/port-boot.md section 7's Aurora-mismatch survey) -- real vertex submission has not been
reached by any run yet (every crash so far predates `Render`/any GX draw call), so whether Aurora's
*own* vertex/display-list decode path expects big-endian FIFO bytes (matching real hardware, no
extra work needed from this port) or host-native ones (needing this port's own vertex-write macros
to swap under `TARGET_PC`) could not be checked against a live code path this pass. **TO VERIFY**
once `Render_init`/an actual `GXWGFifo`-writing call site is reached; the answer likely lives in
whichever Aurora file decodes the FIFO stream on the receiving end (not yet located/read this pass).

## 6. Correction: `Ptr32<T>` storage made uniformly big-endian (coordinator review, 2026-09-24)

Coordinator review of section 1's original design found a genuine latent bug, confirmed by
inspection (not by a new crash): with the original split -- unrelocated fields BE (raw disk bytes,
untouched), relocated fields host-native (computed at runtime, stored unswapped) -- **the
"already relocated?" sign-bit guard used throughout this codebase
(`(s32) field >= 0` / `(s32) field.raw_handle() >= 0`) is wrong for any raw offset whose low byte is
>= 0x80.** A small on-disc offset like `0x000000C0` is stored in memory as bytes `00 00 00 C0` (its
semantic MSB -- always zero for a small offset -- at the lowest address, BE convention); reading
those same 4 bytes as a native little-endian `s32` reinterprets the *last* byte (`0xC0`, the
offset's actual LSB) as the sign byte, giving `0xC0000000` -- negative, a false "already relocated".
This only went unnoticed because every offset this session's runs happened to exercise had a low
byte < 0x80. The same split also meant the inverse-relocation direction
(`RelocPtrToOffset`/`SlidePtr`, `src/game/model.cpp`; the save-data equivalent in
`src/game/game.cpp`) wrote a plain host-native numeric offset back through `FromRaw()` unswapped --
so a relocate -> unrelocate -> relocate round trip (or a save write) would silently flip byte order
relative to a real GameCube's, breaking save compatibility.

**Fix**: `Ptr32<T>`'s storage (`include/port/ptr32.h`) is now *always* big-endian, whether the field
currently holds a raw pre-relocation offset or an already-relocated handle -- exactly like a real
GameCube's own big-endian CPU, which never had two interpretations to begin with. `load()`/`store()`
(private, swap on every access) are the only place the swap happens; every public accessor
(`raw_handle()`, `operator T*()`, the explicit `u32`/`int`/`s32` casts, `FromRaw()`) goes through
them. The since-added, since-removed `raw_handle_be()` workaround (section 3's original fix) is
gone -- it was correct for the one call site it touched (`MessageFont::create()`) but left every
*other* `raw_handle()` user in the tree (`cam_ctrl.cpp`, `card.cpp`, `game.cpp`, `model.cpp`,
`room_jmp.cpp`, `texture.cpp`, `trans.cpp`) still broken, exactly the "systematic, not ad hoc"
project rule this correction restores. No call site needed editing besides `mes.cpp`'s revert back
to plain `raw_handle()` (matching every other call site's existing style) -- the class-wide fix is
what makes all of them correct at once.

**Verification**: `tests/port/test_ptr32.cpp` gained a round-trip test -- a synthetic on-disc buffer
with a raw offset whose low byte is `0xC0` (exactly the failure class), taken through relocate ->
sign-check -> unrelocate -> byte-identical-to-original-bytes check -> relocate again -> same real
pointer as the first time. `ctest`: 4/4 (`RE4_U32_32=ON`), 3/3 (default `OFF`). `re4_boot` re-run:
reaches the same frontier as before the fix (font/message system still completes correctly),
confirming no regression from the correction. Remote byte-identity: unchanged (`ptr32.h` is
`include/port/`, exempt from the byte-matching gate, but re-verified anyway alongside the other
changes in this pass -- see the commit log).

## 7. Sound made inert under `TARGET_PC` (coordinator's plan: "sound off for first boot", systematic)

Audited every `SndMem`/`pSnd`-touching statement in `src/game/snd.cpp` (its own file is the entire
public API boundary -- `include/snd.h` declares ~62 `Snd*` entry points, every one of them defined
in this one file) plus `src/game/se_at.cpp` (`SeAt*`, the other four boundary functions `snd.h`
declares) for unconditional dereferences of state `SndInit()`'s `TARGET_PC` stub never populates.

**Finding: most of this code is already safe.** The vendor's own logic gates almost everything on
`pSnd->blk_flag` (`SND_BIT_CK`/`sndExistCheck`) or a null header check (`se_at.cpp`'s
`Snd.pSeAtHeader == 0`, `sndVolCalc`'s `pSnd->hdr == NULL`, ...) -- a zeroed `SndWork`/`SndMemWork`
(the stub's `memclr_asm`) naturally fails every one of these the same way "block not loaded on real
hardware" would, so `SndCall`, `sndVolCalc`, `sndPitchCalc`, all of `se_at.cpp`, and the large
majority of the ~62 boundary functions need **no** change at all. `SndStrReq`'s 6-argument overload
already has an explicit vendor-provided "missing data" gate (`str_flag`, the `"SND: No STR Header."`
branch) -- `SndInit()`'s stub now sets `str_flag = 0` (one line) so that existing gate does its job
instead of being silently bypassed by its file-scope initializer (`int str_flag = 1;`).

**Genuinely unguarded (found by exhaustive audit of every `SndMem.{bgm_tbl,door_tbl,bgm_file,
str_file,blk_mram}` access in the file, not by waiting for each one to crash)**: real hardware never
needed a "did this fail to load" check for these, so none exists. Stubbed individually, at entry,
under `TARGET_PC`, with a return value matching the function's own documented contract:

| Function | Dereferences | Stub return |
|---|---|---|
| `SndBgmTblInit()` | `SndMem.bgm_tbl->list_ofs` | (void) |
| `SndDoorSeLoad()` | `SndMem.door_tbl->num_ofs` | `-1` ("no read request", already documented) |
| `SndBgmTblSet(u16, int)` | `SndMem.bgm_tbl->list_ofs`/`room_ofs` | `0` ("not found", already documented) |
| `SndBlkInit(int, int, int)` | `SndMem.blk_mram[blk]` (`*(u32*) adr`) | (void) -- also means `pSnd->blk_flag`'s bit for that block is never set, correctly leaving it "still not loaded" for every caller that *does* check the flag |
| `SndBgmLoad(int)` | `SndMem.bgm_file[bgm_no]` | (void) -- `SndBgmDataReadCheck()`'s own guard returns "free slot" (not `-1`) precisely when nothing is loaded, so it does not stop this one |
| `SndDriverInit()` | Real AX/CRI driver setup (`Snd_system_init`, `AXFXSetHooks`, ...), no Aurora backend (Phase 5) | (void) |
| `SndSystemReset()` | Calls `SndDriverInit()` plus `SEQQuit`/`SYNQuit`/`AXQuit`/... (same Phase 5 driver calls) | (void) |

FMV (Sofdec/mwPly): already deferred (`docs/port-boot.md` section 2/6, `SofdecInit()`/
`init_dbmodule()`), not touched this pass -- no FMV entry point has been reached by any run yet.

**Verified**: `re4_boot` now runs past `systemStartInit()`/`systemRestartInit()` entirely, through
`TaskExec(Title_task)`, into the first frame loop (`main_game()`'s `for(;;)`, `main.cpp:140`) --
past every sound-related call on the boot path. Default host build unaffected (`re4_game_all -k 0`
failing-file list unchanged, 34 files, diffed directly against the pre-session baseline); `ctest`
still green.

## 8. Current blocker: `DrawOTag` dereferences a garbage 2D ordering-table pointer (2026-09-24)

```
Stack overflow in Thread 0 !!  (OSPanic, logged but non-fatal here -- a stub, TO VERIFY if that's safe)
* thread, EXC_BAD_ACCESS, address=0x180cf769c
  frame #0: DrawOTag(pOt=0x180cf769c) at libgpu.cpp:98 -> } while (*pOt != 0xFFFFFFFF);
  frame #1: main_game() at main.cpp:140 (the frame loop, first iteration)
```

**Diagnosis (not root-caused this pass)**: this is downstream of `InitOt()`/`Render`/`Render_before`
(all still logging-only stubs, docs/port-boot.md section 7's Aurora-mismatch survey -- no real GX
backend wired yet, Phase 4 scope) -- the 2D ordering table `DrawOTag` walks is either never
initialized to a valid empty-list terminator by these stubs, or `Trans()`/whatever populates
per-frame OT entries writes through a code path this session did not trace. Not an endianness bug
(no on-disc structure is involved here -- the OT lives entirely in runtime-allocated MRAM/arena
memory) and not sound -- this is squarely Phase 4 (render) material, plus the separate
"Stack overflow in Thread 0" `OSPanic` that fired moments earlier and evidently did not actually
abort the process (**TO VERIFY** whether that is itself masking a real problem, e.g. a scheduler
task/stack-size mismatch under `TARGET_PC`'s host `pthread`-based `CreateArenaThread`, or a harmless
stub). Stopping here per the "stop at a design decision" instruction -- reaching real GX submission
is Phase 4's scope, not Phase 3's.

No window renders this session (still predates any actual GX/VI frame submission) -- no screenshot.

## 9. What's next

- Sound is now inert (section 7) and `cModelData`'s plain fields are `BE<T>`'d (section 6's fix
  applies uniformly; `include/model.h` converted this pass) -- neither blocks boot progress anymore.
- Section 8's `DrawOTag`/render blocker is next -- Phase 4 (real Aurora GX submission) territory,
  not endianness.
- `include/room_jmp.h`'s `CRoomInfo` plain fields (if any -- not confirmed this pass) and the DVD
  size table's still-unknown layout are both still open, not yet reached by any run.
- A systematic (not per-crash) sweep of every remaining `// 0xNN`-commented struct for un-`BE<T>`'d
  plain fields (title archive's TPL is covered via `tpl.h`, but nothing downstream of section 8's
  blocker has been reached yet to know what else it touches) is still open.

## 10. OSPanic made fatal, and a real OSThread implementation (coordinator review, 2026-09-24)

Two follow-ups from the coordinator's review of section 8's blocker.

### OSPanic now aborts

`OSPanic()` (`src/port/stubs/generated_c_stubs.cpp`) was a logging-only stub -- a real call means
the game itself detected an unrecoverable condition (`ASSERTMSGLINE`, scheduler.cpp's stack-overflow
guard) and never returns on real hardware; letting it fall through silently meant everything downstream
ran in a state the vendor explicitly gave up on. Now logs the file/line/message, flushes, and calls
`std::abort()`.

### Root cause of "Stack overflow in Thread 0": `OSCreateThread` was a complete no-op

With `OSPanic()` now fatal, the crash it was hiding needed a real fix, not a louder log line.
`OSCreateThread`/`OSResumeThread`/`OSSuspendThread`/`OSSleepThread`/`OSWakeupThread`/
`OSGetCurrentThread`/`OSExitThread` (`src/game/scheduler.cpp`'s entire cooperative task-scheduler
API) were **all** pure logging stubs -- `OSCreateThread` in particular just returned 0 without ever
starting the given function or writing the `OS_THREAD_STACK_MAGIC` guard word a real implementation
sets at the low end of the new thread's stack. That guard word is exactly what
`scheduler.cpp::StackOverflowCheck()` reads -- since no task's thread had ever actually run, the
word was never written, and the very first task dispatched (`Title_task`, slot 0) tripped the
"stack overflow" check on frame 1, every run, deterministically. **No task's code had ever executed
even once** before this pass.

`include/port/os_thread.h` / `src/port/os_thread.cpp` (new): a real, from-scratch OSThread
implementation on top of host `std::thread`s. Full design rationale is in the file's own header
comment; the short version: real hardware gives the scheduler mutual exclusion (only one task's
code genuinely running at a time) through true preemptive scheduling with priorities, which cannot
be reproduced at instruction granularity with plain host threads. Instead, a single explicit
"whose turn is it" token (`g_holder`, a condvar-guarded `OSThread*`) is passed by hand:
`OSResumeThread`/`OSWakeupThread` stay **non-blocking**, exactly like the real API (this matters --
`TaskSleep`/`TaskExit`/`TaskChain` call `OSResumeThread(pParentThread)` themselves to hand control
back, and if that call blocked its caller, the caller -- often the task about to sleep or exit --
would deadlock); every thread calls `BecomeRunner(self)` (block until `g_holder == self`) whenever
it's about to actually execute task code, fresh or woken; and the **driving** thread (real
hardware: main, running `TaskSchedulerMain`) has no way to know when the task it just resumed hands
control back, so a new, `TARGET_PC`-only call, `WaitForHandback()`, is added at the three call sites
in `scheduler.cpp` right after its own `OSResumeThread`/`OSWakeupThread` calls -- this is this host's
explicit stand-in for the preemption/priority mechanism real hardware uses implicitly.

**Verified working**: with this in place, `Title_task` genuinely runs for the first time -- the
boot log shows real game-logic stub calls (`cGameSave::alloc()`, `ItemMgr.init()` (not stubbed, real
code), `CoreDataRead()`, `OptionDataRead()`, `ScreenReSize()`) that were never reached before. Traced
with instrumented builds (temporary, not committed) confirming the handoff protocol itself works
correctly: `main` genuinely blocks in `WaitForHandback()` for the task's entire turn and only
proceeds once the task hands control back via `TaskSleep`/`TaskExit`'s own `OSResumeThread` call --
this was checked directly against the alternative hypothesis (a synchronization race letting main
run concurrently with the task) and ruled out; the two threads do not overlap.

`ctest`: 4/4 (`RE4_U32_32=ON`) / 3/3 (default). `re4_game_all -k 0` failing-file list unchanged (34
files, identical set). `re4_boot` reaches the same crash frontier as before this fix with no
regression (confirming the cleanup of temporary debug instrumentation didn't change behavior).

### New blocker found downstream: a real allocation corrupts an unrelated global

With tasks actually running, a **new**, previously-unreachable bug surfaced: `Task[1]` (the second
scheduler slot, otherwise untouched -- nothing calls `TaskExec(1, ...)` this early) is observed with
`Status == TASK_EXEC` and `hook`/`pFunc == NULL` by the time `TaskScheduler()`'s per-frame loop
reaches it, immediately after dispatching slot 0. Since `TaskExec()`/`TaskChain()` are the only
places that ever set `Status` to `TASK_EXEC`, and both **always** set `hook = TaskExec_hook`
unconditionally first, `hook == NULL` proves neither ran for this slot -- something wrote directly
into `Task[1]`'s memory instead.

Traced with temporary instrumentation (not committed): confirmed `TaskSchedulerInit()` correctly
zeroes `Task[1]` (`Status == 0` right after init); the corruption happens during `Title_task`'s very
first turn, before it even reaches its own state machine (`titleFuncTbl[0]`/`titleInit()`) --
narrowed to somewhere in `pSaveData = GameSave.alloc(); ItemMgr.init(); CoreDataRead();
OptionDataRead(); w = MEM_CALLOC(sizeof(TitleWork), 1, 13);`. `GameSave.alloc()`/`CoreDataRead()`/
`OptionDataRead()` are confirmed inert (logged `STUB: ... called`, do nothing). `w` itself is a
real, correctly-allocated pointer nowhere near `&Task[1]` (checked directly -- ruled out a
heap-aliasing coincidence with `TitleWork::Rno0` writes, the first hypothesis). `cItemMgr::init()`
(`src/game/item.cpp`) is **not stubbed** -- real code, allocates three buffers via
`MEM_ALLOC(..., 1, 13)` (heap argument `13` == `MEM_HEAP_CURRENT`, the normal "current heap"
idiom, confirmed not itself a bug) and writes `p->flags = 0` in a 0x180-iteration loop over one of
them -- the leading suspect, not yet confirmed: either `OSAllocFromHeap` (Aurora's real
implementation) is returning a pointer that doesn't actually own 0x180 `sizeof(ItemWork)`-sized
slots' worth of memory, or a size/count is computed wrong somewhere in this call chain, and the
resulting out-of-bounds write happens to land in `Task[]`'s memory. **Not root-caused this pass** --
budget-limited; whoever picks this up next should instrument `cItemMgr::init()`'s three
`MEM_ALLOC` calls' return pointers and sizes directly, and check them against the heap's actual
bounds (`Heap[CurrentHeap]`), rather than continuing to bisect from the scheduler side.

No window renders this session (crash still predates any GX/VI frame submission) -- no screenshot.
Sections 2 (Render/VI/GX SDK parity, real render-path units) and the `CRoomInfo`/DVD-size-table BE
audit the coordinator also asked for were not reached this pass -- the OSThread root-cause fix and
this new blocker took the full remaining budget.

## 7. Room data formats -- yz2 unit test, `ConsRoom`/`cSmd` endian bugs found and fixed, R120 room
   loop reached

### yz2 test (`tests/port/test_yz2.cpp`)

`src/game/yz2code.cpp`'s `TARGET_PC` decode loop (added `c4b1a979`) had no automated test. Added
`tests/port/test_yz2.cpp` (wired into `ctest`): links the real `src/game/yz2code.cpp`, and for four
real room archives of different sizes/stages (`st1/r120.das` ~1.5 MB, `st1/r117.das` ~3.3 MB,
`st2/r200.das` ~2.4 MB unpacked... see the test for exact figures, `st4/r400.das` ~5.4 MB -- the
smallest-to-largest room archives on disc 1) decodes the same bytes two ways and compares
byte-for-byte: `tools/motion/yz2.py`'s independent Python reference decoder (via the new
`tools/port/yz2_extract_room.py`, which walks the disc's own FST to pull each room archive's raw
yz2 stream straight out of a disc image with no external dependency beyond the stdlib) and
`Yz2DecodeSet`/`Yz2DecodeExec` themselves. Needs a real disc image (`orig/G4BE08/re4_debug_disc1.iso`
or `*.gcm`) at test time -- **SKIPs gracefully** (one stderr line, exit 0) when neither is present,
per the port rules (never commit disc data or anything derived from it: both extracted files are
written under the CMake binary dir, gitignored, not the repo). `ctest --test-dir build-pc -R
test_yz2`: pass, all four rooms byte-identical to the reference. `re4_game_all -k 0`: unaffected
(yz2code.cpp is not in the 25-file baseline).

An encoder-based test (task brief's preferred option) was not attempted: matching the vendor's
adaptive range coder bit-for-bit on the *encode* side (carry propagation through the same
renormalisation thresholds the decoder reverses) is a second, independent reverse-engineering
project on top of the decoder port already done, and the task brief itself allows the real-room-file
fallback when an encoder is impractical -- taken here given the session budget.

### Room archive (`stN/rNNN.das`) sub-file formats -- the tags `GetDataExt()` resolves

A room archive on disc is `[0x400-byte container header][yz2 stream]`; `read.cpp`'s `decodeData()`
decompresses the yz2 body into `pG->pRoom`, and every sub-file below is then looked up by 4-byte tag
through `GetDataExt()` (`read.cpp:1009`, `DataExtHeader` -- already `BE<u32>`'d, Phase 3 section 2's
own table). Every room in this checkout's disc 1 exposes at least `RTP`/`MDT`/`OSD`/`EMI`
(`read.cpp:204-207`); the rest are looked up from `game.cpp`'s room-start sequence (`R120Init`'s
generic counterpart) or the subsystem that owns them. Audited this pass (loader + struct + current
`BE<T>` status); **not exhaustive** -- `st1/r120.das` alone carries 13 tagged sub-files per
`yz2code.cpp`'s own comment, some of which (`RTP`, `OSD`, door/floor/effect tables not listed by
tag name anywhere near `game.cpp`) were not traced to a struct this pass.

| Tag | Loader (file:line) | Struct (file:line) | `BE<T>`/`Ptr32` status |
|---|---|---|---|
| `CNS` | `game.cpp:384` `ConsInitRoom` | `cons.cpp:11` `ConsRoom` | **Fixed this pass**: `num`/`bits[]` (and the `values[]` view over the same array) were entirely raw `u32` -- `re4_port::BE<u32>` under `TARGET_PC` now. This was the actual crash the task brief pointed at: `ConsGetRoomValue(CONS_R_NMODELINFO)` (feeding `cModInfoMgr::create`'s array count) read `r->num` as a garbage host-native value, so almost every id either fell through to `ConsRoomDefault[]` or read `values[id]` off a wrong array base. |
| `SMD` (main + common, `no` 0/1) | `game.cpp:386,388`, `block.cpp:542,643` | `scroll.h:9` `SmdWork`, `scroll.h:27` `cSmd` | **Fixed this pass**: `cSmd::nModel`/`BinTblOfs`/`TplTblOfs`/`MotTblOfs`/`grp.nGroup`/`grp.num[]` were raw; now `BE<u16>`/`BE<u32>`. `SmdWork::pos`/`rot`/`scale` were plain `Vec` (raw `f32` triplets read straight off disc, same class of bug as `id_sys.h`'s `IdData::pos`/`vtx`/`rot` before Phase 2/3); now `SmdVec` (new, same `BeVec` pattern as `id_sys.h`/`room_jmp.h`). `SmdWork::flags`/`.b.attr` union: `flags` is now `BE<u32>`, `.b.attr` (byte 3, the *physical* last-address byte on both a real GameCube and this host, since `BE<T>` never moves bytes) is untouched -- same "single-byte union view needs no swap" reasoning as `CRoomInfo::.stage`/`.room` (docs/port-boot.md section 49). This closed `SmdGetObjNum()`'s own contribution to the same array-count bug (`n = ConsGetRoomValue(...) + SmdGetObjNum()`, `game.cpp:391` etc.) -- `SmdGetObjNum()` returns `nScrWork = pSmd->getWorkNum()`, which reads `nModel`/`grp.nGroup`/`grp.num[]` directly. |
| `SMX` | `game.cpp:387` | `scroll.h:52` `SmxWork`, `scroll.h:66` `cSmx` | **Not converted** (`SelectMask`/`flags`/`color`/`color2`/`uvScrollU`/`uvScrollV` all raw) -- **TO VERIFY**, not reached by this pass's crash chain but same bug class as `SmdWork`. |
| `LIT` (`no` 0/1) | `game.cpp:421-422` | `light.h:221` `cLit` | Already `BE<u16>`/`BE<u32>` (Phase 3, earlier session, `9c4b303c`). |
| `SAT` | `game.cpp:444` | `atari.h` (`cSatMgr`/piece structs, not individually re-audited this pass) | **Not converted** -- raw, **TO VERIFY**. |
| `EAT` | `game.cpp:454` | `atari.h`/`at_sub2.h` (`cEatMgr`) | **Not converted** -- raw, **TO VERIFY**. |
| `AEV`/`ITA` | `game.cpp:467` `SceAtInit` | `sce_at.cpp` (not traced to a header struct this pass) | **Not converted** -- **TO VERIFY**. |
| `SHD` | `game.cpp:477` | `shadow.h:56` `ShdHeader` | **Not converted** -- raw, **TO VERIFY**. |
| `EFF` | `game.cpp:478`, `emdata.cpp:97` | not traced to a header struct this pass (`EffData`-shaped name not found under `include/`) | **Not converted** -- **TO VERIFY**, loader itself not located. |
| `EAR`/`SAR` | `game.cpp:481,484` | not traced this pass | **TO VERIFY**. |
| `TEX` | `game.cpp:487` | likely `tpl.h` (already `BE<T>`'d, Phase 3 section 2) via the same `TEXHeader` family -- **not confirmed this pass** that this call site is the same struct family. | **TO VERIFY**. |
| `ITM`/`ETM`/`ETS` | `game.cpp:490,493,497` | not traced this pass | **TO VERIFY**. |
| `CAM` | `game.cpp:504` | not traced this pass | **TO VERIFY**. |
| `BLK` | `game.cpp:513` | `block.cpp` (`cSmd`-shaped block data, reuses `SMD`'s own structs per `block.cpp:542`) | Covered by the `SMD` fix above. |
| `EVS` | `game.cpp:515` | not traced this pass | **TO VERIFY**. |
| `ESE` | `se_at.cpp:21` | `snd.h:184` `SeAtHead` | **Not converted** -- raw, **TO VERIFY**. |
| `FSE` | `flr_at.cpp:27` | `flr_at.h:54` `FlrAtHead` | **Not converted** -- raw, **TO VERIFY**. |
| `STB` | `snd.cpp:1654` | `snd.h:31` `SndRoomHdr` | **Not converted** -- raw, **TO VERIFY** (Phase 3 section 2 already flagged `SndBgmTbl`/sound-related structs as Phase 5 territory; `pG->pRoom`'s own `STB` header may still need the swap independent of that). |
| `DSE` | `snd.cpp:1749` | `SndDoorSe` (name referenced, not found as a struct under `include/` this pass) | **TO VERIFY**, loader not fully traced. |
| `RTP`/`MDT`/`OSD`/`EMI` | `read.cpp:204-207` | `embarrel.h:21` `EmiData` (EMI); `RTP`/`MDT`/`OSD` structs not traced this pass | `EmiData`: **not converted**, raw. `RTP`/`MDT`/`OSD`: **TO VERIFY**, not located. |

**Net for this pass**: 2 of the formats above (`CNS`, `SMD`/`BLK`) converted and confirmed to fix a
real, previously-blocking bug; the remaining ~15 are surveyed only (loader/struct location where
found) and left raw, flagged `TO VERIFY` -- converting all of them was not achievable in this
session's budget once the live crash chain (below) needed to be followed past `CNS`/`SMD` to know
whether they were even the right fix.

**Verification of the `CNS`/`SMD` fix**: clean `cmake --build build-pc --target re4_game_all -j8 --
-k 0` -- same 25-file baseline as `docs/port-boot.md` section 48 (`cons.cpp`/`scroll.cpp` both
compile clean; `block.cpp` still fails, confirmed via `git stash` to be its own pre-existing,
unrelated `(int) pointer` cast error, unchanged by this pass). `ctest --test-dir build-pc`: 10/10
(9 previous + the new `test_yz2`).

### Boot progress: R120 room loop reached

Re-ran New Game -> AREA JUMP -> OPENING (`RE4_PORT_INPUT="300:UP,340:A,400:DOWN,440:DOWN,480:START"`,
realtime, `build-pc-boot/re4_boot orig/G4BE08/files orig/G4BE08/re4_debug_disc1.iso`) after the
`CNS`/`SMD` fix. The room archive read/decode completes (`DataReadTime`/`DataEncodeTime` both print),
`st1_0` REL module links (`OSLink: module 'st1_0' (id 74) fresh link`), `-- R120 ----------` prints
(the room's own init banner), and the log then shows per-frame room content (`ESP : ID[xx] TEX/ANM
ptn num diff[...]` lines, `alloc[...]:free[...] roomdata.cpp(503/504)`) followed by a steady stream
of `PrimitiveBuff :  work remain under 1/10` -- a real, pre-existing debug warning
(`debug.cpp:346`), not a crash: it means `pG->nPrim` (`ConsGetRoomValue(CONS_R_NPRIM)`) is a small
budget relative to what the room draws, printed once per frame by `PrimitiveBuffDisp()`. The process
kept running (frame loop, not stuck) until killed after ~14 s; no HALT/abort/segfault. This satisfies
the task brief's stopping condition ("the room loop runs (R120Init once, R120Main per frame)") on
the run where it happened -- `R120Init`'s own banner (`-- R120 ----------`) printed exactly once
across that run.

**Not reliably reproducible -- honestly flagged, not glossed over**: two further repeats of the
*identical* command (one with `RE4_PORT_SCREENSHOT`/`_DELAY_MS=9000`, one without) did **not** reach
`R120` -- both instead looped `cDatTbl::end : memory failed` (a known heap/save subsystem stub
message from earlier sessions, docs/port-boot.md's `IdUnit` sizing note) and stayed on the title
menu. This looks like real-time input-script jitter: `RE4_PORT_INPUT`'s frame-numbered script assumes
a frame arrives roughly on schedule under `realtime` pacing, and host load (this session ran three
`re4_boot` instances back to back) shifts exactly when frame N's input is sampled, so the scripted
`START` press can land one menu frame off and never fire. This is a pre-existing property of the
frame-numbered `RE4_PORT_INPUT` mechanism under realtime pacing, not something this pass's `CNS`/
`SMD` edits caused (both fixes are read-only-format changes with no effect on frame timing).

**Made reliable with `RE4_PORT_FIXED_VI=1`**: the same command with `RE4_PORT_FIXED_VI=1` added
reached `-- R120 ----------` on every one of 2 repeats tried (including one with
`RE4_PORT_SCREENSHOT`/`_DELAY_MS=9000`) -- this is the deterministic-timestep mode docs/port-boot.md
names for exactly this kind of jitter, and it fixes it here too. **The screenshot captured under
`RE4_PORT_FIXED_VI=1`** (9 s in, room loop confirmed still running in the log) shows neither the
title screen nor a 3D room render: it is the debug build's own memory-usage HUD screen (a yellow/
green usage bar gauge, "96/100" and two hex readouts `3F3100`/`403100` bottom-of-screen) with a large
`BSS SIZE OVER!!!` message printed in the middle -- a real, pre-existing debug-tool overlay this
disc's debug build has (not a crash: the process kept running and logging `PrimitiveBuff` lines
after the shot), most likely toggled onto the screen by one of the `RE4_PORT_INPUT` script's own
button presses landing on a debug-menu shortcut rather than only game-menu buttons, or a real (and
possibly genuine, given this port's various stub sizes) BSS-budget overrun the vendor's own debug
build warns about instead of crashing on. **Not root-caused this pass** -- flagged here rather than
asserted either way; a screenshot of the actual R120 3D room render (as opposed to this debug HUD
screen) was still not captured this session.

No REL-module-missing stop was hit this pass (`wep01` or otherwise) -- the run was terminated by the
session budget while still cycling in the room's per-frame loop, not by a further crash.

## 8. Coordinator follow-up: "BSS SIZE OVER!!!" root-caused and fixed, SAT format converted, real
   next blocker found (missing weapon REL module)

### "BSS SIZE OVER!!!" -- root cause was `OSModuleLink`'s raw 8-byte host pointers, not a bssSize swap

Traced with a temporary diagnostic (`OSReport` dump of `pModule`'s raw bytes and
`sizeof(OSModuleInfo)`/`offsetof(bssSize)`, added then removed -- not committed) at the exact call
site (`ReadWepData`, `src/game/read.cpp`, loading `em/wep02.drs`, the handgun's own REL): the field
being checked, `OSModuleHeader::bssSize`, was already correctly `OSModU32` (`re4_port::be_u32`,
i.e. `BE<u32>`) -- Phase 4's own prep work, `include/dolphin/os/OSModule.h`'s header comment. The
bug was one struct member *before* it: `OSModuleInfo::link` (`OSModuleLink { OSModuleInfo* next,
*prev; }`) was still two raw C++ pointers, 8 bytes each on this arm64 host instead of the GameCube's
4 -- confirmed live: `sizeof(OSModuleInfo)` came back 48, not the real 32, and
`offsetof(OSModuleHeader, bssSize)` 48 (0x30) instead of 32 (0x20). Every REL header read directly
off disc into this struct (every `(OSModuleHeader*) (REL_READ_OFFSET32(data+4) + data)` cast in
`read.cpp`) was therefore reading `numSections`/`sectionInfoOffset`/`nameOffset`/`nameSize`/
`version`/`bssSize`/... 16 bytes further into the raw REL bytes than the real field -- for
`em/wep02.drs` this meant `pModule->bssSize` read `0x01010100` (16843008) instead of the REL's real,
tiny `bssSize` of 8, tripping the `> DLL_BSS_MAX` (0x80) debug safety net and hanging in the
vendor's own `for (;;) { eprintf("BSS SIZE OVER!!!"); TaskSleep(1); }` loop -- explaining why it never
appeared in the stdout log (`eprintf` draws directly to the screen's debug-text overlay, not
`OSReport`) even though the process kept "running" (still looping, drawing that overlay every
frame) rather than crashing outright.

**Fix** (`include/dolphin/os/OSModule.h`, `TARGET_PC` only): `OSModuleLink::next`/`prev` are now
`re4_port::Ptr32<OSModuleInfo>`, matching every other on-disc pointer field in this tree and
restoring `OSModuleInfo`'s real 0x20-byte size. This one field is never dereferenced as a real
pointer anywhere in this port's own `OSLink()`/`OSUnlink()`/`IsFresh()` (`src/port/rel.cpp`) -- their
"has this exact header buffer already been linked by this host" marker used to stash the address of
a plain static object there (`kHostLinkedMarker`, 8 bytes, not a valid `Ptr32<T>` arena handle);
switched to storing/comparing a fixed raw 32-bit handle instead (`Ptr32<T>::FromRaw()`/
`raw_handle()`, which never dereference, so never trip `Ptr32<T>`'s out-of-arena-window abort) --
`kHostLinkedMarkerHandle = 0xFFFFFFFE`, a value no real disc-read `next`/`prev` (always 0 per the
file's own prior comment) or genuine relocated handle could plausibly collide with.

**Verified**: `re4_game_all -k 0` still the exact same 25-file baseline (this header change touches
nothing in that list). `ctest`: 10/10. Live run: the "BSS SIZE OVER!!!" hang is gone -- the same
`em/wep02.drs` load now correctly proceeds to `OSLink()`, which correctly reports the real next
blocker (below) instead.

### SAT (scenario collision) format converted -- second real bug on the same room, found immediately after

With the above fixed, the very next blocker hit was `cSat::blockInit() INVALID PTR 0x09dc8c42 (
05060706 )` during R120's own collision-data init -- `include/atari.h`'s `cSatFile` (every plain
`u16` count: `m_nVertex`/`m_nNormal`/`m_nEdge`/`m_nPolygon`/`m_nFloor`/`m_nSlope`/`m_nWall`/
`m_nBlock`) and `cSatHeader::ofs[]` were raw, unswapped -- `cSat::operator=(cSatFile*)` copies those
counts directly into `cSat`'s own `vertex_num`/`polygon_num`/etc., which then drive every SAT table
pointer (`norm_p`/`edge_p`/`poly_p`/`block_p` -- literally `vtx + vertex_num`, etc.), so a garbage
count corrupts every pointer downstream, including the block-chain `blockInit()` walks. `cSatBlock`'s
own remaining raw fields (`m_nFloor`/`m_nSlope`/`m_nWall`/`m_Flag`, and `min`/`m_Size`, plain `Vec`s)
had the same bug (`m_pList`, the block-chain pointer, was already `Ptr32<T>`'d in Phase 2 -- only its
siblings were missed). Converted all of the above to `BE<u16>`/`BE<u32>`/a new `SatVec` (same
`BeVec` pattern as `id_sys.h`/`room_jmp.h`/`scroll.h`'s `SmdVec`) under `TARGET_PC`;
`cSatHeader::getSat()`'s raw `*(u32*) (...)` table read became a `BE<u32>` array index. Two call
sites needed their own local variable's type following the field type change (`u16* idx` ->
`BE<u16>* idx` in two `atari.cpp` collision-check helpers; `SatVec` gained an `operator=(const Vec&)`
so the three dead-stripped-on-the-real-target `createSatN` builders still compile on this host).

**Verified**: `re4_game_all -k 0`: same 25-file baseline, `atari.cpp` compiles clean. `ctest`:
10/10. Live run: `cSat::blockInit()`'s INVALID PTR errors are gone -- R120's SAT init now completes
silently.

### The real next blocker: missing weapon REL module (id 4, `em/wep02.drs` -- Leon's handgun)

Past both fixes, `re4_boot` now reaches `re4_port::OSLink: no built module for id 4` (this port's
own `OSLink()` stand-in, `src/port/rel.cpp`, correctly reporting that nothing is registered for that
id) immediately followed by `DLL_Link()`'s own vendor-written 60-frame-then-`HALT()` fallback
(`main_sub.cpp:1440`, a genuine `*(volatile u32*) 0x11111111 = 0` crash -- this is real vendor code
behaving exactly as designed when a DLL fails to link, not a bug this port introduced). `id 4` is
read from the REL header embedded in `em/wep02.drs` (Leon's handgun weapon archive, loaded by
`ReadWepData(no=2, ...)` right after `st1_0` links and just before `R120Init`'s own banner) --
**not yet built for the host**: `RE4_REL_MODULES` (`CMakeLists.txt`) currently lists only `st1_0`.
Building `wep02` (or whichever module directory in `src/wep*` owns REL id 4 -- not yet identified by
name, only by the runtime id) through the same `tools/port/build_rel_module.py` pipeline `st1_0`
already uses (`config/G4BE08/modules/<mod>/rel.json`, add to `RE4_REL_MODULES`, regenerate the
registry) is real, substantial follow-up work -- a full per-module build (cast-rewriting, `ld -r`
combining, static-ctor semantics if the module has any global constructors) for at least one more
module, not attempted this pass given the remaining budget. This is the actual, confirmed-live
"missing REL module" stop the task brief anticipated.

**Screenshot, honestly described**: captured at the point `re4_boot` is looping the vendor's 60-frame
pre-`HALT()` wait (`RE4_PORT_FIXED_VI=1`, same input script, `RE4_PORT_SCREENSHOT_DELAY_MS=8500`) --
a black screen with only the debug HUD overlay (`PrimitiveBuff` lines, the memory-usage gauge bars,
frame counters "53"/"57" bottom-left, hex readouts "3F2FC0"/"402FC0" bottom-right). **Not the room
render**: consistent with every prior session's finding that no run yet has reached an actual GX
draw call for room geometry -- this blocker (and the real vendor `HALT()` that follows it) sits
before that point in the room's own init sequence, not after. Goal 4 (Leon/the level geometry
visible on screen) was not reached this pass.
