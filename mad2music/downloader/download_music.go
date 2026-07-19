package main

import (
	"bufio"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

var categories = []string{
	"copyright",
	"copyright_lyrics",
	"no_copyright",
	"no_copyright_lyrics",
	"custom",
	"custom_lyrics",
}

func ensureYtdlp() (string, error) {
	// 1. Check if yt-dlp is in system PATH
	path, err := exec.LookPath("yt-dlp")
	if err == nil {
		fmt.Printf("[Init] Found system yt-dlp at: %s\n", path)
		return path, nil
	}

	pwd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	portablePath := filepath.Join(pwd, "runtime", "yt-dlp")

	// 2. If portable already exists in runtime folder, use it
	if _, err := os.Stat(portablePath); err == nil {
		fmt.Printf("[Init] Found portable yt-dlp at: %s\n", portablePath)
		return portablePath, nil
	}

	// 3. Download portable yt-dlp to runtime/yt-dlp
	err = os.MkdirAll(filepath.Join(pwd, "runtime"), 0755)
	if err == nil {
		url := "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp"
		fmt.Printf("[Init] Downloading latest portable yt-dlp from %s...\n", url)

		out, err := os.OpenFile(portablePath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0755)
		if err == nil {
			resp, err := http.Get(url)
			if err == nil {
				defer resp.Body.Close()
				if resp.StatusCode == http.StatusOK {
					_, err = io.Copy(out, resp.Body)
					out.Close()
					if err == nil {
						os.Chmod(portablePath, 0755)
						fmt.Println("[Init] yt-dlp downloaded and marked as executable successfully!")
						return portablePath, nil
					}
				} else {
					out.Close()
				}
			} else {
				out.Close()
			}
		}
	}

	return "", fmt.Errorf("could not download or find system yt-dlp")
}

func logMessage(msg string) {
	fmt.Println(msg)
	_ = os.MkdirAll("logs", 0755)
	f, err := os.OpenFile("logs/music_download.log", os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err == nil {
		defer f.Close()
		fmt.Fprintln(f, msg)
	}
}

// audioExts are the file extensions updateSongManifest treats as downloaded
// tracks when scanning a category directory -- everything yt-dlp's
// --audio-format vorbis run or a direct-audio URL download can produce.
var audioExts = map[string]bool{
	".ogg":  true,
	".mp3":  true,
	".wav":  true,
	".flac": true,
	".m4a":  true,
	".opus": true,
}

const songManifestName = "song_names.txt"

// updateSongManifest assigns every audio file in catDir a stable,
// non-spoiling display name -- "song01", "song02", ... -- separate from its
// actual (often messy, verbatim-YouTube-title) filename, in the spirit of
// OpenBSD's anonymized songXX.ogg boot-sound naming. The mapping is
// persisted to catDir/song_names.txt (plain "filename=songNN" lines, same
// key=value convention as mad2config's config.cfg) so it survives repeated
// runs: existing files keep the display name they were already assigned
// (never renumbered), and only newly-seen files get the next free number.
// Deliberately does not touch the files themselves -- only mad2config-style
// metadata -- so this can never interfere with download_archive.txt's own
// video-id-keyed dedup (see the redownload-avoidance note in main()).
func updateSongManifest(catDir string) error {
	manifestPath := filepath.Join(catDir, songManifestName)

	existing := map[string]string{} // actual filename -> "songNN"
	maxNum := 0
	if f, err := os.Open(manifestPath); err == nil {
		scanner := bufio.NewScanner(f)
		for scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			parts := strings.SplitN(line, "=", 2)
			if len(parts) != 2 {
				continue
			}
			fname, display := parts[0], parts[1]
			existing[fname] = display
			if n, err := strconv.Atoi(strings.TrimPrefix(display, "song")); err == nil && n > maxNum {
				maxNum = n
			}
		}
		f.Close()
	}

	entries, err := os.ReadDir(catDir)
	if err != nil {
		return err
	}
	var files []string
	present := map[string]bool{}
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		if audioExts[strings.ToLower(filepath.Ext(e.Name()))] {
			files = append(files, e.Name())
			present[e.Name()] = true
		}
	}
	sort.Strings(files)

	changed := false
	for _, fname := range files {
		if _, ok := existing[fname]; ok {
			continue
		}
		maxNum++
		existing[fname] = fmt.Sprintf("song%02d", maxNum)
		changed = true
	}
	// Drop entries for files that no longer exist (deleted/moved) rather
	// than leaving stale mappings around -- doesn't renumber survivors.
	for fname := range existing {
		if !present[fname] {
			delete(existing, fname)
			changed = true
		}
	}
	if !changed {
		return nil
	}

	names := make([]string, 0, len(existing))
	for fname := range existing {
		names = append(names, fname)
	}
	sort.Slice(names, func(i, j int) bool { return existing[names[i]] < existing[names[j]] })

	f, err := os.Create(manifestPath)
	if err != nil {
		return err
	}
	defer f.Close()
	fmt.Fprintln(f, "# Stable display name for each downloaded track in this category, separate")
	fmt.Fprintln(f, "# from its real (often messy) filename -- OpenBSD-style songXX naming. Managed")
	fmt.Fprintln(f, "# by mad2music/downloader/download_music.go; do not edit the numbers by hand,")
	fmt.Fprintln(f, "# they're assigned in first-seen order and never reused.")
	for _, fname := range names {
		fmt.Fprintf(f, "%s=%s\n", fname, existing[fname])
	}
	return nil
}

func main() {
	ytdlpPath, err := ensureYtdlp()
	if err != nil {
		logMessage(fmt.Sprintf("[Error] Failed to resolve yt-dlp: %v", err))
		os.Exit(1)
	}

	// Relative to cwd -- this tool is meant to be run with cwd set to
	// mad2music/ itself (mad2launcher's RunMusicDownloader does this via
	// cmd.Dir), so this resolves to mad2music/audio/music, matching both
	// music_pool.cpp's own <audioDir>/music/{category} scan and the
	// category link lists (audio/music/*.txt) already checked into the
	// repo. This used to be "game/audio/music", a stale path from before
	// this tool was migrated in-repo that never matched the real layout --
	// running it would have silently started a brand new tree instead of
	// finding any of the already-downloaded tracks or the existing
	// download_archive.txt, i.e. it would have redownloaded everything.
	baseMusicDir := filepath.Join("audio", "music")
	err = os.MkdirAll(baseMusicDir, 0755)
	if err != nil {
		logMessage(fmt.Sprintf("[Error] Failed to create music directory: %v", err))
		os.Exit(1)
	}

	// Check for add-playlist command line flag
	// Usage: ./download_music add-playlist <category> <playlist-url>
	if len(os.Args) >= 4 && os.Args[1] == "add-playlist" {
		targetCat := strings.ToLower(os.Args[2])
		playlistURL := os.Args[3]

		// Validate category
		validCat := false
		for _, cat := range categories {
			if cat == targetCat {
				validCat = true
				break
			}
		}
		if !validCat {
			fmt.Printf("Invalid category '%s'. Available categories: %v\n", targetCat, categories)
			os.Exit(1)
		}

		listFile := filepath.Join(baseMusicDir, targetCat+".txt")
		fmt.Printf("[Playlist] Fetching video URLs from playlist: %s\n", playlistURL)

		// Run yt-dlp to extract flat URLs from playlist
		cmd := exec.Command(ytdlpPath, "--flat-playlist", "--print", "webpage_url", playlistURL)
		outputBytes, errRun := cmd.Output()
		if errRun != nil {
			fmt.Printf("[Error] yt-dlp failed to fetch playlist: %v\n", errRun)
			os.Exit(1)
		}

		urls := strings.Split(string(outputBytes), "\n")
		var addedCount int
		f, errOpen := os.OpenFile(listFile, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0644)
		if errOpen != nil {
			fmt.Printf("[Error] Failed to open list file %s: %v\n", listFile, errOpen)
			os.Exit(1)
		}
		defer f.Close()

		for _, url := range urls {
			url = strings.TrimSpace(url)
			if url == "" {
				continue
			}
			// Write the URL to the file
			_, _ = fmt.Fprintln(f, url)
			addedCount++
		}

		fmt.Printf("[Playlist] Successfully added %d videos to %s\n", addedCount, listFile)
		os.Exit(0)
	}

	for _, cat := range categories {
		catDir := filepath.Join(baseMusicDir, cat)
		err = os.MkdirAll(catDir, 0755)
		if err != nil {
			logMessage(fmt.Sprintf("[Error] Failed to create category directory %s: %v", cat, err))
			continue
		}

		listFile := filepath.Join(baseMusicDir, cat+".txt")
		if _, err := os.Stat(listFile); os.IsNotExist(err) {
			// Write template
			f, err := os.Create(listFile)
			if err != nil {
				logMessage(fmt.Sprintf("[Error] Failed to create template list file %s: %v", listFile, err))
				continue
			}
			fmt.Fprintf(f, "# Add YouTube URLs for the '%s' category here, one per line.\n", cat)
			fmt.Fprintln(f, "# Lines starting with '#' are ignored.")
			f.Close()
			logMessage(fmt.Sprintf("[Init] Created template link list: %s", listFile))
		}

		// Read links from the list file
		file, err := os.Open(listFile)
		if err != nil {
			logMessage(fmt.Sprintf("[Error] Failed to open list file %s: %v", listFile, err))
			continue
		}

		var links []string
		scanner := bufio.NewScanner(file)
		for scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if line != "" && !strings.HasPrefix(line, "#") {
				links = append(links, line)
			}
		}
		file.Close()

		if len(links) == 0 {
			logMessage(fmt.Sprintf("[Init] No links found in: %s", listFile))
			continue
		}

		logMessage(fmt.Sprintf("[Init] Syncing %d tracks for category '%s'...", len(links), cat))
		for _, link := range links {
			lowerLink := strings.ToLower(link)
			isDirectAudio := false
			if strings.HasSuffix(lowerLink, ".ogg") || strings.HasSuffix(lowerLink, ".mp3") || strings.HasSuffix(lowerLink, ".wav") || strings.HasSuffix(lowerLink, ".flac") {
				isDirectAudio = true
			}

			if isDirectAudio {
				filename := filepath.Base(link)
				targetPath := filepath.Join(catDir, filename)

				// Avoid redownloading
				if _, err := os.Stat(targetPath); err == nil {
					logMessage(fmt.Sprintf("[Init] File already exists (skipping): %s", targetPath))
					continue
				}

				logMessage(fmt.Sprintf("[Init] Downloading direct audio: %s", link))
				resp, err := http.Get(link)
				if err != nil {
					logMessage(fmt.Sprintf("[Error] Failed to download direct audio %s: %v", link, err))
					continue
				}

				if resp.StatusCode != http.StatusOK {
					resp.Body.Close()
					logMessage(fmt.Sprintf("[Error] Failed to download direct audio %s: HTTP Status %s", link, resp.Status))
					continue
				}

				out, err := os.Create(targetPath)
				if err != nil {
					resp.Body.Close()
					logMessage(fmt.Sprintf("[Error] Failed to create file %s: %v", targetPath, err))
					continue
				}

				_, err = io.Copy(out, resp.Body)
				out.Close()
				resp.Body.Close()

				if err != nil {
					logMessage(fmt.Sprintf("[Error] Failed to save file %s: %v", targetPath, err))
				} else {
					logMessage(fmt.Sprintf("[Init] Downloaded direct audio successfully: %s", filename))
				}
			} else {
				logMessage(fmt.Sprintf("[Init] Syncing YouTube video: %s", link))
				outputPath := filepath.Join(catDir, "%(title)s.%(ext)s")

				// Use a local download archive in the music directory to skip already downloaded tracks.
				// baseMusicDir (and therefore this path) is stable across runs as long as this tool is
				// always invoked with cwd == mad2music/ -- see RunMusicDownloader in mad2launcher/music.go.
				archiveFile := filepath.Join(baseMusicDir, "download_archive.txt")
				cmd := exec.Command(ytdlpPath,
					"--download-archive", archiveFile,
					"--no-cache-dir",
					"-x",
					"--audio-format", "vorbis",
					"--audio-quality", "5",
					"-o", outputPath,
					link,
				)

				// Redirect yt-dlp logs to our log file so errors are captured
				var ytLog *os.File
				_ = os.MkdirAll("logs", 0755)
				if logF, err := os.OpenFile("logs/music_download.log", os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644); err == nil {
					ytLog = logF
					cmd.Stdout = ytLog
					cmd.Stderr = ytLog
				} else {
					cmd.Stdout = os.Stdout
					cmd.Stderr = os.Stderr
				}

				startTime := time.Now()
				errRun := cmd.Run()

				if ytLog != nil {
					ytLog.Close()
				}

				if errRun != nil {
					logMessage(fmt.Sprintf("[Error] yt-dlp failed to download %s: %v", link, errRun))
				} else {
					logMessage(fmt.Sprintf("[Init] Successfully processed: %s", link))
				}

				// Sleep to avoid rate limiting. If the process took a while (actually downloaded),
				// sleep longer. If it was near-instant (skipped via archive), sleep briefly.
				elapsed := time.Since(startTime)
				if elapsed < 2*time.Second {
					time.Sleep(500 * time.Millisecond)
				} else {
					time.Sleep(3 * time.Second)
				}
			}
		}

		if err := updateSongManifest(catDir); err != nil {
			logMessage(fmt.Sprintf("[Error] Failed to update %s for category '%s': %v", songManifestName, cat, err))
		}
	}
	logMessage("[Init] Music synchronization complete!")
}
