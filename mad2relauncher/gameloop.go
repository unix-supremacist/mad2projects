package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
	"time"
)

// resolveUmuRun returns the umu-run command to exec -- prefers a copy
// bundled right next to this binary (see tools/package.sh's stage_platform
// call for mad2relauncher_linux, which stages one there via `just
// fetch-umu-run`'s build/umu-run output) over requiring the user to
// separately install the umu-launcher package system-wide, falling back to
// plain "umu-run" resolved from PATH if no bundled copy is present. This is
// a plain Python zipapp (see fetch-umu-run's own comment in justfile) --
// still needs a system python3, but that's a far lower bar than a full
// umu-launcher package install.
func resolveUmuRun() string {
	exe, err := os.Executable()
	if err != nil {
		return "umu-run"
	}
	if resolved, err := filepath.EvalSymlinks(exe); err == nil {
		exe = resolved
	}
	beside := filepath.Join(filepath.Dir(exe), "umu-run")
	if st, err := os.Stat(beside); err == nil && !st.IsDir() {
		return beside
	}
	return "umu-run"
}

// GameLoop runs `umu-run <exe>` and relaunches it on every exit,
// unconditionally, until Stop is called -- the user's explicit choice: any
// exit (crash or otherwise) triggers a relaunch, and the only way out is a
// deliberate stop (Ctrl+Q, or gamescope tearing the whole process tree
// down on window close, which takes this process with it).
type GameLoop struct {
	exe        string
	umuRunPath string

	mu      sync.Mutex
	cmd     *exec.Cmd
	stopped bool
}

func NewGameLoop(exe string) *GameLoop {
	return &GameLoop{exe: exe, umuRunPath: resolveUmuRun()}
}

func (g *GameLoop) Run() {
	for {
		g.mu.Lock()
		if g.stopped {
			g.mu.Unlock()
			return
		}
		g.mu.Unlock()

		logf("launching %s %s", g.umuRunPath, g.exe)
		cmd := exec.Command(g.umuRunPath, g.exe)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		cmd.Env = os.Environ()
		// New process group so Stop() can kill umu-run's whole subtree
		// (umu-run itself forks Proton/Wine processes) in one signal
		// instead of just the immediate child.
		// Pdeathsig: same defense-in-depth reasoning as music.go's copy --
		// guarantees the kernel kills umu-run's whole subtree if
		// mad2relauncher itself is ever killed outright, independent of
		// whether its own SIGTERM handler gets a chance to run Stop().
		cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGTERM}

		if err := cmd.Start(); err != nil {
			logf("failed to start umu-run: %v -- retrying in 2s", err)
			time.Sleep(2 * time.Second)
			continue
		}

		g.mu.Lock()
		g.cmd = cmd
		g.mu.Unlock()

		err := cmd.Wait()
		logf("umu-run %s exited: %v", g.exe, err)

		g.mu.Lock()
		g.cmd = nil
		stopped := g.stopped
		g.mu.Unlock()

		if stopped {
			return
		}
		// Brief pause so a fundamentally broken setup (bad prefix, missing
		// Proton, etc.) crash-loops slowly instead of hammering the log/CPU.
		time.Sleep(1 * time.Second)
	}
}

// Stop requests a clean shutdown: no further relaunches, and kills
// whatever umu-run/game process tree is currently running.
func (g *GameLoop) Stop() {
	g.mu.Lock()
	g.stopped = true
	cmd := g.cmd
	g.mu.Unlock()

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
