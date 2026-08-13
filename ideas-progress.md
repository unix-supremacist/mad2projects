# ideas.md Implementation Progress

This document tracks which ideas from `ideas.md` have been implemented, in the order they were completed.

---

## Status Legend

- Done — Implemented and committed to source
- In Progress — Currently being worked on
- Pending — Not yet started

---

## Completed

*(none yet)*

---

## In Progress

*(working on the first batch)*

---

## Pending (by priority)

### High Priority

| # | Idea | Category | File(s) |
|---|------|----------|---------|
| 1 | `LowFps` Randomized FPS | Graphics (refinement) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 2 | `CRTScanlines` | Graphics (new) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 3 | `DoubleVision` | Graphics (new) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 4 | `DutchRoll` | Graphics (new) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 5 | `ButtonSwap` (Unified) | Input (new) | `mad2inputeffectsmod/src/inputeffects.cpp` |
| 6 | `SwapDpadABXY` | Input (new) | `mad2inputeffectsmod/src/inputeffects.cpp` |
| 7 | `StickDrift` | Input (new) | `mad2xinput` + `mad2inputeffectsmod` |
| 8 | `SwapSticks` | Input (new) | `mad2xinput` + `mad2inputeffectsmod` |
| 9 | `TextureTileGrid` | Graphics (new) | `mad2graphicseffectmod/src/graphicseffect.cpp` |

### Medium Priority

| # | Idea | Category | File(s) |
|---|------|----------|---------|
| 10 | `EyelidBlink` | Graphics (new, post-pipeline) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 11 | `LetterboxChop` | Graphics (new) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 12 | `AspectDistortion` | Graphics (new) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 13 | `PictureInPicture` | Graphics (new) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 14 | `SmoothColorShift` | Graphics (new) | `mad2graphicseffectmod/src/graphicseffect.cpp` |
| 15 | `AxisLock` | Input (new) | `mad2inputeffectsmod/src/inputeffects.cpp` |
| 16 | `InputLag` | Input (new) | `mad2inputeffectsmod/src/inputeffects.cpp` |
| 17 | `ButtonInvert` | Input (new) | `mad2inputeffectsmod/src/inputeffects.cpp` |
| 18 | `StickJitter` | Input (new) | `mad2inputeffectsmod/src/inputeffects.cpp` |
| 19 | `Jumpscare` | Graphics (new, post-pipeline) | `mad2graphicseffectmod/src/graphicseffect.cpp` |

### Non-Chaos / Bugfixes

| # | Idea | Category | File(s) |
|---|------|----------|---------|
| 20 | `mad2inputremap` standalone mod | New mod | `mad2inputremap/` |
| 21 | SM64 mapping overhaul | Companion mod fix | `mad2companionmod/src/sm64mod.cpp` |
| 22 | Jak1 / Mario Legacy mapping fix | Companion mod fix | `mad2companionmod/src/jak1mod.cpp`, `mad2companionmod/src/mariolegacymod.cpp` |
| 23 | Companion binary launch bugfix | Companion mod fix | all three companion src files |

---

## Implementation Notes

### XInput API Extensions Needed

New input effects (StickDrift, SwapSticks, StickJitter) require new fields in
`Mad2XInputOverride` and `ApplyOverrideLayer`. These are batched together.

New fields planned:
- `BOOL swapSticks` -- swaps (sThumbLX, sThumbLY) with (sThumbRX, sThumbRY)
- `SHORT biasLX, biasLY, biasRX, biasRY` -- additive bias (StickDrift/StickJitter)

### Post-Pipeline Draw Order

`EyelidBlink` and `Jumpscare` draw **after** the main pipeline, not as pipeline
stages. This ensures they overlay cleanly on top of other active effects rather
than being captured and transformed by the pipeline.
