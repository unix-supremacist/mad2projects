# Chaos Mod Effect Ideas & Architectural Review (Updated)

This document provides a detailed review of the chaos effects architecture in `mad2modloader` for **Madagascar 2: The Game** (`Mad2.exe`), incorporating feedback, detailed design plans for **Chaos Graphics Effects**, **Chaos Input Effects**, and a blueprint for a standalone **Input Remapping Mod** and **Companion Challenge Mapping Overhaul**.

---

## 1. Review of Existing Chaos Effects Baseline

### 1.1 Architecture & Technical Constraints

* **`mad2effects.dll`**: Central chaos registry tracking registered effects, weights, and `Apply`/`Clear` callbacks.
* **`zz_mad2graphicseffectmod.dll`**: Handles graphics chaos effects via a D3D9 `EndScene` (vtable slot 42) post-processing pipeline. Captures the backbuffer into offscreen textures and transforms it through active pipeline stages using ping-pong targets.
* **`mad2inputeffectsmod.dll`**: Handles input chaos effects by registering layered overrides with `mad2xinput.dll`.
* **`aa_mad2hookutil.dll`**: Provides shared IAT and vtable hooking primitives.

#### D3D9 Fixed-Function Constraints
1. **No HLSL / Shaders**: No pixel shader compilers or D3DX helper libraries are present in the MinGW cross-compile environment.
2. **Affine Screen Quads**: `D3DFVF_XYZRHW` vertices skip perspective divide, allowing exact affine transformations (rotation, scaling, translation) via 4 corner UV coordinates.
3. **Tessellated Meshes**: Non-linear warps (sine displacement, melt, kaleidoscope) use CPU-tessellated quad grids/wedges.
4. **Channel Isolation**: Multi-pass operations rely on `D3DRS_COLORWRITEENABLE` masks and render target ping-ponging.

---

### 1.2 Feedback & Rejected / Clarified Ideas

Based on codebase analysis and project testing history:

* ❌ **Wireframe (`WireframeVeto`) — REJECTED**: Tested previously, but the game engine resets fill states and object-level render pipelines in ways that make consistent wireframe rendering unreliable and visually broken.
* ❌ **Triggers Disabled / Phantom Triggers — REJECTED**: Madagascar 2's gameplay control schema does not utilize LT/RT triggers (as verified in `mad2inputdisplay`'s default icon set: D-pad, Start, A, B, X, Y, LB). Blocking or pulsing triggers has no gameplay impact.
* ❌ **Single Axis Only — REJECTED**: Redundant with the existing `StickDirectionDisable` effect and causes game-breaking progression issues on specific level mechanics.
* ❌ **Strobe Effects — REJECTED (Safety Hazard)**: Rapid high-frequency color or brightness strobing presents severe photosensitivity / epilepsy hazards for players and viewers.
* ❌ **Iris Pinhole — REJECTED**: Too similar to the existing `Vignette` radial gradient effect.
* ❌ **Radial Tunnel — REJECTED**: Conceptually overlaps with existing `Zoom` / `Pulse` scale transforms.

---

## 2. Refined Chaos Graphics Effect Ideas (Planning)

### 2.1 Detailed Explanations of Select Graphics Ideas

#### 1. `CRTScanlines` ("Retro Arcade / VHS Glitch")
* **Why it's unique**: Adds a classic 1980s retro CRT monitor / arcade cabinet look without distorting the spatial layout of the game.
* **Visual Appearance**: Thin, translucent dark horizontal scanlines overlay the display. Periodically, a faint horizontal "tracking line" slowly rolls from the top of the screen to the bottom, mimicking VHS tape tracking artifacts or analog TV vertical sync roll.
* **Technical Implementation**:
  * Create a tiny 1D procedural texture (e.g. 1x4 pixels) with alternating dark transparent and clear texels, configured with `D3DSAMP_ADDRESSV = D3DTADDRESS_WRAP`.
  * Render a single full-screen quad over the captured backbuffer using `D3DTOP_MODULATE`.
  * Animate the vertical UV $v$-coordinate frame-by-frame ($v(t) = v_0 + \text{speed} \cdot t$) to scroll the scanlines and tracking bar seamlessly down the screen.
* **`config.cfg` Configuration**:
  * `CRTScanlineScale = 4` — scanline texture height in texels (controls density; visual depends on output resolution, so this should be tunable).
  * `CRTScrollSpeed = 1.0` — tracking bar scroll speed multiplier.

#### 2. `DoubleVision` ("Concussion / Optical Diplopia")
* **How it differs from `Echo`**:
  * **`Echo`** = **Temporal Motion Blur**. It accumulates *previous frames over time* into a persistent texture (`g_EchoTex`). If the player stands completely still, the image collapses back into a single crisp frame.
  * **`DoubleVision`** = **Spatial Diplopia / Single-Frame Double Image**. It operates on a *single instantaneous frame*. It renders the current frame twice simultaneously, offset horizontally and vertically at 50% opacity. Even when standing completely still, every object, character, and HUD element is doubled side-by-side (simulating double vision / concussion).
* **Technical Implementation**:
  * In `EndScene`, perform a two-pass quad draw:
    1. Pass 1: Render the captured backbuffer quad at normal screen coordinates $(0,0)$.
    2. Pass 2: Render the same backbuffer quad shifted by $(\Delta x(t), \Delta y(t))$ with `D3DRS_ALPHABLENDENABLE = TRUE`, `SRCBLEND = SRCALPHA`, `DESTBLEND = INVSRCALPHA`, and alpha set to `0.5f`.
  * The offset $(\Delta x, \Delta y)$ should **slowly drift** via sinusoidal oscillation (e.g. $\pm 5\text{px}$ over a 3-second period) rather than remaining static — a fixed double-image is quickly "solved" by the brain and stops feeling disorienting.
* **`config.cfg` Configuration**:
  * `DoubleVisionBaseOffset = 20` — base pixel offset.
  * `DoubleVisionDriftAmplitude = 5` — sinusoidal drift range in pixels.
  * `DoubleVisionDriftPeriod = 3.0` — drift oscillation period in seconds.

#### 3. `DutchRoll` ("Seasick Camera Roll")
* **Visual Appearance**: Smoothly sways the screen back and forth on a Dutch angle tilt ($\pm 25^\circ$ to $\pm 35^\circ$) with a gentle sinusoidal rhythm, creating a seasick boat-rocking sensation.
* **Technical Implementation**:
  * Extends `ComputeAffineCornerUVs`. Computes a time-varying tilt angle $\theta(t) = A \cdot \sin(\omega t)$.
  * Computes 4 corner UV coordinates for a single screen quad in `EndScene` with slight uniform zoom compensation to prevent edge clipping.

#### 4. `TextureTileGrid` ("Security Monitor Matrix")
* **Visual Appearance**: Splits the view into a tiled grid of repeating, mirrored gameplay screens (like a bank of security monitors).
* **Technical Implementation**:
  * Render backbuffer quad with UV coordinates extending beyond $[0,1]$ (e.g. $[0, 2]$ for 2x2 grid, $[0, 3]$ for 3x3, $[0, 4]$ for 4x4).
  * Set sampler state `D3DSAMP_ADDRESSU = D3DTADDRESS_MIRROR` and `D3DSAMP_ADDRESSV = D3DTADDRESS_MIRROR`.
  * **Randomized grid size per trigger**: On each `ApplyTextureTileGrid` call, randomly pick 2x2, 3x3, or 4x4 and update `Mad2Effects_SetDisplayName` (e.g. `"Security Monitor (3x3)"`).
* **`config.cfg` Configuration**:
  * `TextureTileGridAllow2x2 = true`
  * `TextureTileGridAllow3x3 = true`
  * `TextureTileGridAllow4x4 = true`

#### 5. `LetterboxChop` ("Screen Split Slide")
* **Visual Appearance**: Cuts the screen horizontally across the middle, sliding the top half to the left and the bottom half to the right in a sinusoidal back-and-forth rhythm.
* **Technical Implementation**:
  * Render backbuffer using two separate screen quads:
    * Top Quad ($y \in [0, H/2]$): UV $v \in [0, 0.5]$ with horizontal offset $+u_{\text{shift}}(t)$.
    * Bottom Quad ($y \in [H/2, H]$): UV $v \in [0.5, 1.0]$ with horizontal offset $-u_{\text{shift}}(t)$.
  * $u_{\text{shift}}(t)$ oscillates sinusoidally (slides back and forth) rather than continuously scrolling one direction.
* **`config.cfg` Configuration**:
  * `LetterboxChopMaxShift = 0.3` — maximum horizontal UV shift (fraction of screen width).
  * `LetterboxChopSpeed = 1.0` — oscillation speed multiplier.

#### 6. `SmoothColorShift` ("Mood Lighting / Atmosphere Tint")
* **Visual Appearance**: Smoothly cycles the global screen color cast through ambient shades (deep purple, cyan, warm sunset orange, emerald) over a slow 10-second period.
* **Technical Implementation**:
  * Safe, non-seizure alternative to strobing.
  * Modulates texture color factor (`D3DRS_TEXTUREFACTOR`) via `D3DTOP_MODULATE` using a smoothly interpolated RGB color wheel.

#### 7. `EyelidBlink` ("Character Blinking")
* **Visual Appearance**: Simulates character eyelids periodically blinking shut. Two semi-transparent dark horizontal quads quickly slide down from the top and up from the bottom for ~200ms every 3–5 seconds.
* **Draw Order**: Must draw **after** the pipeline (like HUD overlays), **not** as a pipeline `RenderFn` stage — otherwise the black quads get baked into the captured frame and distorted by any other active effect.
* **`config.cfg` Configuration**:
  * `EyelidBlinkOpacity = 0.7` — eyelid quad opacity (0.0 = fully transparent, 1.0 = fully opaque blackout). Default semi-transparent.
  * `EyelidBlinkDurationMs = 200` — blink duration in milliseconds.
  * `EyelidBlinkMinIntervalMs = 3000` — minimum time between blinks.
  * `EyelidBlinkMaxIntervalMs = 5000` — maximum time between blinks.

#### 8. `PictureInPicture` ("Security PIP / Mini Screen")
* **Visual Appearance**: Shrinks the active game screen into a small Picture-in-Picture window in the top-right corner. The background area outside the PIP window shows a heavily pixelated version of the backbuffer with per-frame sample offset jitter, layered with the `Echo` temporal accumulation effect on top — producing a disorienting, smeared, low-fidelity backdrop that contrasts with the crisp PIP window.
* **Technical Implementation**:
  * **Background**: Downsample the backbuffer to a very low-res target (reusing the existing `g_PixelateTex` downsample path) but shift the source sample coordinates by a small random offset each frame before upsampling back via point filter. Then alpha-blend the result into a persistent accumulation texture (same technique as `Echo`'s `g_EchoTex` decay blend). This produces a crawling, smeared pixelation that's visually distinct from either effect alone.
  * **PIP window**: Render the full-resolution captured backbuffer as a scaled quad in the corner (e.g. 25% screen size, top-right), drawn last to overlay the degraded background.
  * **Note**: True spatial blur is not achievable in D3D9 fixed-function without pixel shaders. The pixelation+echo combo is the fixed-function approximation.
* **`config.cfg` Configuration**:
  * `PIPScale = 0.25` — PIP window size as a fraction of screen dimensions.
  * `PIPPosition = TopRight` — corner position (TopRight, TopLeft, BottomRight, BottomLeft).
  * `PIPBackgroundPixelScale = 12` — downsample factor for background pixelation.
  * `PIPBackgroundEchoDecay = 0.85` — echo decay factor for background accumulation.

#### 9. `AspectDistortion` ("Squished Aspect Ratio")
* **Visual Appearance**: Forces an exaggerated squished 32:9 ultra-wide aspect ratio or tall 4:3 stretched ratio onto the single backbuffer quad.
* **Technical Implementation**:
  * Same technique as `Zoom2x` but with independent X/Y scale factors applied to the screen quad's UV coordinates.
  * On each trigger, randomly pick a distortion variant (ultra-wide squish, tall stretch, or a random aspect) and update `Mad2Effects_SetDisplayName` (e.g. `"Squished (Ultra-Wide)"`, `"Squished (Tall Stretch)"`).
* **`config.cfg` Configuration**:
  * `AspectDistortionAllowWide = true`
  * `AspectDistortionAllowTall = true`

#### 10. `Jumpscare` ("FNAF Jumpscare")
* **Visual Appearance**: A full-screen image suddenly flashes over the entire display for a brief, startling moment (0.5–1.5 seconds), then vanishes. Intended to replicate the shock-factor of FNAF-style jumpscares. The image can be anything — a FNAF2 Foxy render, a custom horror image, or any user-supplied asset.
* **Asset Strategy**: The effect loads images from a **user-supplied directory** (`<exe dir>/data/jumpscares/`) rather than bundling any copyrighted assets. This sidesteps licensing issues entirely and makes the effect fully customizable — users drop in whatever `.bmp` files they want. Multiple images are supported; one is randomly selected per trigger.
* **Technical Implementation**:
  * **New infrastructure: BMP texture loading via GDI**. The existing graphics pipeline has no image-file loading (all textures are procedural or backbuffer captures). Load `.bmp` files using `LoadImageA` (GDI, already available in-process via `mad2textrenderer`'s precedent) → extract pixel data via `GetDIBits` → create a `D3DPOOL_MANAGED` `IDirect3DTexture9` and `LockRect`/copy/`UnlockRect`. No new dependencies needed.
  * At effect init, scan `<exe dir>/data/jumpscares/*.bmp` and pre-load all found images into D3D textures.
  * On trigger: pick a random loaded texture, render it as a single full-screen quad with no blending (fully opaque, drawn after the pipeline like `EyelidBlink`).
  * Short duration (configurable), auto-clears.
* **Draw Order**: Must draw **after** the pipeline (same reasoning as `EyelidBlink`) so the jumpscare isn't captured/distorted by other active effects.
* **`config.cfg` Configuration**:
  * `JumpscareDirectory = data/jumpscares` — directory to scan for `.bmp` files.
  * `JumpscareMinDurationMs = 500` — minimum display duration.
  * `JumpscareMaxDurationMs = 1500` — maximum display duration.
  * `JumpscareStretchMode = Fill` — how to fit non-matching aspect ratios (`Fill` = stretch to screen, `Fit` = letterbox/pillarbox, `Center` = native size centered).

---

### 2.2 Refinements to Existing Effects

#### `LowFps` ("Low Framerate") — Randomized FPS Selection
* **Current behavior**: Always caps to ~3 FPS (~333ms frame interval).
* **Proposed change**: On each trigger, randomly select a target FPS from a configurable pool of terrible framerates (e.g. 1, 2, 3, 5, 8, 10, 15). Update `Mad2Effects_SetDisplayName` to show the selected rate (e.g. `"Low Framerate (5 FPS)"`, `"Low Framerate (1 FPS)"`).
* **Technical Implementation**:
  * Replace the hardcoded `~333ms` interval with `1000.0f / selectedFps`. The rest of the existing `LowFps` render logic (stale-frame redraw between updates) is unchanged.
* **`config.cfg` Configuration**:
  * `LowFpsValues = 1,2,3,5,8,10,15` — comma-separated list of FPS values to randomly select from.

---

## 3. Refined Chaos Input Effect Ideas (Planning)

### 3.1 Design Plan: Unified `ButtonSwap` & Permutation System

Instead of maintaining separate static `SwapAB` and `SwapXY` effects alongside a standalone permutation effect, the input system will feature a single, unified, highly dynamic **`ButtonSwap`** effect.

#### How `ButtonSwap` Will Operate:
1. **Eligible Buttons Pool**: Face buttons (**A**, **B**, **X**, **Y**), shoulder button **LB** (verified active in Mad2), and **Start**.
2. **Dynamic Trigger Allocation**: On each `ApplyButtonSwap` call, the effect randomly picks a swap pattern complexity:
   * **Single Pair Swap**: Swaps 1 pair (e.g. $A \leftrightarrow B$, $X \leftrightarrow Y$, $A \leftrightarrow \text{LB}$, $\text{Start} \leftrightarrow A$, etc.).
   * **Dual Pair Swap**: Swaps 2 pairs simultaneously (e.g. $A \leftrightarrow B$ **and** $X \leftrightarrow Y$, or $\text{LB} \leftrightarrow A$ **and** $\text{Start} \leftrightarrow X$).
   * **Cyclic Rotation (3 to 6 Buttons)**: Rotates button mappings in a cycle (e.g. $A \rightarrow B \rightarrow X \rightarrow A$, or a full shuffle including LB and Start).
3. **Dynamic Display Name**: Automatically updates `mad2effects` via `Mad2Effects_SetDisplayName("ButtonSwap", displayName)` (matching `JoystickReversal` and `StickDirectionDisable` conventions) to clearly inform the player/streamer:
   * Example HUD readouts:
     * `"Swap Buttons (A <-> B)"`
     * `"Swap Buttons (A <-> LB, B <-> Y)"`
     * `"Swap Buttons (Start <-> A)"`
     * `"Swap Buttons (A -> B -> X -> LB -> Start)"`
     * `"Swap Buttons (Full Shuffle)"`
4. **`config.cfg` Configuration**:
   * `ButtonSwapWeight = 10`
   * `ButtonSwapMinDurationMs = 10000`
   * `ButtonSwapMaxDurationMs = 20000`
   * `ButtonSwapAllowSinglePair = true`
   * `ButtonSwapAllowMultiPair = true`
   * `ButtonSwapAllowFullCycle = true`
   * `ButtonSwapIncludeLB = true`
   * `ButtonSwapIncludeStart = true`

---

### 3.2 Design Plan: Separate `SwapDpadABXY` Effect ("D-Pad & Face Button Swap")

A standalone input effect distinct from `ButtonSwap` that swaps the **D-Pad** directional controls with the **A / B / X / Y** face buttons.

* **Gameplay Impact**: Pressing D-Pad directions executes face button actions (Jump, Spin, Attack, Action), while pressing face buttons outputs D-Pad directional inputs!
* **Technical Implementation**:
  * In `mad2xinput` layer override, remap `XINPUT_GAMEPAD_DPAD_UP / DOWN / LEFT / RIGHT` bitmask fields with `XINPUT_GAMEPAD_Y / A / X / B` bitmask fields.
* **Dynamic Display Name**:
  * `"Swap Controls (D-Pad <-> ABXY)"`
* **`config.cfg` Configuration**:
  * `SwapDpadABXYWeight = 10`
  * `SwapDpadABXYMinDurationMs = 10000`
  * `SwapDpadABXYMaxDurationMs = 20000`

---

### 3.3 Additional Input Proposals

#### 1. `StickDrift` ("Phantom Controller Drift")
* **Gameplay Impact**: Simulates severe hardware controller drift by constantly adding a biased vector offset to movement or camera sticks.
* **Technical Implementation**:
  * `mad2xinput` layer override adds a constant bias $(dx, dy)$ to `sThumbLX`/`sThumbLY` or `sThumbRX`/`sThumbRY` before deadzone normalization.
  * Requires new `biasLX`/`biasLY`/`biasRX`/`biasRY` (`SHORT`) fields in `Mad2XInputOverride` — additive offsets applied before the existing scale/force steps.
  * Bias direction and magnitude randomized per trigger (e.g. anywhere from 20%–60% magnitude, any angle).
* **`config.cfg` Configuration**:
  * `StickDriftMinBias = 20` — minimum bias as percentage of stick range.
  * `StickDriftMaxBias = 60` — maximum bias as percentage of stick range.

#### 2. `SwapSticks` ("Southpaw Swap")
* **Gameplay Impact**: Swaps the Left Analog Stick (character movement) and Right Analog Stick (camera control).
* **Technical Implementation**:
  * New `swapSticks` (`BOOL`) field in `Mad2XInputOverride`. When set, `HookXInputGetState` exchanges `(sThumbLX, sThumbLY)` with `(sThumbRX, sThumbRY)` in `XINPUT_GAMEPAD`.

#### 3. `AxisLock` ("Digital 8-Way Snap")
* **Gameplay Impact**: Removes smooth analog stick precision, snapping all stick angles to 8 strict cardinal directions at 100% magnitude (emulating retro digital arcade joysticks).
* **Technical Implementation**:
  * Quantize stick vector angle $\theta = \text{atan2}(y, x)$ to the nearest $45^\circ$ step ($0^\circ, 45^\circ, 90^\circ, \dots$) and clamp vector length to maximum `32767`.
  * Should read post-override state (`Mad2XInput_GetOverriddenState`) then force-write quantized values via `forceLX`/`forceLY`/`forceRX`/`forceRY` — this ensures quantization applies to the values the player actually experiences, composing correctly with other active effects.

#### 4. `InputLag` ("Cloud Stream Latency")
* **Gameplay Impact**: Delays player inputs by 500ms to 1000ms. Button presses and stick movements occur 1 second after physical execution.
* **Technical Implementation**:
  * The effect mod maintains a timestamped ring buffer of `XINPUT_STATE` per slot (not inside `mad2xinput` itself). Each poll, it reads `Mad2XInput_GetOverriddenState` (capturing the post-chain values including any other active effects), pushes the snapshot into the buffer, and force-overrides the entire state with the buffered entry from $T - T_{\text{lag}}$ ms ago.
  * Other override layers applying on top of the already-delayed input is acceptable and arguably *more* chaotic.
* **`config.cfg` Configuration**:
  * `InputLagMinMs = 500` — minimum delay.
  * `InputLagMaxMs = 1000` — maximum delay (randomized per trigger within this range).

#### 5. `ButtonInvert` ("Sticky Buttons / Hold to Release")
* **Gameplay Impact**: Inverts button states for face buttons and LB: buttons are treated as pressed while physically released, and released while physically held down. **Start is excluded** from inversion — including it would cause constant pause/unpause cycling, which is unplayable rather than chaotic.
* **Technical Implementation**:
  * `mad2xinput` layer override flips the bits of `wButtons` for active face buttons (**A, B, X, Y, LB only**, not Start).
  * Implementation: `blockButtons = A|B|X|Y|LB` (suppress real presses), then `forceHoldButtons` set to the bitwise complement of whatever was physically pressed for those 5 buttons.
* **`config.cfg` Configuration**:
  * `ButtonInvertIncludeA = true`
  * `ButtonInvertIncludeB = true`
  * `ButtonInvertIncludeX = true`
  * `ButtonInvertIncludeY = true`
  * `ButtonInvertIncludeLB = true`

#### 6. `StickJitter` ("Hand Tremor Stick")
* **Gameplay Impact**: Adds high-frequency noise/jitter to thumbstick positions, making straight movement or fine aiming shaky.
* **Technical Implementation**:
  * Uses the same `biasLX`/`biasLY`/`biasRX`/`biasRY` fields as `StickDrift`, but updated every poll with fresh pseudo-random noise $(\pm \Delta x, \pm \Delta y)$ rather than a constant bias.
* **`config.cfg` Configuration**:
  * `StickJitterAmplitude = 4000` — maximum noise magnitude (out of 32767 full range).
  * `StickJitterAffectLeft = true`
  * `StickJitterAffectRight = true`

---

## 4. Summary Matrix & Priority Recommendations

| Effect Name | Category | Primary Technique | Complexity | Priority |
| :--- | :--- | :--- | :--- | :--- |
| **`CRTScanlines`** | Graphics | Modulated 1D Scanline Texture + Scrolling UV | Low | High |
| **`DoubleVision`** | Graphics | Single-Frame Dual-Quad Alpha Blending + Sinusoidal Drift | Low | High |
| **`DutchRoll`** | Graphics | Affine Corner UV Oscillations | Low | High |
| **`ButtonSwap` (Unified)**| Input | Dynamic Permutation Table + `mad2xinput` API Extension + `SetDisplayName` | Medium | High |
| **`SwapDpadABXY`** | Input | D-Pad $\leftrightarrow$ ABXY Bitmask Swap (uses `ButtonSwap` permutation infra) | Low | High |
| **`StickDrift`** | Input | `mad2xinput` Bias Vector Layer (new `bias*` fields) | Low | High |
| **`SwapSticks`** | Input | `mad2xinput` Stick Swapping Layer (new `swapSticks` field) | Low | High |
| **`TextureTileGrid`**| Graphics | Texture Sampler Mirror Addressing + Randomized Grid Size | Low | High |
| **`EyelidBlink`** | Graphics | Screen-Space Semi-Transparent Quad Motion (post-pipeline) | Low | Medium |
| **`LetterboxChop`** | Graphics | Dual-Quad Split Geometry + Sinusoidal Oscillation | Low | Medium |
| **`AspectDistortion`**| Graphics | Non-Uniform UV Scaling (Zoom variant) | Low | Medium |
| **`PictureInPicture`**| Graphics | Corner-Scaled Quad + Pixelation+Echo Background | Medium | Medium |
| **`SmoothColorShift`**| Graphics | Texture Factor RGB Cycle Modulation | Low | Medium |
| **`AxisLock`** | Input | Vector Quantization via Force Overrides | Low | Medium |
| **`InputLag`** | Input | Timestamped Ring Buffer Queue (effect-mod-side) | Medium | Medium |
| **`ButtonInvert`** | Input | `wButtons` Bitwise Inversion (excl. Start) | Low | Medium |
| **`StickJitter`** | Input | Pseudo-Random Bias Vector Noise Layer | Low | Medium |
| **`Jumpscare`** | Graphics | GDI BMP Texture Load + Full-Screen Quad Flash (post-pipeline) | Medium | Medium |
| **`LowFps` (Randomized)**| Graphics | Existing effect refinement — randomized FPS selection + `SetDisplayName` | Low | High |

---

## 5. Non-Chaos Feature Planning

### 5.1 Input Remapping Mod (`mad2inputremap`)

A standalone QoL mod (non-chaos effect) that allows fully custom remapping of physical controller inputs — including analog sticks, axis-to-button, and button-to-axis mappings.

#### Architectural Requirements & Chaos Compatibility:
1. **Layered Override Priority**:
   * Registers a persistent override layer with `mad2xinput.dll` (`Mad2XInput_AddOverride`) at startup.
   * Because `mad2xinput` evaluates override handles sequentially in order of creation, `mad2inputremap`'s layer runs **first** (converting physical controller inputs into user-remapped Mad2 actions).
   * Any active chaos effects (`ButtonSwap`, `JoystickReversal`) registered later add their own override layers **on top**, preserving 100% compatibility with all chaos effects!
2. **Config Integration (`config.cfg`)**:
   * Scoped under a `[InputRemap]` section:
     ```ini
     [InputRemap]
     Enabled = true
     # Digital button remapping (any button -> any button)
     RemapA = A
     RemapB = B
     RemapX = X
     RemapY = Y
     RemapLB = LB
     RemapStart = Start
     RemapDpadUp = DpadUp
     RemapDpadDown = DpadDown
     RemapDpadLeft = DpadLeft
     RemapDpadRight = DpadRight
     # Analog stick remapping
     RemapLeftStick = LeftStick
     RemapRightStick = RightStick
     # Axis-to-button mapping (each axis direction can map to a digital button)
     RemapLeftStickUp = None
     RemapLeftStickDown = None
     RemapLeftStickLeft = None
     RemapLeftStickRight = None
     RemapRightStickUp = None
     RemapRightStickDown = None
     RemapRightStickLeft = None
     RemapRightStickRight = None
     # Button-to-axis mapping (digital buttons can map to axis directions)
     # Threshold for axis-to-button activation
     AxisToButtonThreshold = 50
     ```
3. **Scope**:
   * Maps any physical controller input to any other input Madagascar 2 consumes. This includes:
     * **Digital buttons** ↔ **digital buttons** (A, B, X, Y, D-Pad, Start, LB)
     * **Analog sticks** ↔ **analog sticks** (left ↔ right swap)
     * **Axis directions** → **digital buttons** (e.g. left stick up → A button, with configurable activation threshold)
     * **Digital buttons** → **axis directions** (e.g. D-Pad → analog stick emulation)
     * **Axis directions** → **trigger-like values** (e.g. stick push → trigger activation, even though Mad2 doesn't use triggers natively — useful for companion challenge mods that do consume trigger state)

---

### 5.2 Companion Challenge Input Mapping Overhaul (`mad2companionmod`)

Overhauls button translation logic in `sm64mod.cpp` (Super Mario 64 Challenge) and `jak1mod.cpp` / `mariolegacymod.cpp` (Jak & Daxter and Mario Legacy Challenges) to restrict inputs strictly to those used in Madagascar 2, correcting legacy keybind flaws.

#### 1. Super Mario 64 Mapping Blueprint (`sm64mod.cpp`)
* **Current Issue**: `sm64mod.cpp` mapped `X` to N64 Z-trigger, which created a confusing control scheme. `Y` is currently unmapped.
* **New Blueprint**:
  * **Z-Trigger (Crouch / Ground-Pound / Slide)** $\rightarrow$ **LB** (maps Crouch/Roll directly to Madagascar 2's Crouch/Roll button **LB**).
  * **R-Button (Camera View Toggle)** $\rightarrow$ **Y** (maps N64 camera mode toggle to Madagascar 2's **Y** button).
  * **A (Jump)** $\rightarrow$ **A**
  * **B (Punch / Attack / Action)** $\rightarrow$ **both X and B** (either physical button triggers N64 B — lets the player use whichever feels natural).
  * **Start** $\rightarrow$ **Start**
  * **D-Pad** $\rightarrow$ **D-Pad**
  * **Left Stick** $\rightarrow$ **N64 Analog Stick** (1:1 Character movement)
  * **Right Stick** $\rightarrow$ **N64 C-Buttons** (1:1 Camera pitch/yaw via thresholding)
  * Restricts mapping strictly to the active Mad2 button set (**A, B, X, Y, D-Pad, Start, LB, Left/Right Sticks**).

#### 2. Jak 1 & Mario Legacy Mapping Blueprint (`jak1mod.cpp` & `mariolegacymod.cpp`)
* **Current Issue**: Current `XInputToPs2Pad` includes `LT` $\rightarrow$ `L2`, `RT` $\rightarrow$ `R2` (trigger mappings), `RB` $\rightarrow$ `R1`, `LeftThumb` $\rightarrow$ `L3`, `RightThumb` $\rightarrow$ `R3`, and `Back` $\rightarrow$ `Select` — all of which are inputs Madagascar 2 itself never uses. The face-button and LB mappings already largely match the target blueprint.
* **Actual delta** (what needs changing vs current code):
  * **Strip**: `LT`→`L2`, `RT`→`R2`, `RB`→`R1`, `LeftThumb`→`L3`, `RightThumb`→`R3`, `Back`→`Select` — remove all six.
  * **Keep as-is**: `LB`→`L1`, `A`→`Cross`, `B`→`Circle`, `X`→`Square`, `Y`→`Triangle`, `Start`→`Start`, `D-Pad`→`D-Pad`, `Left/Right Sticks`→`Left/Right Analog Sticks`.
* **Target Blueprint** (for reference):
  * In Jak 1, L1 and R1 perform identical crouching/rolling actions.
  * **L1 (Crouch / Roll / Ground-Pound)** $\rightarrow$ **LB** (aligning Jak's roll/crouch button directly with Madagascar 2's **LB**).
  * **Triangle (First-Person View / Action / Vehicle)** $\rightarrow$ **Y** (maps PS2 Triangle directly to Madagascar 2's **Y** button).
  * **X (Jump)** $\rightarrow$ **A**
  * **Square (Punch)** $\rightarrow$ **X**
  * **Circle (Spin Attack)** $\rightarrow$ **B**
  * **Start** $\rightarrow$ **Start**
  * **D-Pad** $\rightarrow$ **D-Pad**
  * **Left & Right Sticks** $\rightarrow$ **Left & Right Analog Sticks** (1:1 Movement & Camera control)
  * Strips out unused trigger mappings to maintain a clean 1:1 match with Madagascar 2's button profile.

---

## 6. Known Bugfixes

### 6.1 `mad2companionmod`: Unbuilt companion binary causes permanent input lock

* **Bug**: If `SM64Challenge`, `JakChallenge`, or `MarioLegacyChallenge` triggers while the corresponding companion binary hasn't been built (e.g. `extern/sm64ex-alo` was never compiled, or `extern/jak-project/build/game/gk` doesn't exist), the effect's `Apply` callback blocks Mad2's controller input via `mad2xinput` (adding an override layer that suppresses all input to redirect it to the companion process), but the child process either fails to launch or exits immediately. The watcher thread that's supposed to detect the child exiting and self-clear the effect via `Mad2Effects_ClearByName` either never starts (if launch failed before spawning it) or fires but doesn't properly restore input. The result is **permanently disabled controller input** for the rest of the session — the player is stuck.
* **Root cause**: The `Apply` callbacks block input *before* verifying the binary exists and *before* confirming the launch succeeded. There's no rollback path if the launch fails.
* **Fix**: In each effect's `Apply` callback (`ApplySM64Challenge`, `ApplyJakChallenge`, `ApplyMarioLegacyChallenge`):
  1. **Check binary existence first** — before touching `mad2xinput` at all, verify the target executable exists on disk (e.g. `GetFileAttributesA` on the expected path). If it doesn't exist, log a warning and **return without applying** (or immediately self-clear), so `mad2chaosmod` just picks a different effect next cycle.
  2. **Rollback on launch failure** — if the binary exists but the launch fails (e.g. `CreateProcess` returns `FALSE`, or `mad2relauncher` replies with an error/times out), immediately remove the `mad2xinput` override layer and self-clear the effect.
  3. **Timeout safety net** — if the watcher thread doesn't see the child process appear within a reasonable timeout (e.g. 5 seconds after sending `LAUNCH`), assume launch failed and self-clear. This catches edge cases like `mad2relauncher` not running.
* **`config.cfg` Configuration**:
  * `CompanionLaunchTimeoutMs = 5000` — timeout before assuming launch failure and rolling back.
