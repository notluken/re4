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
