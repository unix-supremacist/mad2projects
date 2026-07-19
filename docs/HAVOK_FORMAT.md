# Havok data (`.hkt` files)

Lowest priority of the three formats investigated this round, per explicit
time-boxing — this is findings-only, no parser. No code was written for
this format.

## What's confirmed

- **All 451 `.hkt` files are named `*_animation.hkt`**, with no exceptions
  (checked by pattern across the full sample set). Filenames read as
  per-actor animation clips for specific cutscenes, e.g.
  `ma_level_intro_mort_animation.hkt`,
  `ma_level_intro_buzzard1_animation.hkt`,
  `wh_race_intro_camera_animation.hkt` (note: cameras get their own
  `.hkt` "animation" too, alongside each character). There is no
  `*_physics.hkt`, `*_ragdoll.hkt`, `*_collision.hkt`, or similarly-named
  file anywhere in the sampled corpus. This strongly suggests `.hkt` is
  used exclusively for **cutscene skeletal/camera animation clips**, not
  for physics/rigid-body/collision data, despite the engine clearly having
  a full Havok Physics integration too (see below) — that data must live in
  a different container (not investigated).

- **This is genuine, unmodified Havok SDK code**, not a custom fork or
  reimplementation: grepping
  `../../mad2assetextractor/wii_demo_ref/Mad2.elf.symbols.txt` for `hka`/`hkp`
  turns up exact, recognizable public Havok class and file names —
  `hkpRigidBody.cpp`, `hkpRigidBodyCinfo.cpp`, `hkBinaryPackfileReader.cpp`
  (with its nested `PackfileObjectsCollector`/`PackfilePointersMapListener`
  helper classes), `hkaDeltaCompressedSkeletalAnimationCtor.cpp`,
  `hkaDeltaCompressedSkeletalAnimation.cpp`, `hkVersionUtil.cpp`, etc. —
  matching Havok Physics/Animation's own well-known public source layout
  (the `.cpp` file names are preserved as ELF `FILE` symbols even in this
  stripped-of-debug-info-but-not-stripped-of-symbols demo build).

- **The animation subsystem specifically uses `hkaDeltaCompressedSkeletalAnimation`**
  (confirmed present with real, non-trivial decompression logic —
  `decompressBlockToCache`, `retrieveFromCache`, `releaseToCache` all
  exist as real functions, not stubs) — this is Havok's quantized
  delta-compression scheme for skeletal animation tracks, one of several
  Havok animation formats (others include spline-compressed and
  fully-uncompressed/interleaved variants, not confirmed present or absent
  here).

- **No recognizable Havok packfile/tagfile header** is present at the start
  of a `.hkt` file. A hex dump of a real sample
  (`Morts_AdventureARC/builddata/win/ma_level_intro_mort_animation.hkt`,
  1,148,928 bytes) starts immediately with what look like raw IEEE-754
  floats — no `TAG0`/`SDKV`/`DATA` or similar Havok tagfile section markers,
  no `hkpackfile` string, nothing resembling the standard Havok binary
  packfile header/table-of-contents that public tools (`hkxcmd`,
  `HKXConvert`, various community `hktypes.py`/Havok Behavior Toolkit
  scripts) are built to read. This matches the task brief's own
  observation ("no distinct header — first bytes are Havok's own class
  data directly") and is the main reason public Havok tooling doesn't
  directly apply here without adaptation.

  A repeating 4-byte pattern (`00 00 80 3F` — the IEEE-754 bit pattern for
  `1.0f`) recurs roughly every 16-32 bytes throughout the sample, consistent
  with quaternion `w=1` or uniform-scale `1.0` components inside a
  transform-track structure — circumstantial support for "this is
  positions/rotations/scales", not proof of the exact struct layout.

## What's not established (genuinely open, not attempted further)

- The exact reason the standard Havok packfile header/TOC is absent. Two
  plausible explanations, neither confirmed: (a) the engine's tool-chain
  deliberately strips it before embedding into the IGA/IGZ asset pipeline,
  since the exact Havok SDK version and class layout compiled into the game
  binary is already known at load time (a real, documented Havok usage
  pattern for shipping titles that don't need forward/backward version
  compatibility) — the surrounding IGZ container's own reflection metadata
  might supply what the stripped header would have, similar to how `.texs`
  uses the IGZ class directory for `igImage2`, or (b) these were captured
  from a different serialization path entirely (e.g. a memory-image dump
  rather than `hkSerializeUtil`'s normal file-write path).
- Whether the data is actually `hkaDeltaCompressedSkeletalAnimation` (the
  one confirmed-present compressed-animation class) as opposed to a
  simpler uncompressed variant, or a `hkaAnimationContainer` wrapping
  multiple tracks/bindings.
- Any byte-level struct layout for whichever Havok class(es) are actually
  instantiated here. This would need the same technique that worked for
  `igImage2` in `TEXTURE_FORMAT.md` (decompiling constructors/field-
  registration call sites in the Wii ELF or the PC `igCore.dll`/whichever
  DLL wraps Havok — likely a `TfbHavokLibrary.dll`-adjacent module, per
  `mad2igttimer`'s own note in the top-level `CLAUDE.md` about reading a
  status byte from `TfbHavokLibrary.dll`) — not attempted, given the
  explicit time-box for this format relative to audio/texture.
- Whether physics/rigid-body data (confirmed present in the engine's
  compiled code, per the `hkp*` symbols above) is stored in `.hkt` files
  elsewhere in the full game (only cutscene-animation-named files were
  found in the sampled corpus) or in a completely different
  file/extension not yet identified.

## Suggested next steps, if picked up again

1. Confirm which Havok class is actually being deserialized by finding and
   decompiling whatever function reads a `.hkt` blob at runtime (likely in
   a Havok-wrapper DLL such as `TfbHavokLibrary.dll` in the PC build, or the
   Wii ELF's equivalent link-time-merged code) — this would settle the
   "which class" and "how is it framed without a packfile header" questions
   in one pass, the same way decompiling `igImage2`'s field-registration
   call site did for textures.
2. Once the class is known, apply `CLASS_SCHEMA.md`'s Wii-ELF field-offset
   extraction technique to it, same as this session did for `igImage2`.
3. Cross-reference against Havok's public (non-open-source, but
   widely-documented-by-the-modding-community) class layouts for whichever
   Havok version this is (the compiled symbol names suggest a mid-2000s
   Havok Animation/Physics SDK, consistent with this being a 2008-era
   multi-platform (PS2/PSP/GameCube/Wii/Xbox 360/PC) title) — community
   tools like `hkxcmd`/`HKXConvert`/Havok Behavior Toolkit docs are built
   against the *standard* packfile format and won't read a headerless blob
   directly, but their struct definitions for `hkaAnimation` and friends
   would still be directly useful once the framing question is answered.
