// Package iga implements parsing and extraction of Intrinsic Graphics Archive (IGA)
// files used by the Vicarious Visions / Toys for Bob Alchemy game engine.
//
// This implementation targets version 0x02 of the format as used by
// Madagascar: Escape 2 Africa (PC), but the structures are similar across
// other Alchemy engine titles.
//
// Based on research from the igArchiveExtractor project (GPL-3.0) by
// NefariousTechSupport, with additional reverse engineering for the v0x02
// variant used by Madagascar 2.
package iga

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"
)

// Magic is the 4-byte file signature: "IGA\x1A" in little-endian (0x1A414749).
const Magic uint32 = 0x1A414749

// MagicBE is the same signature in big-endian byte order (0x4947411A).
const MagicBE uint32 = 0x4947411A

// ChunkDecompressedSize is the size of each decompressed zlib chunk (32 KiB).
const ChunkDecompressedSize = 0x8000

// ChunkAlignment is the alignment boundary for data chunks in compressed files.
const ChunkAlignment = 0x800

// CompressionMode constants.
const (
	ModeUncompressed uint32 = 0xFFFFFFFF
)

// Header represents the fixed 48-byte header at the start of every IGA archive.
type Header struct {
	Magic           uint32    // 0x00: Must be 0x1A414749 (LE) or 0x4947411A (BE)
	Version         uint32    // 0x04: Format version (0x02 for MAD2)
	FileTableSize   uint32    // 0x08: Total size of CRCs + descriptors + chunk properties
	FileCount       uint32    // 0x0C: Number of files in the archive
	HashSearchDiv   uint32    // 0x10: Hash search divisor (0xFFFFFFFF / FileCount)
	MemoryPoolIndex uint32    // 0x14: Memory pool index
	NametableOffset uint32    // 0x18: Absolute offset to the nametable
	NametableSize   uint32    // 0x1C: Size of the nametable in bytes
	Reserved        [4]uint32 // 0x20-0x2F: Reserved / padding (always zero)
}

// FileDescriptor describes a single file entry within the archive.
type FileDescriptor struct {
	DataOffset       uint32 // Absolute offset to the file's data in the archive
	DecompressedSize uint32 // Size of the file after decompression
	Mode             uint32 // Compression mode / chunk properties offset
}

// FileEntry combines all metadata for a single file in the archive.
type FileEntry struct {
	Index      uint32
	Name       string
	CRC        uint32
	Descriptor FileDescriptor
}

// Archive represents a parsed IGA archive file.
type Archive struct {
	FilePath  string
	Header    Header
	Entries   []FileEntry
	BigEndian bool // true if the archive uses big-endian byte order

	file      *os.File
	byteOrder binary.ByteOrder
}

// Open parses an IGA archive from the given file path.
func Open(path string) (*Archive, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open archive: %w", err)
	}

	arc := &Archive{
		FilePath: path,
		file:     f,
	}

	if err := arc.parseHeader(); err != nil {
		f.Close()
		return nil, err
	}

	if err := arc.parseFileTable(); err != nil {
		f.Close()
		return nil, err
	}

	if err := arc.parseNametable(); err != nil {
		f.Close()
		return nil, err
	}

	return arc, nil
}

// Close releases the underlying file handle.
func (a *Archive) Close() error {
	if a.file != nil {
		return a.file.Close()
	}
	return nil
}

// parseHeader reads and validates the 48-byte archive header.
func (a *Archive) parseHeader() error {
	var magic uint32
	if err := binary.Read(a.file, binary.LittleEndian, &magic); err != nil {
		return fmt.Errorf("read magic: %w", err)
	}

	switch magic {
	case Magic:
		a.byteOrder = binary.LittleEndian
		a.BigEndian = false
	case MagicBE:
		a.byteOrder = binary.BigEndian
		a.BigEndian = true
	default:
		return fmt.Errorf("invalid magic number: 0x%08X (expected IGA\\x1A)", magic)
	}

	a.Header.Magic = Magic // normalise

	// Seek back and re-read the full header with correct endianness
	if _, err := a.file.Seek(4, io.SeekStart); err != nil {
		return err
	}

	if err := binary.Read(a.file, a.byteOrder, &a.Header.Version); err != nil {
		return fmt.Errorf("read version: %w", err)
	}
	if err := binary.Read(a.file, a.byteOrder, &a.Header.FileTableSize); err != nil {
		return fmt.Errorf("read file table size: %w", err)
	}
	if err := binary.Read(a.file, a.byteOrder, &a.Header.FileCount); err != nil {
		return fmt.Errorf("read file count: %w", err)
	}
	if err := binary.Read(a.file, a.byteOrder, &a.Header.HashSearchDiv); err != nil {
		return fmt.Errorf("read hash search div: %w", err)
	}
	if err := binary.Read(a.file, a.byteOrder, &a.Header.MemoryPoolIndex); err != nil {
		return fmt.Errorf("read memory pool index: %w", err)
	}
	if err := binary.Read(a.file, a.byteOrder, &a.Header.NametableOffset); err != nil {
		return fmt.Errorf("read nametable offset: %w", err)
	}
	if err := binary.Read(a.file, a.byteOrder, &a.Header.NametableSize); err != nil {
		return fmt.Errorf("read nametable size: %w", err)
	}
	if err := binary.Read(a.file, a.byteOrder, &a.Header.Reserved); err != nil {
		return fmt.Errorf("read reserved: %w", err)
	}

	return nil
}

// parseFileTable reads CRCs and file descriptors from the file table.
func (a *Archive) parseFileTable() error {
	n := a.Header.FileCount
	a.Entries = make([]FileEntry, n)

	// CRCs start at offset 0x30
	if _, err := a.file.Seek(0x30, io.SeekStart); err != nil {
		return fmt.Errorf("seek to CRCs: %w", err)
	}

	for i := uint32(0); i < n; i++ {
		var crc uint32
		if err := binary.Read(a.file, a.byteOrder, &crc); err != nil {
			return fmt.Errorf("read CRC %d: %w", i, err)
		}
		a.Entries[i].Index = i
		a.Entries[i].CRC = crc
	}

	// File descriptors follow immediately after CRCs (12 bytes each for v0x02)
	for i := uint32(0); i < n; i++ {
		var desc FileDescriptor
		if err := binary.Read(a.file, a.byteOrder, &desc); err != nil {
			return fmt.Errorf("read descriptor %d: %w", i, err)
		}
		a.Entries[i].Descriptor = desc
	}

	return nil
}

// parseNametable reads filenames from the nametable at the end of the archive.
func (a *Archive) parseNametable() error {
	ntOff := int64(a.Header.NametableOffset)
	ntSize := int64(a.Header.NametableSize)

	if ntOff == 0 || ntSize == 0 {
		// No nametable; generate placeholder names
		for i := range a.Entries {
			a.Entries[i].Name = fmt.Sprintf("file_%04d.bin", i)
		}
		return nil
	}

	// Read entire nametable into memory
	ntData := make([]byte, ntSize)
	if _, err := a.file.ReadAt(ntData, ntOff); err != nil {
		return fmt.Errorf("read nametable: %w", err)
	}

	n := a.Header.FileCount

	// First N uint32s are offsets relative to the nametable start
	for i := uint32(0); i < n; i++ {
		if int(i*4+4) > len(ntData) {
			return fmt.Errorf("nametable offset entry %d out of bounds", i)
		}
		nameOff := a.byteOrder.Uint32(ntData[i*4 : i*4+4])

		if int(nameOff) >= len(ntData) {
			a.Entries[i].Name = fmt.Sprintf("file_%04d.bin", i)
			continue
		}

		// Read null-terminated string
		end := int(nameOff)
		for end < len(ntData) && ntData[end] != 0 {
			end++
		}
		a.Entries[i].Name = string(ntData[nameOff:end])
	}

	return nil
}

// IsCompressed returns true if the file entry uses zlib chunk compression.
func (e *FileEntry) IsCompressed() bool {
	return e.Descriptor.Mode != ModeUncompressed
}

// ExtractFile extracts a single file from the archive to the given writer.
func (a *Archive) ExtractFile(index int, w io.Writer) error {
	if index < 0 || index >= len(a.Entries) {
		return fmt.Errorf("file index %d out of range [0, %d)", index, len(a.Entries))
	}

	entry := &a.Entries[index]

	if !entry.IsCompressed() {
		return a.extractUncompressed(entry, w)
	}

	return a.extractCompressed(entry, w)
}

// ExtractRawData extracts the raw, potentially compressed blocks of data for a file to the writer.
func (a *Archive) ExtractRawData(index int, w io.Writer) error {
	if index < 0 || index >= len(a.Entries) {
		return fmt.Errorf("file index %d out of range [0, %d)", index, len(a.Entries))
	}

	entry := &a.Entries[index]

	if !entry.IsCompressed() {
		return a.extractUncompressed(entry, w)
	}

	offset := int64(entry.Descriptor.DataOffset)
	remaining := int64(entry.Descriptor.DecompressedSize)
	pos := offset

	for remaining > 0 {
		sizeBuf := make([]byte, 2)
		if _, err := a.file.ReadAt(sizeBuf, pos); err != nil {
			return fmt.Errorf("read chunk size at 0x%08X: %w", pos, err)
		}
		compSize := int64(binary.BigEndian.Uint16(sizeBuf))
		if compSize == 0 {
			pos = alignUp(pos+2, ChunkAlignment)
			continue
		}

		chunkSize := int64(ChunkDecompressedSize)
		if remaining < chunkSize {
			chunkSize = remaining
		}
		remaining -= chunkSize

		pos = alignUp(pos+2+compSize, ChunkAlignment)
	}

	sr := io.NewSectionReader(a.file, offset, pos-offset)
	_, err := io.Copy(w, sr)
	return err
}

// extractUncompressed copies raw uncompressed data from the archive.
func (a *Archive) extractUncompressed(entry *FileEntry, w io.Writer) error {
	offset := int64(entry.Descriptor.DataOffset)
	size := int64(entry.Descriptor.DecompressedSize)

	sr := io.NewSectionReader(a.file, offset, size)
	_, err := io.Copy(w, sr)
	return err
}

// extractCompressed decompresses chunked zlib data.
//
// Each compressed chunk has the layout:
//
//	[2 bytes big-endian] compressed_size
//	[compressed_size bytes] zlib-compressed data
//	[padding to next ChunkAlignment boundary]
//
// Each chunk decompresses to ChunkDecompressedSize (32 KiB) except
// potentially the last chunk.
//
// Some chunks are stored uncompressed when compression is not beneficial.
// These are detected by checking for a valid zlib header (0x78 as first byte).
// If the header is invalid, the chunk is treated as raw uncompressed data
// and 0x8000 bytes are read from the original position instead.
func (a *Archive) extractCompressed(entry *FileEntry, w io.Writer) error {
	offset := int64(entry.Descriptor.DataOffset)
	remaining := int64(entry.Descriptor.DecompressedSize)

	pos := offset

	for remaining > 0 {
		// Read 2-byte big-endian compressed size
		sizeBuf := make([]byte, 2)
		if _, err := a.file.ReadAt(sizeBuf, pos); err != nil {
			return fmt.Errorf("read chunk size at 0x%08X: %w", pos, err)
		}
		compSize := int64(binary.BigEndian.Uint16(sizeBuf))

		if compSize == 0 {
			// Zero-size means padding; align to next boundary
			pos = alignUp(pos+2, ChunkAlignment)
			continue
		}

		// Peek at the first byte of data after the 2-byte size
		headerBuf := make([]byte, 1)
		if _, err := a.file.ReadAt(headerBuf, pos+2); err != nil {
			return fmt.Errorf("peek chunk header at 0x%08X: %w", pos+2, err)
		}

		decompressed := false

		// Valid zlib headers start with 0x78 (deflate, 32K window).
		// Attempt zlib decompression; fall back to raw on any error,
		// since some uncompressed chunks can start with 0x78 by coincidence.
		if headerBuf[0] == 0x78 {
			compData := make([]byte, compSize)
			if _, err := a.file.ReadAt(compData, pos+2); err != nil {
				return fmt.Errorf("read chunk data at 0x%08X: %w", pos+2, err)
			}

			if zr, err := zlib.NewReader(bytes.NewReader(compData)); err == nil {
				var buf bytes.Buffer
				n, derr := io.Copy(&buf, zr)
				zr.Close()
				if derr == nil && n > 0 {
					if _, werr := w.Write(buf.Bytes()); werr != nil {
						return fmt.Errorf("write decompressed chunk: %w", werr)
					}
					remaining -= n
					pos += 2 + compSize
					decompressed = true
				}
			}
		}

		if !decompressed {
			// Uncompressed chunk — read raw data from current position.
			// The 2-byte "size" field is part of the raw data stream.
			chunkSize := int64(ChunkDecompressedSize)
			if remaining < chunkSize {
				chunkSize = remaining
			}

			rawData := make([]byte, chunkSize)
			if _, err := a.file.ReadAt(rawData, pos); err != nil {
				return fmt.Errorf("read raw chunk at 0x%08X: %w", pos, err)
			}

			if _, err := w.Write(rawData); err != nil {
				return fmt.Errorf("write raw chunk: %w", err)
			}

			remaining -= chunkSize
			pos += chunkSize
		}

		// Align position to next chunk boundary
		pos = alignUp(pos, ChunkAlignment)
	}

	return nil
}

// alignUp rounds pos up to the next multiple of alignment.
func alignUp(pos int64, alignment int64) int64 {
	if pos%alignment == 0 {
		return pos
	}
	return ((pos + alignment - 1) / alignment) * alignment
}

// SanitizePath converts an absolute game path like
// "c:/tfb/content/builddata/win/foo.texs" into a relative path
// suitable for extraction (e.g., "builddata/win/foo.texs").
func SanitizePath(name string) string {
	// Normalise separators
	name = strings.ReplaceAll(name, "\\", "/")

	// Strip drive letter prefix (e.g., "c:/")
	if len(name) >= 3 && name[1] == ':' && name[2] == '/' {
		name = name[3:]
	}

	// Strip leading slash
	name = strings.TrimLeft(name, "/")

	// Try to strip common prefixes used by Toys for Bob
	prefixes := []string{
		"tfb/content/",
		"content/",
	}
	lower := strings.ToLower(name)
	for _, prefix := range prefixes {
		if strings.HasPrefix(lower, prefix) {
			name = name[len(prefix):]
			break
		}
	}

	return name
}
