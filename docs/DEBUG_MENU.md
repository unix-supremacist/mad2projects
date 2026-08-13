# Madagascar 2 native "Debug Options" menu — reverse-engineering notes

Goal: reproduce ImJustATester's PS2 "Debug Options" trick (see `../DEBUGRESOURCE.txt`)
on the PC build. Their PS2 method (from the pause → Options menu):

- write `0x00DDC2C8 = 0x00CC9674` (repoint the **Sound** options item's action to the
  debug-menu opener)
- write `0x00DD85B8 = 0`
- then press X on "Sound" → opens the Debug Options menu

Those addresses are PS2-specific and don't map to PC (no PS2 binary available; PS2/PC/Wii
builds differ, per the resource). This doc records what the debug menu actually **is** on
PC and how it's reached, derived entirely from the shipped PC asset data.

## Key finding: the debug menu is real, shipped data on the PC retail build

It is **not** native C++ menu code — Mad2's menus are script-driven (TFBScriptInfo VM).
The Wii demo `Mad2.elf` (symboled) has **no** native menu/FrontEnd/Options C++ classes;
menus are built from the global script graph. The debug menu is a normal script UI screen
whose text and structure ship in `mad2/Content/Streams/win/global.bld` (loaded in every
level, which is why the debug trick works from the in-game pause menu).

Extraction path (all via `build/mad2repack`, built from `mad2repack/`):

```
mad2repack unpack global.bld            -> global_unpacked/{ENGLISH.pak, level.bld, ...}
mad2repack pak-dump   ENGLISH.pak       -> the 492 display strings (menu labels)
mad2repack objects-dump level.bld       -> object graph (492 StringInfo, 214 ScriptSet, ...)
mad2repack script-dump  level.bld       -> full opcode graph (28 ScriptInfo, 214 ScriptSet roots)
```

### The debug menu content is in `ENGLISH.pak` (retail display text, not dev-only):

```
"Debug Menu"  "DEBUG"  "DEBUG CHEATS"  "SAVE EDITOR"  "PLACEHOLDER"
"Collect Coins" "Collect Monkeys" "Remove Actors" "Restore Coins" "Restore Monkeys"
"Restore Actors" "Toggle Completes" "Toggle Variables"
```

Freecam overlay text (same pak): `"RB/LB - Speed"  "Camera Speed: @x"  "[ Drop Player"
"} Exit"  "{ Reset"`.

## Architecture (from the named scripts in `level.bld`)

`script-dump` exposes `referenced_ai_paths` — the real script names:

- `ControllerGestureEvaluator.ai` — button-combo/gesture detection
- `FlyaroundCamController.ai` — the freecam (SI[13] in the dump)
- `MenuManager.ai`, `MainUIController.ai`, `PauseController.ai`
- `ControllerMonitor_PC.ai`, `ControllerManager.ai`, `Controller_InputAverager.ai`

Because freecam (LT+RT+A) and the main-menu checkpoint-skip combo already work on PC, these
global scripts are **live and functional** on PC — so the debug-menu trigger almost
certainly also works on PC and just hasn't been found.

## Cracking the name pool (solves, for this file, the documented "naming" wall)

Script/variable/object names are ordinals into `level.bld`'s Section-0 string pool. The
pool is built exactly like `mad2iga/objgraph.go`'s `StringPool()`:
`realStart = sec0Off + u32@(sec0Off+0x2C) + 0xA840`, entries prefixed with
`["ttr","OpMoveFrom","igHandleList"]`, and an ordinal `N` resolves to `pool[N+1]`
(`ResolveStringOrdinal`). Verified: `OpCreateVariable._varName` ordinal for the freecam
matches `"cam flyaround mode"`. (`ScriptSet._name` still resolves to unrelated manifest
filenames — the documented unreliable case — but `StringInfo._name`, `OpCreateVariable
._varName` resolve to real identifiers here.)

## The debug menu, mapped

**Options menu entries** (`Options_Opt_*`, siblings in the options list):
`Options_Opt_Sound`, `Options_Opt_Video`, `Options_Opt_Controller`, **`Options_Opt_DEBUG`**.

So on PC the "DEBUG" item is a **real sibling** of Sound/Video/Controller in the pause →
Options menu — normally hidden/skipped, added by the **`additional menu options actor -
options menu`** variable when debug is enabled. (This is exactly why the PS2 user had to
repoint *Sound*: on PS2 the hidden DEBUG sibling wasn't being added, so they hijacked an
existing visible item instead.)

**Debug main menu** (`Menu_DebugMain`) items:
- `Debug_Opt_ChangeLevel`
- `Debug_Opt_SaveEditor` → `DEBUG_SaveGame_Editor` submenu:
  `DEBUG_SGEd_CollectCoins/CollectMonkeys/CollectObjs/RestoreCoins/RestoreMonkeys/RestoreObjs/ToggleCompletes/ToggleVariables`
- `Debug_Opt_ToggleDebugCheatsVar` (toggles `DEBUG_CHEATS_ENABLED`)

**The gate (refined).** Confirmed live by the user: enabling the native debug cheat does
**not** make the Options→DEBUG entry appear. That's consistent with PS2 — cheat #14
"DEBUG OPTIONS ENABLED" turns on debug *features* (freecam etc.), but the debug *menu*
always needed the separate repoint hack. (Cross-checked the cheat struct: `mad2cheatapply`'s
`debugMode` offset `+0xD4` maps exactly to PS2 `0x20CCD494` = cheat #14, and `superMangos
-0x60`→`0x20CCD360`, `headOfTheGame -0x34`→`0x20CCD38C`, `penguinProjectile +0x24`→
`0x20CCD3E4` all line up — so the cheat IS being written correctly; it just isn't the menu
gate.)

So the menu gate is a **separate script-side condition** in the `additional menu options
actor - options menu` builder (a real `OpCreateVariable` in the 4.8 MB master script SI[1]).
Evidence it's conditional: the `Options_Opt_DEBUG` StringInfo (`0x148964`) has **9**
references vs 5/6/3 for Sound/Video/Controller — extra guard+add+action handling the plain
options don't have. `DEBUG_CHEATS_ENABLED` is toggled *from inside* the debug menu
(`Debug_Opt_ToggleDebugCheatsVar`), so it can't be the gate to reach the menu (circular);
`Cheat_Supercheat`/`Cheat_Test` are the other cheat-key `StringInfo`s in the same block
(`0x14e0c8`/`0x14e0fc`/`0x14e164`).

**The gate site, located** (via `script-pretty`, after teaching it to resolve container
operands to member names — see below). In the options-menu builder's execution stream the
DEBUG entry is added under a guard the plain options do **not** have:

```
IF   <reg 0x1D7410> == 1        // 0x1D7410 := (regA != 0) OR (regB != 0)
IF   <reg 0x21C864> == 0        // a persistent flag register
OpCheckReference(... RHS={"Options_Opt_DEBUG"})   // is it already present?
FIND-VAR "pause start button debounce timer"
OpSetReference(... RHS={"Options_Opt_DEBUG"})      // <-- ADD the DEBUG option
...
OpSetReference(... RHS={"Options_Opt_Video"})       // Video/Sound: added unconditionally
OpSetReference(... RHS={"Options_Opt_Sound"})
```

So DEBUG is gated on a **computed condition** (`0x1D7410==1 && 0x21C864==0`), where `0x1D7410`
is itself `regA≠0 || regB≠0` set a few ops earlier. Sound/Video/Controller have no such guard.

**Why the ultimate variable names aren't pinned:** these operands are the VM's **shared
global scratch registers** (`0x102E30`/`0x102E50`/… are each read by *thousands* of ops
across the whole script — proven by reverse-ref count), so a register→variable binding is
**temporal** (whatever the last `FIND-VAR`/`SET` wrote), not addressable. Naming the gate's
inputs would require faithfully simulating the VM's branch+register dataflow — a real
interpreter, and unverifiable without the running game. `0x21C864` in particular is a
persistent flag compared to 0/1 in many places (lines 501/2060/2123/… in the global
`script-pretty` dump) — the prime suspect for the "debug enabled" flag.

**Recommended close-out: runtime.** Since static analysis bottoms out at the shared-register
dataflow, the reliable last mile is a `mad2minigamemodefix`-style probe: at runtime the
options menu is a live item container; find it (or the `additional menu options actor -
options menu` object, now name-resolvable at runtime via `*(obj+8)`) and either force the
gate register/flag or inject the `Options_Opt_DEBUG` item directly. The static evidence above
says exactly what to look for.

**Tooling note (done this session):** `script-pretty` now resolves container operands
(`ScriptGroupStack`/`SetStack`/`ScriptObjectList`/`igObjectList`) to the **named members**
they hold (`containerMemberNames` in `mad2repack/script_pretty.go`) — which is what made the
`OpSetReference(... RHS={"Options_Opt_DEBUG"})` add-site legible in the first place. It does
**not** simulate register dataflow (see above).

## Freecam / debug combos (secondary request)

Freecam (`FlyaroundCamController`) — known toggle **LT+RT+A**. Its vars/controls:
`cam flyaround mode`, `cam flyaround pitch`, `cam flyaround R2 debounce` (RT/R2 toggles),
`cam flyaround remembered player`, `cam mult` / `DEBUG_ChangeCamMult` / `DEBUG_CurCamMult`
(RB/LB speed), `DEBUG_DropPlayer`, `DEBUG_Exit`, `DEBUG_ResetToActivator`,
`DEBUG_FlyCamRememberedActivator`. It also reads PC-keyboard bindings (`Button_PCKey_9`,
`Button_PCMouse_Right`) — freecam may have keyboard controls too.

Combos are decided in `ControllerGestureEvaluator` (`gesture evaluators`, `gesture pressed
time`, `gesture anim`, `mini gesture *`). Decoding the exact gesture table there is the
open follow-up for finding the debug-menu-open combo directly.

## Status / next steps

- CONFIRMED: debug menu exists as PC data; reached via a hidden `Options_Opt_DEBUG` entry
  in the pause→Options menu, gated on a debug flag; freecam is script-driven and live.
- IMMEDIATE TEST (user, live): enable the existing debug cheat (mad2cheatapply /
  `debugMode`), open pause → Options, look for a "DEBUG" entry. If the native cheat drives
  the gate, it should already appear.
- If not automatic: a mod (mad2minigamemodefix-style) can force the gate variable at
  runtime — locate the relevant ValueInfo/`additional menu options actor` object by its
  runtime name (`*(obj+8)`, the runtime-naming technique) and set it.
- OPEN: exact gate variable+value, and the `ControllerGestureEvaluator` combo table (would
  give a direct button combo to open the menu, matching how freecam is triggered).

## Applied patch (minor, reversible): force the DEBUG option unconditional

Rather than reverse the gate's variable, we neutralized the **DEBUG-only** guard so the
option is always added to the pause→Options menu, exactly like Sound — leaving the rest of
the menu logic untouched.

Located via `MAD2_PRETTY_ADDR=1 mad2repack script-pretty` (env-gated file-address prefix):
in the options-menu builder the entries are added at
```
pc=163 IF <reg 0x1D7410>==1   \  whole-options-block guards (also gate Sound/Video) -- NOT touched
pc=164 IF <reg 0x21C864>==0   /
pc=165 @0x21DCEC OpCheckReference(... RHS={"Options_Opt_DEBUG"})  branchPC=170  <-- DEBUG-only gate
pc=166 FIND-VAR ; pc=167 OpSetReference add Options_Opt_DEBUG      (body of pc=165)
pc=170.. add Options_Opt_Video ; pc=173 add Options_Opt_Sound      (unconditional)
```
Normal play hides DEBUG because pc=165 branches to pc=170 (skipping the add). Setting that
branch target to the fall-through (pc=166) makes the add run whether or not the branch is
taken — always added. Sound/Video are outside pc=165's body, so they're unaffected; pc=163/164
are left alone (Sound/Video already pass them).

**The edit:** `level.bld` byte at object `0x21DCEC` + field offset `0x24` (`OpBranch._branchPC`,
a uint32 LE) changed `170 (0xAA) -> 166 (0xA6)`. Same length, single field — the "safe"
class per this doc's Bug-4 finding.

**Deploy:** `mad2repack replace global.bld -o global_patched.bld -file level.bld=<patched>`
(recompresses; round-trip verified byte-identical except that one byte). Deployed to
`mad2/Content/Streams/win/global.bld`; original saved as `global.bld.orig-backup` beside it.
Revert = restore the backup. Not yet applied to `mad2russia`/`mad2demo` (separate archives).

**Status: UNTESTED in-game** — needs a live check: pause → Options → is there a selectable
DEBUG entry, and does it open `Menu_DebugMain`? If the OpCheckReference turns out to also
guard against duplicate insertion (menu rebuilt incrementally rather than fresh), watch for a
doubled DEBUG row; if so, the gate is instead one of the pc=163/164 registers and this needs
the runtime approach.
