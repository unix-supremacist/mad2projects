// FSB3 (FMOD Sound Bank version 3) parsing and extraction, for MAD2's
// `.snds` files.
//
// This is a NEW file, independent of iga.go/repack.go/pak.go/level.go — see
// docs/AUDIO_FORMAT.md for the full reverse-engineering writeup (header
// layout, per-sample header layout, which codec MAD2 actually uses, and open
// questions). Short version: nearly every `.snds` in the game is a
// single-sample FSB3 container holding mono "XBOX IMA" ADPCM (FMOD's
// `FSOUND_IMAADPCM` mode) at 22050/44100/48000 Hz; a substantial minority
// (~560 of 23,378 sampled) are not FSB3 at all — they're plain Ogg Vorbis
// streams (`OggS` magic) reusing the `.snds` extension, and are already
// directly playable without any parsing.
package iga

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
)

// FSOUND_* mode flags, as used in FSB3's per-sample header "Mode" field.
// Names/values match FMOD's own public FSOUND_MODE enum (confirmed against
// vgmstream's src/meta/fsb.c, the most complete public reference for the FSB
// container family). Only the bits actually seen in MAD2's assets, or needed
// to interpret them, are listed — this is not the full FMOD enum.
const (
	FSoundLoopOff      = 0x00000001
	FSoundLoopNormal   = 0x00000002
	FSound8Bits        = 0x00000008
	FSound16Bits       = 0x00000010
	FSoundMono         = 0x00000020
	FSoundStereo       = 0x00000040
	FSoundUnsigned     = 0x00000080
	FSoundSigned       = 0x00000100
	FSoundMPEG         = 0x00000200
	FSoundImaAdpcm     = 0x00400000 // FSOUND_IMAADPCM
	FSoundVag          = 0x00800000 // PS2 Sony VAG
	FSoundXMA          = 0x01000000 // Xbox 360 XMA
	FSoundGCAdpcm      = 0x02000000 // GameCube DSP-ADPCM
	FSoundMultichannel = 0x04000000
	FSoundCelt         = 0x08000000
)

const fsbMainHeaderSize = 24 // magic(4) + numSamples(4) + headerSize(4) + dataSize(4) + version(4) + flags(4)

// FSBSample is one embedded sample from an FSB3 container's sample-header
// table, plus its raw (still-encoded) data slice.
type FSBSample struct {
	Name            string
	LengthSamples   uint32 // decoded sample-frame count (per channel)
	CompressedBytes uint32 // byte length of this sample's data block
	LoopStart       uint32
	LoopEnd         uint32
	Mode            uint32
	Freq            int32
	DefVol          int16
	DefPan          int16
	DefPri          int16
	NumChannels     int16
	Data            []byte // raw encoded bytes, sliced from the parent file
}

// FSBFile is a parsed FSB3 container.
type FSBFile struct {
	NumSamples int32
	Version    uint32
	Flags      uint32
	Samples    []FSBSample
}

// IsFSB3 reports whether data starts with the FSB3 magic ("FSB3").
func IsFSB3(data []byte) bool {
	return len(data) >= 4 && string(data[0:4]) == "FSB3"
}

// IsOggVorbis reports whether data is a raw Ogg container ("OggS" magic) —
// some `.snds` files are this verbatim, not FSB3 at all. See
// docs/AUDIO_FORMAT.md.
func IsOggVorbis(data []byte) bool {
	return len(data) >= 4 && string(data[0:4]) == "OggS"
}

// ParseFSB3 parses an FSB3 container's header and per-sample header table.
// Sample data slices point into the input `data` slice (no copy).
func ParseFSB3(data []byte) (*FSBFile, error) {
	if !IsFSB3(data) {
		return nil, fmt.Errorf("not an FSB3 file (bad magic)")
	}
	if len(data) < fsbMainHeaderSize {
		return nil, fmt.Errorf("file too small (%d bytes) for FSB3 main header", len(data))
	}

	numSamples := int32(binary.LittleEndian.Uint32(data[4:8]))
	headerSize := binary.LittleEndian.Uint32(data[8:12])
	// dataSize (data[12:16]) is the total data-block size; not needed for
	// parsing since each sample's own CompressedBytes is authoritative, but
	// documented here since it round-trips (headerSize+dataSize+24 == file
	// size for every sample checked, see docs/AUDIO_FORMAT.md).
	version := binary.LittleEndian.Uint32(data[16:20])
	flags := binary.LittleEndian.Uint32(data[20:24])

	if numSamples < 0 || numSamples > 1<<16 {
		return nil, fmt.Errorf("implausible sample count %d", numSamples)
	}

	headerAreaStart := fsbMainHeaderSize
	headerAreaEnd := headerAreaStart + int(headerSize)
	if headerAreaEnd > len(data) || headerAreaEnd < headerAreaStart {
		return nil, fmt.Errorf("sample header area (%d bytes at +0x18) exceeds file size %d", headerSize, len(data))
	}

	f := &FSBFile{NumSamples: numSamples, Version: version, Flags: flags}

	// Walk the per-sample header table. Each entry's own first uint16 is its
	// total byte length (including that uint16 itself), so entries can be
	// walked as a chain without knowing the exact struct shape up front —
	// useful since FSB3.0 vs FSB3.1 sample headers differ in size (0x40 vs
	// 0x50 per vgmstream) and MAD2's own extended variant is 0x58 here (see
	// docs/AUDIO_FORMAT.md for the byte-exact breakdown, including 8
	// trailing unexplained-but-zero-filled bytes per entry that this walk
	// does not need to understand since it trusts the entry's own size
	// field, not a fixed struct size).
	pos := headerAreaStart
	for i := int32(0); i < numSamples; i++ {
		if pos+2 > headerAreaEnd {
			return nil, fmt.Errorf("sample %d: header entry starts past header area", i)
		}
		entrySize := int(binary.LittleEndian.Uint16(data[pos : pos+2]))
		if entrySize < 2 {
			return nil, fmt.Errorf("sample %d: implausible header entry size %d", i, entrySize)
		}
		end := pos + entrySize
		if end > len(data) {
			end = len(data)
		}
		s, err := parseFSBSampleHeader(data[pos:end])
		if err != nil {
			return nil, fmt.Errorf("sample %d: %w", i, err)
		}
		f.Samples = append(f.Samples, s)
		pos += entrySize
	}

	// Data blocks follow the entire header area, back to back, in the same
	// order as the sample-header table, each occupying its own
	// CompressedBytes length.
	dPos := headerAreaEnd
	for i := range f.Samples {
		n := int(f.Samples[i].CompressedBytes)
		if dPos+n > len(data) {
			n = len(data) - dPos
			if n < 0 {
				n = 0
			}
		}
		f.Samples[i].Data = data[dPos : dPos+n]
		dPos += n
	}

	return f, nil
}

// parseFSBSampleHeader decodes one sample-header table entry. `entry` is the
// exact byte range for this entry (starting at its own size-prefix field).
func parseFSBSampleHeader(entry []byte) (FSBSample, error) {
	// Layout (matches vgmstream's fsb.c for FSB3.1-style extended headers,
	// confirmed byte-for-byte against real MAD2 .snds samples — see
	// docs/AUDIO_FORMAT.md):
	//   +0x00 u16   entry size (already consumed by caller)
	//   +0x02 [30]  name, fixed-width, NOT necessarily null-terminated if it
	//               exactly fills the 30 bytes
	//   +0x20 u32   LengthSamples
	//   +0x24 u32   LengthCompressedBytes
	//   +0x28 u32   LoopStart
	//   +0x2C u32   LoopEnd
	//   +0x30 u32   Mode
	//   +0x34 s32   DefFreq
	//   +0x38 s16   DefVol
	//   +0x3A s16   DefPan
	//   +0x3C s16   DefPri
	//   +0x3E s16   NumChannels
	//   +0x40 f32   MinDistance  (present in the extended/0x58 variant only)
	//   +0x44 f32   MaxDistance
	//   +0x48 s32   VarFreq
	//   +0x4C s16   VarVol
	//   +0x4E s16   VarPan
	//   (+0x50..+0x57: 8 bytes, unexplained, zero in every sample checked)
	const minLen = 2 + 30 + 4 + 4 // size + name + LengthSamples + LengthCompressedBytes, bare minimum to be useful
	if len(entry) < minLen {
		return FSBSample{}, fmt.Errorf("entry too short (%d bytes)", len(entry))
	}

	name := string(bytes.TrimRight(entry[2:32], "\x00"))
	fbase := 32

	get32 := func(off int) uint32 {
		if off+4 > len(entry) {
			return 0
		}
		return binary.LittleEndian.Uint32(entry[off:])
	}
	get16 := func(off int) int16 {
		if off+2 > len(entry) {
			return 0
		}
		return int16(binary.LittleEndian.Uint16(entry[off:]))
	}

	s := FSBSample{
		Name:            name,
		LengthSamples:   get32(fbase + 0x00),
		CompressedBytes: get32(fbase + 0x04),
		LoopStart:       get32(fbase + 0x08),
		LoopEnd:         get32(fbase + 0x0C),
		Mode:            get32(fbase + 0x10),
		Freq:            int32(get32(fbase + 0x14)),
		DefVol:          get16(fbase + 0x18),
		DefPan:          get16(fbase + 0x1A),
		DefPri:          get16(fbase + 0x1C),
		NumChannels:     get16(fbase + 0x1E),
	}
	if s.NumChannels == 0 {
		s.NumChannels = 1
	}
	return s, nil
}

// DecodeToPCM16 decodes a sample's raw data to interleaved 16-bit PCM,
// dispatching on its Mode flags. See docs/AUDIO_FORMAT.md for which modes are
// actually exercised by MAD2's assets (in practice: mono FSOUND_IMAADPCM,
// almost universally) versus which are handled defensively/untested.
func (s *FSBSample) DecodeToPCM16() (pcm []int16, channels int, sampleRate int, err error) {
	channels = int(s.NumChannels)
	if channels < 1 {
		channels = 1
	}
	sampleRate = int(s.Freq)

	switch {
	case s.Mode&FSoundMPEG != 0:
		return nil, 0, 0, fmt.Errorf("sample %q: MPEG-mode FSB sample, not PCM-decodable here — extract s.Data directly as a raw MP3 stream instead", s.Name)

	case s.Mode&FSoundImaAdpcm != 0:
		if channels > 2 || s.Mode&FSoundMultichannel != 0 {
			return nil, 0, 0, fmt.Errorf("sample %q: interleaved-header FSB_IMA variant (>2ch or FSOUND_MULTICHANNEL) not implemented — not seen in MAD2's assets", s.Name)
		}
		pcm, err = decodeXboxIMA(s.Data, channels)
		return pcm, channels, sampleRate, err

	case s.Mode&FSoundVag != 0:
		return nil, 0, 0, fmt.Errorf("sample %q: PS2 VAG-mode FSB sample not implemented (not expected on PC assets)", s.Name)
	case s.Mode&FSoundXMA != 0:
		return nil, 0, 0, fmt.Errorf("sample %q: Xbox360 XMA-mode FSB sample not implemented (not expected on PC assets)", s.Name)
	case s.Mode&FSoundGCAdpcm != 0:
		return nil, 0, 0, fmt.Errorf("sample %q: GameCube DSP-ADPCM FSB sample not implemented (not expected on PC assets)", s.Name)

	case s.Mode&FSound8Bits != 0:
		pcm = decodePCM8(s.Data, s.Mode&FSoundUnsigned != 0)
		return pcm, channels, sampleRate, nil

	default:
		// No compression flag set: raw PCM16. Assumed little-endian (the
		// FMOD_FSB_SOURCE_BIGENDIANPCM flag lives in the main-header Flags
		// field, not checked here since no MAD2 sample has been found using
		// it — see docs/AUDIO_FORMAT.md).
		pcm = decodePCM16LE(s.Data)
		return pcm, channels, sampleRate, nil
	}
}

// --- IMA ADPCM ("XBOX IMA" FSB variant) decoding ---
//
// Standard IMA4 nibble-expansion tables and algorithm (identical across
// every IMA ADPCM flavor — Microsoft's, Xbox's, Apple's QuickTime IMA4,
// etc.); what differs between flavors is purely the block/header framing,
// handled by decodeXboxIMA below. See docs/AUDIO_FORMAT.md for how this was
// cross-checked against real MAD2 .snds sample-count/byte-count math.

var imaIndexTable = [16]int32{-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8}

var imaStepTable = [90]int32{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
	34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
	157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
	3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767, 0,
}

func imaExpandNibble(code int32, hist1 *int32, stepIndex *int32) int16 {
	step := imaStepTable[*stepIndex]
	delta := step >> 3
	if code&1 != 0 {
		delta += step >> 2
	}
	if code&2 != 0 {
		delta += step >> 1
	}
	if code&4 != 0 {
		delta += step
	}
	if code&8 != 0 {
		delta = -delta
	}
	sample := *hist1 + delta
	if sample > 32767 {
		sample = 32767
	} else if sample < -32768 {
		sample = -32768
	}
	*hist1 = sample
	*stepIndex += imaIndexTable[code]
	if *stepIndex < 0 {
		*stepIndex = 0
	} else if *stepIndex > 88 {
		*stepIndex = 88
	}
	return int16(sample)
}

// imaBlockSize is the per-channel XBOX-IMA frame size in bytes (4-byte
// header + 32 bytes of nibble data = 64 samples/channel per frame).
// Confirmed exactly against real MAD2 samples: LengthSamples is always an
// exact multiple of 64 for every mono sample checked.
const imaBlockSize = 0x24

// decodeXboxIMA decodes FSOUND_IMAADPCM-mode FSB data (the "XBOX IMA"
// variant per vgmstream's naming — historically Microsoft's Xbox ADPCM
// codec, reused verbatim by FMOD's PC-targeted FSB3 encoder here; distinct
// from the "interleaved-header" FSB_IMA variant used for >2-channel or
// FSOUND_MULTICHANNEL samples, not implemented, see DecodeToPCM16).
//
// Mono path is verified against real MAD2 assets (see docs/AUDIO_FORMAT.md).
// The 2-channel path is implemented from vgmstream's documented frame
// layout but is UNTESTED — no stereo `.snds` sample exists anywhere in the
// sampled corpus (23,378 files, 100% mono) to check it against.
func decodeXboxIMA(data []byte, channels int) ([]int16, error) {
	if channels < 1 {
		channels = 1
	}
	frameSize := imaBlockSize * channels
	blockSamples := (imaBlockSize - 4) * 2 // 64, per channel
	headerBytes := 4 * channels

	out := make([]int16, 0, (len(data)/frameSize+1)*blockSamples*channels)

	for off := 0; off+frameSize <= len(data); off += frameSize {
		frame := data[off : off+frameSize]
		hist := make([]int32, channels)
		step := make([]int32, channels)
		for c := 0; c < channels; c++ {
			hist[c] = int32(int16(binary.LittleEndian.Uint16(frame[c*4:])))
			step[c] = int32(frame[c*4+2])
			if step[c] > 88 {
				step[c] = 88
			}
		}

		chanOut := make([][]int16, channels)
		for c := range chanOut {
			chanOut[c] = make([]int16, blockSamples)
		}

		if channels == 1 {
			payload := frame[headerBytes:]
			for i := 0; i < blockSamples; i++ {
				b := payload[i/2]
				var code int32
				if i%2 == 0 {
					code = int32(b & 0xF)
				} else {
					code = int32(b >> 4)
				}
				chanOut[0][i] = imaExpandNibble(code, &hist[0], &step[0])
			}
		} else {
			// Interleaved-per-channel: nibbles grouped 8-at-a-time (4 bytes)
			// per channel, groups alternating channel-major.
			for i := 0; i < blockSamples; i++ {
				groupIdx := i / 8
				withinGroup := i % 8
				byteIdx := withinGroup / 2
				highNibble := withinGroup%2 == 1
				for c := 0; c < channels; c++ {
					bpos := headerBytes + groupIdx*4*channels + c*4 + byteIdx
					b := frame[bpos]
					var code int32
					if highNibble {
						code = int32(b >> 4)
					} else {
						code = int32(b & 0xF)
					}
					chanOut[c][i] = imaExpandNibble(code, &hist[c], &step[c])
				}
			}
		}

		for i := 0; i < blockSamples; i++ {
			for c := 0; c < channels; c++ {
				out = append(out, chanOut[c][i])
			}
		}
	}
	return out, nil
}

func decodePCM8(data []byte, unsigned bool) []int16 {
	out := make([]int16, len(data))
	for i, b := range data {
		if unsigned {
			out[i] = (int16(b) - 128) << 8
		} else {
			out[i] = int16(int8(b)) << 8
		}
	}
	return out
}

func decodePCM16LE(data []byte) []int16 {
	n := len(data) / 2
	out := make([]int16, n)
	for i := 0; i < n; i++ {
		out[i] = int16(binary.LittleEndian.Uint16(data[i*2:]))
	}
	return out
}

// --- IMA ADPCM ("XBOX IMA" FSB variant) ENCODING ---
//
// The mathematical inverse of decodeXboxIMA/imaExpandNibble above. Standard
// IMA4 encode: for each sample, quantize the delta from the running
// predictor into a 4-bit code via the textbook bit-by-bit magnitude
// comparison against the current step size (the reference algorithm found
// in essentially every public IMA ADPCM encoder — Microsoft's, the
// "cr.c"/"dvi_adpcm" reference implementation, etc.), then advance the
// predictor/step-index state by feeding that exact code back through
// imaExpandNibble — the *same* function decodeXboxIMA uses to consume it.
// Delegating the state update to imaExpandNibble (rather than duplicating
// its clamping/index-table logic here) is deliberate: it's what guarantees
// the encoder's state transition after each sample is bit-for-bit identical
// to what decoding that same nibble back will compute, so a round-trip
// (encode then decode) can't silently drift out of sync between the two
// implementations.

// imaEncodeNibble picks the 4-bit code that best encodes `sample` given the
// current predictor/step-index state (*hist1, *stepIndex), then advances
// that state via imaExpandNibble exactly as a decoder would when consuming
// the returned code. See the package-level comment above for why.
func imaEncodeNibble(sample int16, hist1 *int32, stepIndex *int32) int32 {
	step := imaStepTable[*stepIndex]
	diff := int32(sample) - *hist1

	code := int32(0)
	if diff < 0 {
		code = 8
		diff = -diff
	}

	// Bit-by-bit magnitude comparison, mirroring the decoder's own
	// delta = (step>>3) [+step>>2] [+step>>1] [+step] construction: whichever
	// bits would make the decoded delta come closest to (without exceeding)
	// the true diff get set here.
	t := step
	if diff >= t {
		code |= 4
		diff -= t
	}
	t >>= 1
	if diff >= t {
		code |= 2
		diff -= t
	}
	t >>= 1
	if diff >= t {
		code |= 1
	}

	imaExpandNibble(code, hist1, stepIndex) // advance state identically to decode
	return code
}

// encodeXboxIMAMono encodes mono PCM16 to the "XBOX IMA" FSB frame layout —
// the exact inverse framing of decodeXboxIMA's mono path (see
// docs/AUDIO_FORMAT.md's "XBOX IMA frame layout" section): fixed 0x24-byte
// frames, +0x00 s16 hist1 (the predictor BEFORE this frame's samples, not
// itself an emitted sample), +0x02 u8 step_index, +0x03 u8 reserved (0),
// +0x04 32 bytes / 64 nibbles (low nibble first) = 64 samples/frame.
//
// The very first frame starts from predictor=0, step_index=0 — the standard
// IMA-ADPCM starting convention, confirmed against this codebase's own
// decoder output for a real MAD2 sample (see fsb_test.go's
// TestFirstFrameStartsAtZero and docs/AUDIO_FORMAT.md's encode-path section).
//
// If len(pcm) isn't an exact multiple of 64, the final frame is padded with
// silence (0) — matches decodeXboxIMA's own "only whole 36-byte frames"
// handling and mirrors the trailing-pad slack already observed in real MAD2
// sample data (see docs/AUDIO_FORMAT.md's "4 bytes over" note).
//
// Scope: mono only — matches decodeXboxIMA's own verified scope exactly; the
// 2-channel interleaved layout is unexercised on the decode side too and not
// attempted here.
func encodeXboxIMAMono(pcm []int16) []byte {
	const blockSamples = 64
	numFrames := (len(pcm) + blockSamples - 1) / blockSamples
	if numFrames == 0 {
		return nil
	}
	out := make([]byte, numFrames*imaBlockSize)

	hist1 := int32(0)
	stepIndex := int32(0)

	for f := 0; f < numFrames; f++ {
		frame := out[f*imaBlockSize : (f+1)*imaBlockSize]

		// Frame header records the predictor/step-index state as it stands
		// BEFORE this frame's samples are encoded — matches decodeXboxIMA,
		// which seeds its own hist/step from these same header bytes and
		// then applies the frame's nibbles starting there.
		binary.LittleEndian.PutUint16(frame[0:2], uint16(int16(hist1)))
		frame[2] = byte(stepIndex)
		frame[3] = 0 // reserved

		start := f * blockSamples
		for i := 0; i < blockSamples; i++ {
			var sample int16
			if idx := start + i; idx < len(pcm) {
				sample = pcm[idx]
			} // else: silence-pad the final partial frame
			code := imaEncodeNibble(sample, &hist1, &stepIndex)
			byteIdx := 4 + i/2
			if i%2 == 0 {
				frame[byteIdx] = byte(code & 0xF)
			} else {
				frame[byteIdx] |= byte(code&0xF) << 4
			}
		}
	}
	return out
}

// RepackFSB3 re-encodes `pcm` (mono PCM16) and splices it into a copy of
// `original`'s own FSB3 bytes, producing a new, valid FSB3 container. Every
// header byte is carried through from `original` UNCHANGED — including the
// fields ParseFSB3 doesn't even bother extracting into FSBSample
// (MinDistance/MaxDistance/VarFreq/VarVol/VarPan, the 8 unexplained trailing
// bytes per docs/AUDIO_FORMAT.md) — except:
//   - the sample header entry's LengthSamples (+0x20, recomputed as
//     framecount*64) and LengthCompressedBytes (+0x24, recomputed as
//     framecount*36)
//   - the main header's DataSize (+0x0C, recomputed to match)
//
// This patches the original bytes in place rather than reserializing the
// header from scratch — the same "preserve everything except what actually
// changed" discipline EncodeInsert (texture.go) already uses for `.texs`,
// and it's what guarantees the open/unexplained fields survive a repack
// byte-for-byte instead of being silently zeroed or reconstructed wrong.
//
// Scope, matching decodeXboxIMA/encodeXboxIMAMono exactly: single-sample
// (NumSamples==1 — true for 100% of MAD2's real corpus, see
// docs/AUDIO_FORMAT.md), mono, FSOUND_IMAADPCM only. Anything outside that
// is rejected with a clear error rather than silently mishandled.
func RepackFSB3(original []byte, pcm []int16) ([]byte, error) {
	if !IsFSB3(original) {
		return nil, fmt.Errorf("RepackFSB3: not an FSB3 file (bad magic)")
	}
	if len(original) < fsbMainHeaderSize {
		return nil, fmt.Errorf("RepackFSB3: file too small (%d bytes) for FSB3 main header", len(original))
	}

	numSamples := int32(binary.LittleEndian.Uint32(original[4:8]))
	if numSamples != 1 {
		return nil, fmt.Errorf("RepackFSB3: only single-sample FSB3 containers are supported (got NumSamples=%d) -- matches 100%% of MAD2's real corpus, see docs/AUDIO_FORMAT.md", numSamples)
	}

	headerSize := int(binary.LittleEndian.Uint32(original[8:12]))
	headerAreaStart := fsbMainHeaderSize
	headerAreaEnd := headerAreaStart + headerSize
	if headerAreaEnd > len(original) || headerAreaEnd < headerAreaStart {
		return nil, fmt.Errorf("RepackFSB3: sample header area (%d bytes) exceeds file size %d", headerSize, len(original))
	}
	if headerAreaEnd < headerAreaStart+2 {
		return nil, fmt.Errorf("RepackFSB3: sample header area too small (%d bytes)", headerSize)
	}

	entrySize := int(binary.LittleEndian.Uint16(original[headerAreaStart : headerAreaStart+2]))
	if headerAreaStart+entrySize > headerAreaEnd {
		return nil, fmt.Errorf("RepackFSB3: sample header entry (%d bytes) exceeds the %d-byte header area", entrySize, headerSize)
	}
	if entrySize < 0x40 {
		return nil, fmt.Errorf("RepackFSB3: sample header entry too small (%d bytes) to contain Mode/NumChannels fields", entrySize)
	}
	// entrySize doesn't always exactly fill headerAreaEnd: real MAD2 samples
	// come in two entry-length variants (the "extended" 0x58-byte one, and a
	// shorter 0x50-byte one lacking nothing structurally different in the
	// fields this function actually reads/patches -- LengthSamples/
	// LengthCompressedBytes/Mode/NumChannels are at the same fixed offsets
	// from entry start in both), and the shorter variant can leave a few
	// trailing zero-padding bytes inside the declared header area, past the
	// entry's own self-reported size (confirmed against a real sample,
	// db_section2_hit7_marty.snds: entrySize=0x50/80 but headerSize=88, 8
	// trailing zero bytes). ParseFSB3 already tolerates this (it never
	// asserts pos==headerAreaEnd for a single-sample file); RepackFSB3
	// preserves those padding bytes unchanged too, since it copies the
	// WHOLE original[:headerAreaEnd] range below rather than just entrySize
	// bytes of it.

	entry := original[headerAreaStart:headerAreaEnd]
	mode := binary.LittleEndian.Uint32(entry[0x30:0x34])
	numChannels := int16(binary.LittleEndian.Uint16(entry[0x3E:0x40]))
	if numChannels == 0 {
		numChannels = 1
	}
	if mode&FSoundImaAdpcm == 0 {
		return nil, fmt.Errorf("RepackFSB3: sample Mode 0x%08X is not FSOUND_IMAADPCM -- only the IMA-ADPCM encode path is implemented", mode)
	}
	if numChannels != 1 {
		return nil, fmt.Errorf("RepackFSB3: sample has %d channels -- only mono re-encoding is implemented (matches decodeXboxIMA's own verified scope)", numChannels)
	}

	encoded := encodeXboxIMAMono(pcm)
	numFrames := len(encoded) / imaBlockSize
	lengthSamples := uint32(numFrames) * 64
	compressedBytes := uint32(len(encoded))

	out := make([]byte, headerAreaEnd+len(encoded))
	copy(out, original[:headerAreaEnd]) // main header + sample header entry, byte-for-byte from original
	binary.LittleEndian.PutUint32(out[12:16], compressedBytes)                 // main header DataSize
	binary.LittleEndian.PutUint32(out[headerAreaStart+0x20:], lengthSamples)   // entry LengthSamples
	binary.LittleEndian.PutUint32(out[headerAreaStart+0x24:], compressedBytes) // entry LengthCompressedBytes
	copy(out[headerAreaEnd:], encoded)

	return out, nil
}

// ReadPCM16WAV parses a 16-bit PCM RIFF/WAVE file — the inverse of
// WritePCM16WAV. Chunks are walked generically (not assumed at fixed
// offsets) so this tolerates minor real-world variation a real editor might
// produce when saving an edited copy of a WritePCM16WAV-produced file (e.g.
// an inserted "LIST"/"fact" chunk, a "fmt " chunk padded past 16 bytes) —
// this matters because parsing an edited WAV, not just one this package
// wrote itself, is the actual path RepackSnds/mad2tool exercises.
func ReadPCM16WAV(r io.Reader) (pcm []int16, channels, sampleRate int, err error) {
	data, err := io.ReadAll(r)
	if err != nil {
		return nil, 0, 0, err
	}
	if len(data) < 12 || string(data[0:4]) != "RIFF" || string(data[8:12]) != "WAVE" {
		return nil, 0, 0, fmt.Errorf("not a RIFF/WAVE file")
	}

	pos := 12
	var fmtFound, dataFound bool
	var audioFormat, bitsPerSample uint16
	var dataBytes []byte

	for pos+8 <= len(data) {
		chunkID := string(data[pos : pos+4])
		chunkSize := int(binary.LittleEndian.Uint32(data[pos+4 : pos+8]))
		chunkStart := pos + 8
		chunkEnd := chunkStart + chunkSize
		if chunkEnd > len(data) || chunkEnd < chunkStart {
			chunkEnd = len(data)
		}

		switch chunkID {
		case "fmt ":
			if chunkEnd-chunkStart < 16 {
				return nil, 0, 0, fmt.Errorf("fmt chunk too small (%d bytes)", chunkEnd-chunkStart)
			}
			fc := data[chunkStart:chunkEnd]
			audioFormat = binary.LittleEndian.Uint16(fc[0:2])
			channels = int(binary.LittleEndian.Uint16(fc[2:4]))
			sampleRate = int(binary.LittleEndian.Uint32(fc[4:8]))
			bitsPerSample = binary.LittleEndian.Uint16(fc[14:16])
			fmtFound = true
		case "data":
			dataBytes = data[chunkStart:chunkEnd]
			dataFound = true
		}

		pos = chunkEnd
		if chunkSize%2 == 1 { // chunks are word-aligned; odd-sized chunks have a pad byte
			pos++
		}
	}

	if !fmtFound {
		return nil, 0, 0, fmt.Errorf("no fmt chunk found")
	}
	if !dataFound {
		return nil, 0, 0, fmt.Errorf("no data chunk found")
	}
	if audioFormat != 1 {
		return nil, 0, 0, fmt.Errorf("unsupported WAV audio format %d (only PCM=1 supported)", audioFormat)
	}
	if bitsPerSample != 16 {
		return nil, 0, 0, fmt.Errorf("unsupported bits-per-sample %d (only 16-bit PCM supported)", bitsPerSample)
	}
	if channels < 1 {
		channels = 1
	}

	n := len(dataBytes) / 2
	pcm = make([]int16, n)
	for i := 0; i < n; i++ {
		pcm[i] = int16(binary.LittleEndian.Uint16(dataBytes[i*2:]))
	}
	return pcm, channels, sampleRate, nil
}

// RepackSnds is the one-call convenience counterpart to ExtractSnds: given
// the ORIGINAL `.snds` file's raw bytes (needed as the header template — see
// RepackFSB3) and an edited WAV's raw bytes, produces new `.snds` bytes with
// the edited audio re-encoded in. Only handles the FSB3 case (mono
// IMA-ADPCM); the Ogg-Vorbis-minority `.snds` files need no encoder at all
// (IsOggVorbis + a plain rename is already lossless) and should not be
// passed here.
func RepackSnds(originalSndsData []byte, wavData []byte) ([]byte, error) {
	if !IsFSB3(originalSndsData) {
		return nil, fmt.Errorf("RepackSnds: original .snds is not FSB3 (Ogg-Vorbis .snds files need no encoder -- see IsOggVorbis)")
	}
	pcm, channels, _, err := ReadPCM16WAV(bytes.NewReader(wavData))
	if err != nil {
		return nil, fmt.Errorf("RepackSnds: read edited wav: %w", err)
	}
	if channels != 1 {
		return nil, fmt.Errorf("RepackSnds: edited wav has %d channels, only mono re-encoding is implemented", channels)
	}
	return RepackFSB3(originalSndsData, pcm)
}

// WritePCM16WAV writes a minimal canonical 16-bit PCM RIFF/WAVE file.
func WritePCM16WAV(w io.Writer, pcm []int16, channels, sampleRate int) error {
	if channels < 1 {
		channels = 1
	}
	dataSize := len(pcm) * 2
	byteRate := sampleRate * channels * 2
	blockAlign := channels * 2

	var buf bytes.Buffer
	buf.WriteString("RIFF")
	binary.Write(&buf, binary.LittleEndian, uint32(36+dataSize))
	buf.WriteString("WAVE")
	buf.WriteString("fmt ")
	binary.Write(&buf, binary.LittleEndian, uint32(16))
	binary.Write(&buf, binary.LittleEndian, uint16(1)) // PCM
	binary.Write(&buf, binary.LittleEndian, uint16(channels))
	binary.Write(&buf, binary.LittleEndian, uint32(sampleRate))
	binary.Write(&buf, binary.LittleEndian, uint32(byteRate))
	binary.Write(&buf, binary.LittleEndian, uint16(blockAlign))
	binary.Write(&buf, binary.LittleEndian, uint16(16))
	buf.WriteString("data")
	binary.Write(&buf, binary.LittleEndian, uint32(dataSize))
	if err := binary.Write(&buf, binary.LittleEndian, pcm); err != nil {
		return err
	}
	_, err := w.Write(buf.Bytes())
	return err
}

// ExtractSnds decodes a `.snds` file's raw bytes to a playable audio file.
// Some `.snds` files are plain Ogg Vorbis streams reusing the extension
// (see IsOggVorbis) and are returned unchanged with ext=".ogg"; everything
// else is parsed as FSB3 and its first embedded sample is decoded to a
// canonical WAV (ext=".wav"). MAD2's `.snds` files are single-sample in
// every case sampled so far — see docs/AUDIO_FORMAT.md — so only the first
// sample is extracted; ParseFSB3 exposes the rest if that's ever not true.
func ExtractSnds(data []byte) (out []byte, ext string, err error) {
	if IsOggVorbis(data) {
		return data, ".ogg", nil
	}
	fsb, err := ParseFSB3(data)
	if err != nil {
		return nil, "", err
	}
	if len(fsb.Samples) == 0 {
		return nil, "", fmt.Errorf("FSB3 file has no samples")
	}
	s := fsb.Samples[0]
	pcm, channels, rate, err := s.DecodeToPCM16()
	if err != nil {
		return nil, "", err
	}
	var buf bytes.Buffer
	if err := WritePCM16WAV(&buf, pcm, channels, rate); err != nil {
		return nil, "", err
	}
	return buf.Bytes(), ".wav", nil
}
