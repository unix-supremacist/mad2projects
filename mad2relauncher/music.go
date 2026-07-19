package main

import (
	"os"
	"os/exec"
	"sync"
	"syscall"
	"time"
)

// MusicLoop runs the native mad2music binary and relaunches it on every
// exit -- same crash-relaunch shape as GameLoop (see gameloop.go), but
// execing mad2music directly instead of wrapping it in `umu-run`, since
// it's already a native Linux binary. Unlike GameLoop it starts alongside
// the game and keeps running for the whole session regardless of what the
// game process is doing -- background music isn't effect-gated, only the
// podcast interruption mad2podcastmod.dll triggers over loopback UDP is
// (see ../mad2podcastmod/include/mad2podcast_protocol.h and
// ../mad2music/src/main.cpp for the other two-thirds of this picture).
type MusicLoop struct {
	musicPath string
	audioDir  string

	mu      sync.Mutex
	cmd     *exec.Cmd
	stopped bool
}

func NewMusicLoop(musicPath, audioDir string) *MusicLoop {
	return &MusicLoop{musicPath: musicPath, audioDir: audioDir}
}

func (m *MusicLoop) Run() {
	for {
		m.mu.Lock()
		if m.stopped {
			m.mu.Unlock()
			return
		}
		m.mu.Unlock()

		logf("launching mad2music (audioDir=%s)", m.audioDir)
		cmd := exec.Command(m.musicPath, "-audio-dir", m.audioDir)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		cmd.Env = os.Environ()
		// New process group so Stop() can kill mad2music (and anything it
		// forks, e.g. its ffmpeg decode subprocesses) in one signal instead
		// of just the immediate child -- same reasoning as GameLoop's
		// Setpgid use for umu-run's Proton/Wine subtree.
		// Pdeathsig is defense-in-depth on top of Stop()'s own explicit
		// signaling: observed live, mad2music surviving for minutes after
		// gamescope's nested display died because mad2relauncher's own
		// SIGTERM handler never got a chance to run (see x11.go's
		// stop-on-connection-close fix for the primary cause) -- but even if
		// mad2relauncher itself is ever killed outright (SIGKILL, no
		// userspace cleanup possible), the kernel now kills mad2music the
		// instant that happens, unconditionally.
		cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGTERM}

		if err := cmd.Start(); err != nil {
			logf("failed to start mad2music: %v -- retrying in 2s", err)
			time.Sleep(2 * time.Second)
			continue
		}

		m.mu.Lock()
		m.cmd = cmd
		m.mu.Unlock()

		err := cmd.Wait()
		logf("mad2music exited: %v", err)

		m.mu.Lock()
		m.cmd = nil
		stopped := m.stopped
		m.mu.Unlock()

		if stopped {
			return
		}
		// Brief pause so a fundamentally broken setup (missing SDL3/ffmpeg,
		// bad -audio-dir) crash-loops slowly instead of hammering the
		// log/CPU -- same reasoning as GameLoop's own pause.
		time.Sleep(1 * time.Second)
	}
}

// Stop requests a clean shutdown: no further relaunches, and kills whatever
// mad2music process is currently running.
func (m *MusicLoop) Stop() {
	m.mu.Lock()
	m.stopped = true
	cmd := m.cmd
	m.mu.Unlock()

	if cmd == nil || cmd.Process == nil {
		return
	}
	pgid, err := syscall.Getpgid(cmd.Process.Pid)
	if err != nil {
		cmd.Process.Kill()
		return
	}
	syscall.Kill(-pgid, syscall.SIGTERM)
	time.Sleep(500 * time.Millisecond)
	syscall.Kill(-pgid, syscall.SIGKILL)
}
