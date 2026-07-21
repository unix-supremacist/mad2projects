package iga

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

// RepackEntry describes a single file to include in a repacked archive.
type RepackEntry struct {
	Name             string // Original path stored in the nametable
	CRC              uint32 // FNV-1a hash of the name
	Data             []byte // File contents (uncompressed, or raw compressed if KeepZlib is true)
	Offset           uint32 // Optional original offset to preserve file layout order
	DecompressedSize uint32 // Required if KeepZlib is true
	Mode             uint32 // Required if KeepZlib is true (original mode index)
}

// RepackOptions controls how the archive is written.
type RepackOptions struct {
	Version            uint32    // Archive version (default 0x02)
	MemoryPoolIndex    uint32    // Memory pool index from original header
	OriginalReserved   [4]uint32 // Optional original reserved fields to match tables
	OriginalProperties []uint16  // Optional raw properties table from original archive
	KeepZlib           bool      // If true, entries' Data fields contain raw compressed bytes
}

// Repack writes a new IGA archive to outputPath using the given entries.
// If MemoryPoolIndex is 1, it chunk-compresses the files using zlib.
// Entries are sorted by CRC for the hash lookup table.
func Repack(outputPath string, entries []RepackEntry, opts RepackOptions) error {
	if opts.Version == 0 {
		opts.Version = 0x02
	}

	n := uint32(len(entries))
	if n == 0 {
		return fmt.Errorf("no entries to pack")
	}

	// Sort entries by CRC (ascending) as required by the engine's hash search
	sorted := make([]RepackEntry, len(entries))
	copy(sorted, entries)
	sort.Slice(sorted, func(i, j int) bool {
		return sorted[i].CRC < sorted[j].CRC
	})

	isCompressed := opts.MemoryPoolIndex == 1

	// We determine the data layout order.
	// If entries contain original offset values, we order their data region placement accordingly.
	hasOffsets := false
	for _, entry := range entries {
		if entry.Offset != 0 {
			hasOffsets = true
			break
		}
	}

	layoutOrder := make([]RepackEntry, len(sorted))
	copy(layoutOrder, sorted)

	if hasOffsets {
		// Sort layoutOrder by original Offset
		sort.Slice(layoutOrder, func(i, j int) bool {
			return layoutOrder[i].Offset < layoutOrder[j].Offset
		})
	}

	type CompressedFile struct {
		CRC          uint32
		Data         []byte
		ChunkOffsets []uint32 // sector offsets relative to the start of the file's data
		TotalSectors uint32
	}

	compressedMap := make(map[uint32]*CompressedFile)

	if isCompressed {
		for _, entry := range layoutOrder {
			if opts.KeepZlib {
				var chunkOffsets []uint32
				remaining := int64(entry.DecompressedSize)
				pos := int64(0)
				fileChunksCount := 0

				for remaining > 0 {
					if pos+2 > int64(len(entry.Data)) {
						break
					}
					compSize := int64(binary.BigEndian.Uint16(entry.Data[pos : pos+2]))
					if compSize == 0 {
						pos = int64(alignUp32(uint32(pos+2), ChunkAlignment))
						continue
					}

					chunkOffsets = append(chunkOffsets, uint32(pos)/ChunkAlignment)

					chunkSize := int64(ChunkDecompressedSize)
					if remaining < chunkSize {
						chunkSize = remaining
					}
					remaining -= chunkSize

					pos = int64(alignUp32(uint32(pos+2+compSize), ChunkAlignment))
					fileChunksCount++
				}

				totalSectors := uint32(len(entry.Data)) / ChunkAlignment
				if fileChunksCount == 0 {
					chunkOffsets = append(chunkOffsets, 0)
				}

				compressedMap[entry.CRC] = &CompressedFile{
					CRC:          entry.CRC,
					Data:         entry.Data,
					ChunkOffsets: chunkOffsets,
					TotalSectors: totalSectors,
				}
				continue
			}

			var fileBuf bytes.Buffer
			data := entry.Data
			remaining := len(data)
			offset := 0

			var chunkOffsets []uint32
			fileChunksCount := 0

			for remaining > 0 {
				chunkSize := ChunkDecompressedSize
				if remaining < chunkSize {
					chunkSize = remaining
				}

				rawChunk := data[offset : offset+chunkSize]

				// Sector offset of this chunk relative to start of file data
				chunkOffsets = append(chunkOffsets, uint32(fileBuf.Len())/ChunkAlignment)

				// Compress chunk
				var compBuf bytes.Buffer
				zw, err := zlib.NewWriterLevel(&compBuf, zlib.BestCompression)
				if err != nil {
					return fmt.Errorf("zlib init error: %w", err)
				}
				if _, err := zw.Write(rawChunk); err != nil {
					return fmt.Errorf("zlib write error: %w", err)
				}
				zw.Close()

				compBytes := compBuf.Bytes()
				compSize := len(compBytes)

				// Write size prefix
				sizeBuf := make([]byte, 2)
				binary.BigEndian.PutUint16(sizeBuf, uint16(compSize))

				fileBuf.Write(sizeBuf)
				fileBuf.Write(compBytes)

				// Align to next boundary (zero padded)
				padLen := alignUp32(uint32(fileBuf.Len()), ChunkAlignment) - uint32(fileBuf.Len())
				if padLen > 0 {
					fileBuf.Write(make([]byte, padLen))
				}

				fileChunksCount++
				offset += chunkSize
				remaining -= chunkSize
			}

			totalSectors := uint32(fileBuf.Len()) / ChunkAlignment
			if fileChunksCount == 0 {
				// Empty file has at least 1 chunk placeholder
				chunkOffsets = append(chunkOffsets, 0)
			}

			compressedMap[entry.CRC] = &CompressedFile{
				CRC:          entry.CRC,
				Data:         fileBuf.Bytes(),
				ChunkOffsets: chunkOffsets,
				TotalSectors: totalSectors,
			}
		}
	}

	// Layout planning
	hasReserved := false
	for _, val := range opts.OriginalReserved {
		if val > 0 {
			hasReserved = true
			break
		}
	}

	isLevelFile := func(name string) bool {
		return strings.HasSuffix(strings.ToLower(name), ".bld")
	}

	var levelTable int
	var pakTable int

	if hasReserved {
		if opts.OriginalReserved[0] > 0 {
			levelTable = 0
		} else {
			levelTable = 1
		}
		if opts.OriginalReserved[2] > 0 {
			pakTable = 2
		} else {
			pakTable = 1
		}
	} else {
		levelTable = 1
		pakTable = 2
	}

	var firstDataOffset uint32
	var fileStartSectors map[uint32]uint32

	for iter := 0; iter < 5; iter++ {
		var table0Count uint32
		var table1Count uint32
		var table2Count uint32

		for _, entry := range layoutOrder {
			var chunks int
			if isCompressed {
				chunks = len(compressedMap[entry.CRC].ChunkOffsets) + 1
			} else {
				chunks = 1
			}

			var t int
			if isLevelFile(entry.Name) {
				t = levelTable
			} else {
				t = pakTable
			}

			if t == 0 {
				table0Count += uint32(chunks)
			} else if t == 1 {
				table1Count += uint32(chunks)
			} else {
				table2Count += uint32(chunks)
			}
		}

		var totalPropsSize uint32
		if isCompressed {
			totalPropsSize = table0Count*4 + table1Count*2 + table2Count
		}
		fileTableSize := n*16 + totalPropsSize
		firstDataOffset = alignUp32(uint32(0x30)+fileTableSize, ChunkAlignment)

		fileStartSectors = make(map[uint32]uint32)
		currentSector := firstDataOffset / ChunkAlignment

		changed := false
		for _, entry := range layoutOrder {
			fileStartSectors[entry.CRC] = currentSector

			var size uint32
			if isCompressed {
				cf := compressedMap[entry.CRC]
				size = uint32(len(cf.Data))

				if isLevelFile(entry.Name) && !hasReserved {
					maxSector := currentSector + cf.TotalSectors
					if maxSector >= 0x8000 && levelTable != 0 {
						levelTable = 0
						pakTable = 1
						changed = true
					}
				}
			} else {
				size = uint32(len(entry.Data))
			}

			currentSector += alignUp32(size, ChunkAlignment) / ChunkAlignment
		}

		if !changed {
			break
		}
	}

	// Build chunk properties table
	var table0Props []uint16
	var table1Props []uint16
	var table2Props []uint16

	table0Indices := make(map[uint32]uint32)
	table1Indices := make(map[uint32]uint32)
	table2Indices := make(map[uint32]uint32)

	var currentTable0Idx uint32
	var currentTable1Idx uint32
	var currentTable2Idx uint32

	if isCompressed {
		// Table 0 properties first
		for _, entry := range layoutOrder {
			var t int
			if isLevelFile(entry.Name) {
				t = levelTable
			} else {
				t = pakTable
			}
			if t != 0 {
				continue
			}
			cf := compressedMap[entry.CRC]
			table0Indices[entry.CRC] = currentTable0Idx

			C := len(cf.ChunkOffsets)
			for j := 0; j < C; j++ {
				sec := cf.ChunkOffsets[j]
				table0Props = append(table0Props, uint16(sec&0xFFFF))
				table0Props = append(table0Props, uint16(0x8000|(sec>>16)))
			}
			table0Props = append(table0Props, uint16(cf.TotalSectors&0xFFFF))
			table0Props = append(table0Props, uint16(cf.TotalSectors>>16))

			currentTable0Idx += uint32(C + 1)
		}

		// Table 1 properties second
		for _, entry := range layoutOrder {
			var t int
			if isLevelFile(entry.Name) {
				t = levelTable
			} else {
				t = pakTable
			}
			if t != 1 {
				continue
			}
			cf := compressedMap[entry.CRC]
			table1Indices[entry.CRC] = currentTable1Idx

			C := len(cf.ChunkOffsets)
			for j := 0; j < C; j++ {
				sec := cf.ChunkOffsets[j]
				table1Props = append(table1Props, uint16(0x8000|sec))
			}
			table1Props = append(table1Props, uint16(cf.TotalSectors))

			currentTable1Idx += uint32(C + 1)
		}

		// Table 2 properties third (grouped by field for pak files)
		for _, entry := range layoutOrder {
			var t int
			if isLevelFile(entry.Name) {
				t = levelTable
			} else {
				t = pakTable
			}
			if t != 2 {
				continue
			}
			cf := compressedMap[entry.CRC]
			table2Indices[entry.CRC] = currentTable2Idx
			table2Props = append(table2Props, uint16((cf.TotalSectors<<8)|0x80))
			currentTable2Idx += 2
		}
		for _, entry := range layoutOrder {
			var t int
			if isLevelFile(entry.Name) {
				t = levelTable
			} else {
				t = pakTable
			}
			if t != 2 {
				continue
			}
			table2Props = append(table2Props, uint16(0))
		}
	}

	chunkProps := append(table0Props, table1Props...)
	chunkProps = append(chunkProps, table2Props...)

	fileTableSize := n*16 + uint32(len(chunkProps)*2) - currentTable2Idx

	hashSearchDiv := uint32(0xFFFFFFFF) / n

	// Create output file
	f, err := os.Create(outputPath)
	if err != nil {
		return fmt.Errorf("create output: %w", err)
	}
	defer f.Close()

	headerBuf := make([]byte, firstDataOffset)

	// Write magic
	binary.LittleEndian.PutUint32(headerBuf[0x00:], Magic)
	// Version
	binary.LittleEndian.PutUint32(headerBuf[0x04:], opts.Version)
	// FileTableSize
	binary.LittleEndian.PutUint32(headerBuf[0x08:], fileTableSize)
	// FileCount
	binary.LittleEndian.PutUint32(headerBuf[0x0C:], n)
	// HashSearchDiv
	binary.LittleEndian.PutUint32(headerBuf[0x10:], hashSearchDiv)
	// MemoryPoolIndex
	binary.LittleEndian.PutUint32(headerBuf[0x14:], opts.MemoryPoolIndex)

	// Set Reserved fields to the number of properties in each table
	binary.LittleEndian.PutUint32(headerBuf[0x20:], currentTable0Idx)
	binary.LittleEndian.PutUint32(headerBuf[0x24:], currentTable1Idx)
	binary.LittleEndian.PutUint32(headerBuf[0x28:], currentTable2Idx)

	// Write CRCs at 0x30
	for i, entry := range sorted {
		binary.LittleEndian.PutUint32(headerBuf[0x30+uint32(i)*4:], entry.CRC)
	}

	entryOffsets := make(map[uint32]uint32)
	currentOffset := firstDataOffset

	for _, entry := range layoutOrder {
		entryOffsets[entry.CRC] = currentOffset
		var dataSize uint32
		if isCompressed {
			dataSize = uint32(len(compressedMap[entry.CRC].Data))
		} else {
			dataSize = uint32(len(entry.Data))
		}
		currentOffset = alignUp32(currentOffset+dataSize, ChunkAlignment)
	}

	// Write descriptors in CRC-sorted order
	descBase := 0x30 + n*4
	for i, entry := range sorted {
		offsetVal := entryOffsets[entry.CRC]
		var size uint32
		var mode uint32

		if isCompressed {
			var t int
			if isLevelFile(entry.Name) {
				t = levelTable
			} else {
				t = pakTable
			}
			if opts.KeepZlib {
				size = entry.DecompressedSize
			} else {
				size = uint32(len(entry.Data))
			}
			if opts.KeepZlib {
				mode = entry.Mode
			} else {
				if t == 0 {
					mode = table0Indices[entry.CRC]
				} else if t == 1 {
					mode = table1Indices[entry.CRC]
				} else {
					mode = table2Indices[entry.CRC]
				}
			}
		} else {
			size = uint32(len(entry.Data))
			mode = ModeUncompressed
		}

		dOff := descBase + uint32(i)*12
		binary.LittleEndian.PutUint32(headerBuf[dOff+0:], offsetVal)
		binary.LittleEndian.PutUint32(headerBuf[dOff+4:], size)
		binary.LittleEndian.PutUint32(headerBuf[dOff+8:], mode)
	}

	// Write chunk properties table
	if isCompressed {
		propBase := descBase + n*12
		for i, prop := range chunkProps {
			binary.LittleEndian.PutUint16(headerBuf[propBase+uint32(i)*2:], prop)
		}
	}

	// Nametable starts after all file data (aligned)
	nametableOffset := currentOffset

	// Build nametable
	nametable := buildNametable(sorted)
	nametableSize := uint32(len(nametable))

	// Write nametable offset and size in header
	binary.LittleEndian.PutUint32(headerBuf[0x18:], nametableOffset)
	binary.LittleEndian.PutUint32(headerBuf[0x1C:], nametableSize)

	// Write header + table to file
	if _, err := f.Write(headerBuf); err != nil {
		return fmt.Errorf("write header: %w", err)
	}

	// Write file data sequentially as specified by layoutOrder
	for _, entry := range layoutOrder {
		offsetVal := entryOffsets[entry.CRC]
		if _, err := f.Seek(int64(offsetVal), io.SeekStart); err != nil {
			return fmt.Errorf("seek to file %s data: %w", entry.Name, err)
		}

		if isCompressed {
			cData := compressedMap[entry.CRC].Data
			if _, err := f.Write(cData); err != nil {
				return fmt.Errorf("write compressed file %s data: %w", entry.Name, err)
			}
		} else {
			if _, err := f.Write(entry.Data); err != nil {
				return fmt.Errorf("write file %s data: %w", entry.Name, err)
			}
		}
	}

	// Write nametable
	if _, err := f.Seek(int64(nametableOffset), io.SeekStart); err != nil {
		return fmt.Errorf("seek to nametable: %w", err)
	}
	if _, err := f.Write(nametable); err != nil {
		return fmt.Errorf("write nametable: %w", err)
	}

	return nil
}

// RepackFromArchive creates a new archive from an existing one, optionally
// replacing specific files. replacements maps original file names to new data.
// Files not in the replacements map are copied from the original archive.
func RepackFromArchive(srcPath, dstPath string, replacements map[string][]byte) error {
	arc, err := Open(srcPath)
	if err != nil {
		return fmt.Errorf("open source: %w", err)
	}
	defer arc.Close()

	entries := make([]RepackEntry, len(arc.Entries))

	for i, fe := range arc.Entries {
		entries[i].Name = fe.Name
		entries[i].CRC = fe.CRC
		entries[i].Offset = fe.Descriptor.DataOffset
		entries[i].DecompressedSize = fe.Descriptor.DecompressedSize
		entries[i].Mode = fe.Descriptor.Mode

		if replacement, ok := replacements[fe.Name]; ok {
			// The descriptor's DecompressedSize must reflect the *actual*
			// decompressed length of the replacement content, not the
			// original file it's replacing. This field is load-bearing on
			// two separate paths that both walk it as a byte-count loop
			// bound rather than trusting the physical chunk stream:
			//   - Repack()'s own KeepZlib chunk-offset walk just below,
			//     which reconstructs this entry's chunk-properties-table
			//     rows by decrementing a `remaining` counter seeded from
			//     this value; a stale (too-small) value stops that walk
			//     before it reaches chunks that are physically present in
			//     the replacement data, silently dropping them from the
			//     properties table.
			//   - iga.go's extractCompressed/ExtractRawData, which use the
			//     descriptor's DecompressedSize the same way on read-back
			//     (by this tool or, presumably, the game's own loader).
			// Leaving it stale (the pre-existing bug this fixes) is
			// harmless only for edits that don't cross a 32KB chunk
			// boundary; it silently truncates/corrupts anything that does.
			entries[i].DecompressedSize = uint32(len(replacement))

			if arc.Header.MemoryPoolIndex == 1 {
				var fileBuf bytes.Buffer
				data := replacement
				remaining := len(data)
				offset := 0
				for remaining > 0 {
					chunkSize := ChunkDecompressedSize
					if remaining < chunkSize {
						chunkSize = remaining
					}
					rawChunk := data[offset : offset+chunkSize]

					var compBuf bytes.Buffer
					zw, err := zlib.NewWriterLevel(&compBuf, zlib.BestCompression)
					if err != nil {
						return fmt.Errorf("zlib replace compress error: %w", err)
					}
					if _, err := zw.Write(rawChunk); err != nil {
						return fmt.Errorf("zlib replace write error: %w", err)
					}
					zw.Close()

					compBytes := compBuf.Bytes()
					compSize := len(compBytes)

					sizeBuf := make([]byte, 2)
					binary.BigEndian.PutUint16(sizeBuf, uint16(compSize))
					fileBuf.Write(sizeBuf)
					fileBuf.Write(compBytes)

					padLen := alignUp32(uint32(fileBuf.Len()), ChunkAlignment) - uint32(fileBuf.Len())
					if padLen > 0 {
						fileBuf.Write(make([]byte, padLen))
					}

					offset += chunkSize
					remaining -= chunkSize
				}
				entries[i].Data = fileBuf.Bytes()
			} else {
				entries[i].Data = replacement
			}
		} else {
			// Extract raw compressed data from original
			var buf bytes.Buffer
			if err := arc.ExtractRawData(i, &buf); err != nil {
				return fmt.Errorf("extract raw file %d (%s): %w", i, fe.Name, err)
			}
			entries[i].Data = buf.Bytes()
		}
	}

	opts := RepackOptions{
		Version:          arc.Header.Version,
		MemoryPoolIndex:  arc.Header.MemoryPoolIndex,
		OriginalReserved: arc.Header.Reserved,
		KeepZlib:         true,
	}

	return Repack(dstPath, entries, opts)
}

// buildNametable constructs the nametable bytes:
// [N × uint32 offsets] [null-terminated strings]
func buildNametable(entries []RepackEntry) []byte {
	n := len(entries)

	// Calculate string section offset (after the N offset entries)
	stringBase := uint32(n * 4)

	// Build string data
	var strData []byte
	offsets := make([]uint32, n)

	for i, entry := range entries {
		offsets[i] = stringBase + uint32(len(strData))
		strData = append(strData, []byte(entry.Name)...)
		strData = append(strData, 0) // null terminator
	}

	// Build complete nametable
	result := make([]byte, int(stringBase)+len(strData))
	for i, off := range offsets {
		binary.LittleEndian.PutUint32(result[i*4:], off)
	}
	copy(result[stringBase:], strData)

	return result
}

// alignUp32 rounds val up to the next multiple of align.
func alignUp32(val, align uint32) uint32 {
	if val%align == 0 {
		return val
	}
	return ((val + align - 1) / align) * align
}
