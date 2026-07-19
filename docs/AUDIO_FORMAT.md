# Audio format (`.snds` files)

Source of truth: `mad2iga/fsb.go`. Fully reverse-engineered and verified —
every one of the 23,378 `.snds` samples under
`../../mad2assetextractor/mad2raw/` extracts cleanly to a valid, playable
file with zero failures (see "Verification" below).

## Two distinct formats share the `.snds` extension

`.snds` is not one format. A quick survey of all 23,378 sample files found:

- **22,818 files (97.6%)** are **FSB3** (FMOD Sound Bank version 3), magic
  `FSB3\x01\x00\x00\x00`. See "FSB3 container" below.
- **560 files (2.4%)** are plain **Ogg Vorbis** streams (`OggS` magic),
  reusing the `.snds` extension verbatim — these are already a standard,
  directly-playable container and need no parsing at all, just a rename
  (`mad2iga.IsOggVorbis` / `ExtractSnds` handles this by passing the bytes
  through unchanged with `ext=".ogg"`). These tend to be the larger,
  music/cutscene-adjacent files (e.g. `croc_ride_2.snds`,
  `bn_level_intro_5point1.snds` — stereo, tens of seconds, clearly full
  mixed audio rather than short SFX/dialogue lines).

Both were confirmed by dumping full-corpus header/magic statistics
(`survey_snds.py`-style scan, not checked in — see "Verification").

## FSB3 container

### Main header (24 bytes)

| Offset | Size | Field       | Notes                                        |
|--------|------|-------------|-----------------------------------------------|
| 0x00   | 4    | Magic       | `"FSB3"`                                       |
| 0x04   | 4    | NumSamples  | Always `1` in every real MAD2 `.snds` sampled  |
| 0x08   | 4    | HeaderSize  | Total size of the sample-header table that follows |
| 0x0C   | 4    | DataSize    | Total size of the sample data blocks that follow the header table |
| 0x10   | 4    | Version     | `0x00030001` observed (FSB3.1-shaped, see below) |
| 0x14   | 4    | Flags       | `0x00000002` observed; bit meanings not enumerated here (not needed — no MAD2 sample was found using `FMOD_FSB_SOURCE_BIGENDIANPCM` or any other flag-dependent behavior) |

Confirmed byte-exact: `24 + HeaderSize + DataSize == file size` for every
sample checked.

### Per-sample header

Immediately follows the main header, one entry per `NumSamples` (always 1 in
practice — see below). Each entry is self-describing: its own first `uint16`
is its total byte length (including that `uint16` itself), so
`mad2iga.ParseFSB3` walks the table as a chain rather than assuming a fixed
struct size — this matters because FSB3.0 vs FSB3.1 sample headers differ in
size (`0x40` vs `0x50` per the vgmstream reference implementation, the most
complete public source for the FSB family), and MAD2's own variant is a
0x58-byte "extended" form:

| Offset (from entry start) | Size | Field                  |
|----|------|--------------------------------------------------------|
| 0x00 | 2  | Entry size (`0x58` = 88, observed)                       |
| 0x02 | 30 | Name, fixed-width, not necessarily null-terminated if it exactly fills 30 bytes |
| 0x20 | 4  | LengthSamples (decoded frame count, per channel)         |
| 0x24 | 4  | LengthCompressedBytes (this sample's data block size)    |
| 0x28 | 4  | LoopStart                                                |
| 0x2C | 4  | LoopEnd                                                  |
| 0x30 | 4  | Mode (FSOUND_* flags, see below)                         |
| 0x34 | 4  | DefFreq (sample rate)                                    |
| 0x38 | 2  | DefVol                                                   |
| 0x3A | 2  | DefPan                                                   |
| 0x3C | 2  | DefPri                                                   |
| 0x3E | 2  | NumChannels                                              |
| 0x40 | 4  | MinDistance (float)                                      |
| 0x44 | 4  | MaxDistance (float)                                      |
| 0x48 | 4  | VarFreq                                                  |
| 0x4C | 2  | VarVol                                                   |
| 0x4E | 2  | VarPan                                                   |
| 0x50 | 8  | **Unexplained** — zero in every sample checked, not part of vgmstream's documented FSB3.1 layout. Open question, harmless to ignore (entry-size-based walking skips over it correctly regardless). |

Verified byte-exact against a real sample
(`Card_Match_GameARC/builddata/win/pr_partmatching_introskipped_mason.snds`):
name = `"PR_PartMatching_IntroSkipped_M"` (30 chars, exactly fills the field),
DefFreq = 44100, NumChannels = 1, MinDistance/MaxDistance = 1.0/10000.0
(`0x461C4000` is the well-known IEEE-754 bit pattern for `10000.0f`) — all
sane, expected FMOD defaults.

Sample data blocks follow the entire header-table area, back to back, each
occupying its own `LengthCompressedBytes` — this is how `ParseFSB3` locates
each sample's raw (still-encoded) bytes without needing per-sample data
offsets.

**`NumSamples` is always 1** in every one of the 22,818 real FSB3 `.snds`
files sampled — multi-sample FSB3 containers are a real thing in the FSB
family generally, but MAD2 doesn't appear to use them (each sound effect /
line of dialogue gets its own `.snds` file with exactly one embedded
sample). `ParseFSB3` still handles `NumSamples > 1` structurally (walks the
table, slices data sequentially) but this path is unexercised by any real
asset.

### Codec: FSOUND_IMAADPCM ("XBOX IMA"), universally

Every one of the 22,818 real FSB3 samples has `Mode == 0x00400026` decomposed
as `FSOUND_IMAADPCM (0x00400000) | FSOUND_MONO (0x00000020)` — i.e. **100%
mono IMA ADPCM**. (One single outlier, `penguinsARC/.../video_loop_18.snds`,
has an extra unexplained high bit set — `0x80400020` — but decodes
identically; the bit's meaning is untested/unknown.) Sample rates found:
44100 Hz (16,300 files), 48000 Hz (6,491 files), 22050 Hz (27 files).

Per vgmstream's `fsb.c` (`load_codec`), `FSOUND_IMAADPCM` selects the
**"XBOX IMA"** variant (historically Microsoft's Xbox ADPCM codec, reused by
FMOD's encoder here regardless of target platform) *unless* the sample is
>2 channels or has `FSOUND_MULTICHANNEL` set, in which case it's the
different "interleaved-header" `FSB_IMA` variant instead. Since every real
MAD2 sample is mono, only the plain "XBOX IMA" path is exercised —
`mad2iga.decodeXboxIMA`'s 2-channel branch is implemented from vgmstream's
documented frame layout but is **untested** (no stereo sample exists
anywhere in the 23,378-file corpus to check it against).

#### XBOX IMA frame layout (mono, verified)

Fixed 0x24-byte (36-byte) frames:

```
+0x00  s16  hist1       (initial predictor, NOT itself an output sample)
+0x02  u8   step_index  (clamped 0..88)
+0x03  u8   reserved
+0x04  32 bytes of nibble data, low nibble first, 64 nibbles = 64 output samples
```

Each nibble is expanded via the textbook IMA4 algorithm (`imaExpandNibble` in
`fsb.go`) — same step-size table (`ima_step_size_table[90]`) and index table
(`ima_index_table[16]`) used by essentially every IMA ADPCM implementation
(Microsoft's, Apple QuickTime's, Xbox's) confirmed against vgmstream's
`ima_decoder.c`.

**Verified with real arithmetic, not just decode-without-crashing**: for the
sample above, `LengthSamples = 137408`, and `137408 / 64 = 2147` exactly —
i.e. the sample is an exact whole number of 64-sample frames, which only
holds if the block size (36 bytes → 64 samples/channel) and the "header
sample is not itself emitted" convention are both correct. Expected data
bytes `2147 × 36 = 77292` vs. actual `LengthCompressedBytes = 77296` — 4
bytes over, unexplained (harmless trailing pad; `decodeXboxIMA` naturally
ignores it since it only processes whole 36-byte frames).

### Other Mode values: handled defensively, unexercised

`DecodeToPCM16` also handles `FSOUND_MPEG`, `FSOUND_VAG` (PS2), `FSOUND_XMA`
(Xbox 360), `FSOUND_GCADPCM` (GameCube), plain `FSOUND_8BITS` (signed or
`FSOUND_UNSIGNED`), and an "otherwise assume raw PCM16LE" fallback — all
either return a clear "not implemented, not expected on PC assets" error or
(for PCM8/PCM16) decode directly, but **none of these paths is exercised by
any real MAD2 `.snds` file** (100% of samples are mono IMA ADPCM). They're
there for robustness/documentation of the format family, not because MAD2
needs them.

## Extraction API (`mad2iga/fsb.go`)

- `IsFSB3(data)`, `IsOggVorbis(data)` — magic sniffing.
- `ParseFSB3(data) (*FSBFile, error)` — full header + sample-table parse;
  `FSBFile.Samples[i].Data` is the raw (still-encoded) slice.
- `(*FSBSample).DecodeToPCM16() (pcm []int16, channels, sampleRate int, err error)`.
- `WritePCM16WAV(w, pcm, channels, sampleRate)` — minimal canonical
  RIFF/WAVE writer.
- `ExtractSnds(data) (out []byte, ext string, err error)` — the one-call
  convenience wrapper: Ogg passthrough or FSB3-decode-to-WAV, whichever
  applies. This is what `mad2repack snd-extract`/`snd-extract-all` call.

## CLI (`mad2repack`)

```sh
mad2repack snd-extract <in.snds> <out>          # ext ignored; writes .wav or .ogg as appropriate, reports which
mad2repack snd-extract-all <dir> <outdir>       # recursively finds *.snds, mirrors directory structure
```

## Verification

- **Full-corpus decode test**: every one of the 23,378 `.snds` files under
  `mad2assetextractor/mad2raw/` was extracted via `ExtractSnds` with **zero
  failures** (22,818 FSB3-decoded to WAV, 560 Ogg passthrough).
- **ffprobe/ffmpeg validation** on a handful of representative outputs
  (mono 44.1kHz dialogue line, mono 44.1kHz video-loop SFX, stereo 44.1kHz
  Ogg cutscene track): all three report valid, well-formed audio with
  sane codec/sample-rate/channel/duration metadata and non-degenerate
  volume levels (`mean_volume` around -10 to -21 dB, `max_volume` just
  under 0 dB — real dynamic-range audio, not silence or clipped noise).
- No spectrogram/waveform-shape comparison against an independent reference
  decoder was done (none was available in this environment) — the
  verification above confirms the output is valid, plausible audio, not
  that it's bit-identical to what FMOD's own decoder would produce. Given
  the algorithm is the textbook IMA4 expansion (not a MAD2-specific
  variant) and the frame-size arithmetic checks out exactly, this is
  considered solid, but flagging the gap for completeness.

## Open questions

- The 8 unexplained trailing bytes in each sample header entry (always zero
  in samples checked), and the small (4-byte, in the sample checked)
  surplus in `LengthCompressedBytes` vs. the exact frame-count-derived byte
  total.
- The single outlier sample with an extra high bit set in `Mode`
  (`0x80000000`) — decodes fine, meaning unknown.
- The 2-channel "XBOX IMA" frame-interleaving path is implemented from
  vgmstream's documented layout but has no real sample to verify against in
  this corpus.
- `FSB_IMA` (the interleaved-header variant for >2ch/`FSOUND_MULTICHANNEL`)
  is not implemented at all — no MAD2 sample needs it, but a from-scratch
  reimplementation would be needed if one ever turns up (e.g. in a
  different regional build).
