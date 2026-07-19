// mad2save is a command-line tool for reading, editing, checksumming,
// and converting Madagascar: Escape 2 Africa save files between PC and Wii.
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"strconv"
)

const (
	SaveFileSize = 13568
	// PC has 8 extra zero bytes at 0x120 that Wii does not.
	// All data after this point is shifted by +8 on PC relative to Wii.
	PCGapOffset = 0x120
	PCGapSize   = 8
)

func main() {
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "mad2save — Madagascar 2 Save Game Editor & Checksummer\n\n")
		fmt.Fprintf(os.Stderr, "Usage:\n")
		fmt.Fprintf(os.Stderr, "  mad2save info <save_file>\n")
		fmt.Fprintf(os.Stderr, "  mad2save progress <save_file> <percent>\n")
		fmt.Fprintf(os.Stderr, "  mad2save set-byte <save_file> <offset> <val>\n")
		fmt.Fprintf(os.Stderr, "  mad2save set-uint32 <save_file> <offset> <val>\n")
		fmt.Fprintf(os.Stderr, "  mad2save pc2wii <pc_save> <wii_output>\n")
		fmt.Fprintf(os.Stderr, "  mad2save wii2pc <wii_save> <pc_output>\n")
	}
	flag.Parse()

	args := flag.Args()
	if len(args) < 2 {
		flag.Usage()
		os.Exit(1)
	}

	cmd := args[0]
	savePath := args[1]

	// Read save file
	data, err := os.ReadFile(savePath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: read save file: %v\n", err)
		os.Exit(1)
	}

	if len(data) < 16 {
		fmt.Fprintf(os.Stderr, "Error: save file too small (must be at least 16 bytes)\n")
		os.Exit(1)
	}

	switch cmd {
	case "info":
		printInfo(savePath, data)

	case "progress":
		if len(args) < 3 {
			fmt.Fprintf(os.Stderr, "Usage: mad2save progress <save_file> <percent_float_or_int>\n")
			os.Exit(1)
		}
		percentVal, err := strconv.ParseFloat(args[2], 64)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: invalid percentage value: %v\n", err)
			os.Exit(1)
		}

		progressInt := uint32(percentVal * 100.0)
		binary.LittleEndian.PutUint32(data[12:16], progressInt)
		fmt.Printf("Updated progress to %.2f%% (value = %d) at offset 0x0C\n", percentVal, progressInt)
		saveFilePC(savePath, data)

	case "set-byte":
		if len(args) < 4 {
			fmt.Fprintf(os.Stderr, "Usage: mad2save set-byte <save_file> <offset> <value>\n")
			os.Exit(1)
		}
		offset, err := parseOffset(args[2], len(data))
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		val, err := strconv.ParseUint(args[3], 0, 8)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: invalid byte value (must be 0-255): %v\n", err)
			os.Exit(1)
		}

		data[offset] = byte(val)
		fmt.Printf("Updated byte at offset 0x%X (%d) to 0x%02X\n", offset, offset, val)
		saveFilePC(savePath, data)

	case "set-uint32":
		if len(args) < 4 {
			fmt.Fprintf(os.Stderr, "Usage: mad2save set-uint32 <save_file> <offset> <value>\n")
			os.Exit(1)
		}
		offset, err := parseOffset(args[2], len(data)-3)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		val, err := strconv.ParseUint(args[3], 0, 32)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: invalid uint32 value: %v\n", err)
			os.Exit(1)
		}

		binary.LittleEndian.PutUint32(data[offset:offset+4], uint32(val))
		fmt.Printf("Updated uint32 at offset 0x%X (%d) to 0x%08X (%d)\n", offset, offset, val, val)
		saveFilePC(savePath, data)

	case "pc2wii":
		if len(args) < 3 {
			fmt.Fprintf(os.Stderr, "Usage: mad2save pc2wii <pc_save> <wii_output>\n")
			os.Exit(1)
		}
		outPath := args[2]
		convertPCToWii(data, outPath)

	case "wii2pc":
		if len(args) < 3 {
			fmt.Fprintf(os.Stderr, "Usage: mad2save wii2pc <wii_save> <pc_output>\n")
			os.Exit(1)
		}
		outPath := args[2]
		convertWiiToPC(data, outPath)

	default:
		flag.Usage()
		os.Exit(1)
	}
}

func printInfo(savePath string, data []byte) {
	// Auto-detect platform by checking if the version field (0x10) reads
	// correctly as LE or BE. Value should be 17 (0x11).
	verLE := binary.LittleEndian.Uint32(data[0x10:0x14])
	verBE := binary.BigEndian.Uint32(data[0x10:0x14])

	isWii := false
	if verBE == 17 && verLE != 17 {
		isWii = true
	}

	var progressVal uint32
	var expectedCRC, calculatedCRC uint32
	platform := "PC (Little-Endian)"

	if isWii {
		platform = "Wii (Big-Endian)"
		progressVal = binary.BigEndian.Uint32(data[12:16])
		expectedCRC = binary.BigEndian.Uint32(data[len(data)-4:])
		calculatedCRC = crc32.ChecksumIEEE(data[:len(data)-4]) ^ 0xFFFFFFFF
	} else {
		progressVal = binary.LittleEndian.Uint32(data[12:16])
		expectedCRC = binary.LittleEndian.Uint32(data[len(data)-4:])
		calculatedCRC = crc32.ChecksumIEEE(data[:len(data)-4]) ^ 0xFFFFFFFF
	}

	percent := float64(progressVal) / 100.0

	fmt.Printf("Save Game Info: %s\n", filepath.Base(savePath))
	fmt.Printf("  Platform:       %s\n", platform)
	fmt.Printf("  File Size:      %d bytes\n", len(data))
	fmt.Printf("  Progress:       %.2f%% (Value: %d at offset 0x0C)\n", percent, progressVal)
	fmt.Printf("  Saved Checksum: 0x%08X\n", expectedCRC)
	fmt.Printf("  Calc Checksum:  0x%08X\n", calculatedCRC)
	if expectedCRC == calculatedCRC {
		fmt.Println("  Checksum Status: ✓ VALID")
	} else {
		fmt.Println("  Checksum Status: ✗ INVALID (Corrupt save or modified without checksummer)")
	}
}

// convertPCToWii converts a PC (little-endian) save file to Wii (big-endian).
//
// Conversion steps:
//  1. Copy all bytes and byte-swap all uint32s from LE to BE.
//  2. Shift the main cheats array from PC [0x128-0x167] to Wii [0x120-0x15F].
//  3. Shift the golf cheats: PC [0x28C] -> Wii [0x284], PC [0x2A0] -> Wii [0x298].
//  4. Recalculate CRC32 checksum and store as big-endian in footer.
func convertPCToWii(pcData []byte, outPath string) {
	if len(pcData) != SaveFileSize {
		fmt.Fprintf(os.Stderr, "Warning: PC save file is %d bytes, expected %d\n", len(pcData), SaveFileSize)
	}

	// Step 1: Copy and byte-swap all uint32s
	wiiData := make([]byte, len(pcData))
	copy(wiiData, pcData)
	swapEndianUint32(wiiData)

	// Step 2: Shift main cheats block (0x128-0x167 on PC to 0x120-0x15F on Wii)
	// Cheats are 16 uint32s = 64 bytes
	copy(wiiData[0x120:0x160], wiiData[0x128:0x168])
	// Zero out the now unused PC-only space in the Wii save at 0x160-0x167
	binary.BigEndian.PutUint32(wiiData[0x160:0x164], 0)
	binary.BigEndian.PutUint32(wiiData[0x164:0x168], 0)

	// Step 3: Shift golf cheats
	// PC 0x28C -> Wii 0x284
	copy(wiiData[0x284:0x288], wiiData[0x28C:0x290])
	binary.BigEndian.PutUint32(wiiData[0x28C:0x290], 0)
	// PC 0x2A0 -> Wii 0x298
	copy(wiiData[0x298:0x29C], wiiData[0x2A0:0x2A4])
	binary.BigEndian.PutUint32(wiiData[0x2A0:0x2A4], 0)

	// Step 4: Recalculate CRC32 and store as big-endian
	newCRC := crc32.ChecksumIEEE(wiiData[:len(wiiData)-4]) ^ 0xFFFFFFFF
	binary.BigEndian.PutUint32(wiiData[len(wiiData)-4:], newCRC)

	// Write output
	if err := os.WriteFile(outPath, wiiData, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "Error: failed to write Wii save: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Converted PC → Wii save file (1-to-1 layout)\n")
	fmt.Printf("  Output: %s\n", outPath)
	fmt.Printf("  Checksum: 0x%08X\n", newCRC)
	fmt.Println("  Done!")
}

// convertWiiToPC converts a Wii (big-endian) save file to PC (little-endian).
//
// Conversion steps:
//  1. Copy all bytes and byte-swap all uint32s from BE to LE.
//  2. Shift the main cheats array from Wii [0x120-0x15F] to PC [0x128-0x167].
//  3. Shift the golf cheats: Wii [0x284] -> PC [0x28C], Wii [0x298] -> PC [0x2A0].
//  4. Recalculate CRC32 checksum and store as little-endian in footer.
func convertWiiToPC(wiiData []byte, outPath string) {
	if len(wiiData) != SaveFileSize {
		fmt.Fprintf(os.Stderr, "Warning: Wii save file is %d bytes, expected %d\n", len(wiiData), SaveFileSize)
	}

	// Step 1: Copy and byte-swap all uint32s
	pcData := make([]byte, len(wiiData))
	copy(pcData, wiiData)
	swapEndianUint32(pcData)

	// Step 2: Shift main cheats block (0x120-0x15F on Wii to 0x128-0x167 on PC)
	copy(pcData[0x128:0x168], pcData[0x120:0x160])
	// Zero out the now unused Wii-only space in the PC save at 0x120-0x127
	binary.LittleEndian.PutUint32(pcData[0x120:0x124], 0)
	binary.LittleEndian.PutUint32(pcData[0x124:0x128], 0)

	// Step 3: Shift golf cheats
	// Wii 0x284 -> PC 0x28C
	copy(pcData[0x28C:0x290], pcData[0x284:0x288])
	binary.LittleEndian.PutUint32(pcData[0x284:0x288], 0)
	// Wii 0x298 -> PC 0x2A0
	copy(pcData[0x2A0:0x2A4], pcData[0x298:0x29C])
	binary.LittleEndian.PutUint32(pcData[0x298:0x29C], 0)

	// Step 4: Recalculate CRC32 and store as little-endian
	newCRC := crc32.ChecksumIEEE(pcData[:len(pcData)-4]) ^ 0xFFFFFFFF
	binary.LittleEndian.PutUint32(pcData[len(pcData)-4:], newCRC)

	// Write output
	if err := os.WriteFile(outPath, pcData, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "Error: failed to write PC save: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Converted Wii → PC save file (1-to-1 layout)\n")
	fmt.Printf("  Output: %s\n", outPath)
	fmt.Printf("  Checksum: 0x%08X\n", newCRC)
	fmt.Println("  Done!")
}

// swapEndianUint32 reverses the byte order of every 4-byte word in-place.
// This converts LE↔BE for all uint32 fields simultaneously.
func swapEndianUint32(data []byte) {
	for i := 0; i+3 < len(data); i += 4 {
		data[i], data[i+3] = data[i+3], data[i]
		data[i+1], data[i+2] = data[i+2], data[i+1]
	}
}

func parseOffset(s string, limit int) (int, error) {
	// Supports hex (0x...) or decimal
	offset, err := strconv.ParseInt(s, 0, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid offset: %w", err)
	}
	if offset < 0 || int(offset) >= limit {
		return 0, fmt.Errorf("offset out of bounds [0, %d)", limit)
	}
	return int(offset), nil
}

func saveFilePC(savePath string, data []byte) {
	// Recalculate checksum at the end (PC: little-endian)
	calcCRC := crc32.ChecksumIEEE(data[:len(data)-4]) ^ 0xFFFFFFFF
	binary.LittleEndian.PutUint32(data[len(data)-4:], calcCRC)
	fmt.Printf("Recalculated Checksum: 0x%08X\n", calcCRC)

	// Write back to disk
	if err := os.WriteFile(savePath, data, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "Error: failed to write save file: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("Successfully saved file changes.")
}
