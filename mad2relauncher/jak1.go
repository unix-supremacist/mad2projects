package main

import (
	"encoding/binary"
	"fmt"
	"math/rand"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
)

// gk has no --width/--height/--fullscreen CLI flags (unlike sm64ex-alo's),
// so it opens at a hardcoded 640x480 (game/graphics/gfx.cpp's
// Display::InitMainDisplay call) -- normally resized by GOAL menu code
// once the player picks a resolution, which we never reach since we boot
// straight into a level. FocusWindowAndHideMad2 force-resizes to this via
// plain X11 ConfigureWindow once gk's window appears -- matches the
// justfile's `gamescope -w 1280 -h 720` output size (see _play).
var jak1WindowSize = [2]uint32{1280, 720}

// The 16 continue-point names defined in
// extern/jak-project/goal_src/jak1/engine/level/level-info.gc, one per
// level -- reused by hand rather than parsed from that file at build time,
// same can't-share-across-repos reasoning as sm64.go's sm64LevelPool
// comment (this is Go, that's GOAL script).
var jak1Checkpoints = []string{
	"training-start", "village1-hut", "beach-start", "jungle-start",
	"misty-start", "firecanyon-start", "village2-start", "sunken-start",
	"swamp-start", "rolling-start", "ogre-start", "village3-start",
	"snow-start", "maincave-start", "lavatube-start", "citadel-start",
}

// Wire protocol shared with mad2sm64mod/include/mad2jak1_protocol.h -- hand
// duplicated the same way sm64.go's control-packet constants are (one side
// is C++, this is Go; see that file's own comment). Unlike sm64.go, the
// input packet is ALSO duplicated here, because this process (not gk) is
// the actual UDP receiving endpoint for it -- see jak1InputListener below
// and mad2jak1_protocol.h's file header for why.
const (
	jak1ControlMagic  uint32 = 0x4A324D32
	jak1ControlLaunch uint32 = 1
	jak1ControlStop   uint32 = 2
	jak1ControlExited uint32 = 3

	jak1InputMagic uint32 = 0x4A314D32
	// magic(4) + seq(4) + button0(2) + leftx(1) + lefty(1) + rightx(1) + righty(1)
	jak1InputPacketSize = 14
)

// Jak1Controller owns:
//  1. the loopback UDP control listener mad2sm64mod's JakChallenge effect
//     (LAUNCH/STOP) talks to, and the gk child process lifecycle -- same
//     shape as sm64.go's Sm64Controller.
//  2. the loopback UDP INPUT listener -- unlike SM64 (source-vendored in
//     extern/sm64ex-alo, patched with controller_udp.c to receive UDP
//     directly), gk has no socket primitive of its own. So this process is
//     the actual receiving endpoint, and bridges the latest state into gk
//     by atomically overwriting a small file inside gk's own working
//     directory -- polled once per frame by a hand-written patch in
//     extern/jak-project/goal_src/jak1/engine/ps2/pad.gc's service-cpads.
//     See mad2jak1_protocol.h's file header for the full picture.
type Jak1Controller struct {
	jak1Dir   string // extern/jak-project -- repo root; gk itself lives at <jak1Dir>/build/game/gk
	inputPort int

	controlConn *net.UDPConn
	inputConn   *net.UDPConn
	inputPath   string // <jak1Dir>/mad2_input.dat -- what the GOAL patch polls
	x11         *X11   // nil if X11 connection failed -- launching still works, just no window focus juggling

	mu        sync.Mutex
	cmd       *exec.Cmd
	replyAddr *net.UDPAddr
}

func NewJak1Controller(jak1Dir string, inputPort, controlPort int, x11 *X11) (*Jak1Controller, error) {
	controlAddr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: controlPort}
	controlConn, err := net.ListenUDP("udp", controlAddr)
	if err != nil {
		return nil, fmt.Errorf("listen on control port %d: %w", controlPort, err)
	}

	inputAddr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: inputPort}
	inputConn, err := net.ListenUDP("udp", inputAddr)
	if err != nil {
		controlConn.Close()
		return nil, fmt.Errorf("listen on input port %d: %w", inputPort, err)
	}

	j := &Jak1Controller{
		jak1Dir:     jak1Dir,
		inputPort:   inputPort,
		controlConn: controlConn,
		inputConn:   inputConn,
		inputPath:   filepath.Join(jak1Dir, "mad2_input.dat"),
		x11:         x11,
	}
	// Defensive: an unclean previous shutdown could have left a stale
	// injection file behind. Removing it here means a manually-launched gk
	// (e.g. via `task boot-game`, which also passes -level and thus also
	// sets *kernel-boot-level*) never picks up old chaos-mod input by
	// accident -- the GOAL patch's file-stream-open simply fails (file
	// doesn't exist) and falls back cleanly to real hardware input, same
	// "must degrade cleanly if injection stops arriving" contract
	// mad2sm64_protocol.h documents for the SM64 side.
	os.Remove(j.inputPath)
	return j, nil
}

// Serve pumps both UDP sockets forever. Call each with `go`.
func (j *Jak1Controller) ServeControl() {
	buf := make([]byte, 64)
	for {
		n, addr, err := j.controlConn.ReadFromUDP(buf)
		if err != nil {
			logf("jak1 control socket read error: %v", err)
			return
		}
		if n != 8 {
			continue
		}
		magic := binary.LittleEndian.Uint32(buf[0:4])
		if magic != jak1ControlMagic {
			continue
		}
		switch binary.LittleEndian.Uint32(buf[4:8]) {
		case jak1ControlLaunch:
			j.handleLaunch(addr)
		case jak1ControlStop:
			j.handleStop()
		}
	}
}

// jak1InputListener bridges the loopback UDP input stream into the file
// engine/ps2/pad.gc's patched service-cpads polls. Every packet is written
// unconditionally (regardless of whether gk is currently running -- exactly
// like sm64mod.cpp's own StreamThreadFunc streams unconditionally once
// started, see its comment); the only per-packet validation is the magic
// check, since (unlike the control family) a dropped/reordered/stale input
// packet is a non-event -- only the newest one matters, and file rename is
// what already gives us atomicity, not sequencing.
func (j *Jak1Controller) ServeInput() {
	buf := make([]byte, 64)
	for {
		n, _, err := j.inputConn.ReadFromUDP(buf)
		if err != nil {
			logf("jak1 input socket read error: %v", err)
			return
		}
		if n != jak1InputPacketSize {
			continue
		}
		if binary.LittleEndian.Uint32(buf[0:4]) != jak1InputMagic {
			continue
		}
		if err := j.writeInputFileAtomic(buf[:jak1InputPacketSize]); err != nil {
			logf("jak1 input file write failed: %v", err)
		}
	}
}

// writeInputFileAtomic writes via a temp file + rename so a concurrent
// file-stream-open/-read in gk's GOAL patch (which has no locking of its
// own -- GOAL has no mutex primitive exposed either) always sees either a
// complete old packet or a complete new one, never a torn write. POSIX
// rename() onto an existing path is atomic on the same filesystem, which
// this always is (both paths are inside jak1Dir).
func (j *Jak1Controller) writeInputFileAtomic(data []byte) error {
	tmp := j.inputPath + ".tmp"
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, j.inputPath)
}

func (j *Jak1Controller) handleLaunch(replyAddr *net.UDPAddr) {
	j.mu.Lock()
	if j.cmd != nil {
		j.mu.Unlock()
		logf("jak1 LAUNCH received but Jak1 is already running -- ignoring")
		return
	}
	j.replyAddr = replyAddr
	j.mu.Unlock()

	// gk lives under build/game/ (CMake's own output layout, see
	// extern/jak-project/CMakeLists.txt's game/ subdirectory) -- built via
	// `just build-jak1-linux`, not shipped/committed.
	gkPath := filepath.Join(j.jak1Dir, "build", "game", "gk")
	if _, err := os.Stat(gkPath); err != nil {
		logf("jak1 LAUNCH received but gk not found at %s (run `just build-jak1-linux` first) -- ignoring", gkPath)
		j.mu.Lock()
		j.replyAddr = nil
		j.mu.Unlock()
		return
	}

	// Isolated save location, deliberately separate from wherever a normal
	// `task boot-game` dev session saves (a chaos-triggered,
	// boot-straight-into-a-random-checkpoint run has no business touching
	// the player's normal OpenGOAL save data).
	saveDir := filepath.Join(j.jak1Dir, "saves_chaos")
	if err := os.MkdirAll(saveDir, 0755); err != nil {
		logf("jak1 failed to create save dir %s: %v -- ignoring LAUNCH", saveDir, err)
		j.mu.Lock()
		j.replyAddr = nil
		j.mu.Unlock()
		return
	}

	checkpoint := jak1Checkpoints[rand.Intn(len(jak1Checkpoints))]
	// --game/--config-path before the "--" separator are gk's own flags;
	// -boot/-fakeiso/-level after it are passed through to the game itself
	// (see `./build/game/gk --help`'s "Game Args" section). -level sets
	// *kernel-boot-level*, which is what makes
	// goal_src/jak1/engine/game/game-info.gc's power-cell pickup handler
	// call (kernel-shutdown) automatically -- see jak1mod.cpp's file header.
	cmd := exec.Command(gkPath,
		"--game", "jak1", "--config-path", saveDir,
		"--", "-boot", "-fakeiso", "-level", checkpoint)
	// cwd=jak1Dir (the extern/jak-project repo root, NOT gk's own build/game/
	// directory) matters for exactly one thing: our mad2_input.dat is
	// written/read relative to it. gk itself finds data/goal_src/etc.
	// relative to its own binary path regardless of cwd (verified: it logs
	// "Using development repo path: <repo root>" on startup).
	cmd.Dir = j.jak1Dir
	// SDL_VIDEODRIVER=x11: gk's vendored SDL3 build supports Wayland, and
	// gamescope doesn't reliably strip WAYLAND_DISPLAY from its child
	// processes' environment -- without this, gk's window can end up
	// created on the *outer* Wayland session instead of gamescope's nested
	// X11 display, so it never appears inside the gamescope window at all
	// (observed). Forcing X11 is always correct here since gamescope is
	// itself a nested X11 compositor.
	cmd.Env = append(os.Environ(), "SDL_VIDEODRIVER=x11")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	// Pdeathsig: same defense-in-depth reasoning as music.go's copy --
	// guarantees the kernel kills Jak1 if mad2relauncher itself is ever
	// killed outright, independent of whether its own SIGTERM handler gets
	// a chance to run StopAll().
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGTERM}

	if err := cmd.Start(); err != nil {
		logf("failed to launch Jak1 (%s): %v", gkPath, err)
		j.mu.Lock()
		j.replyAddr = nil
		j.mu.Unlock()
		return
	}
	logf("launched Jak1: %s (pid=%d, checkpoint=%s)", gkPath, cmd.Process.Pid, checkpoint)

	j.mu.Lock()
	j.cmd = cmd
	j.mu.Unlock()

	if j.x11 != nil {
		go j.x11.FocusWindowAndHideMad2("OpenGOAL", jak1WindowSize)
	}

	go func() {
		err := cmd.Wait()
		logf("Jak1 exited: %v", err)

		j.mu.Lock()
		j.cmd = nil
		reply := j.replyAddr
		j.replyAddr = nil
		j.mu.Unlock()

		if j.x11 != nil {
			j.x11.RestoreMad2()
		}
		// Stale-file guard (see NewJak1Controller's comment) -- once Jak1 is
		// gone, nothing should keep trusting whatever was last streamed to it.
		os.Remove(j.inputPath)
		j.sendExited(reply)
	}()
}

func (j *Jak1Controller) handleStop() {
	j.mu.Lock()
	cmd := j.cmd
	j.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return
	}
	// cmd.Wait()'s goroutine in handleLaunch notices the exit and sends
	// EXITED (and removes the input file) itself -- nothing further to do here.
	if pgid, err := syscall.Getpgid(cmd.Process.Pid); err == nil {
		syscall.Kill(-pgid, syscall.SIGTERM)
	} else {
		cmd.Process.Kill()
	}
}

func (j *Jak1Controller) sendExited(to *net.UDPAddr) {
	if to == nil {
		return
	}
	buf := make([]byte, 8)
	binary.LittleEndian.PutUint32(buf[0:4], jak1ControlMagic)
	binary.LittleEndian.PutUint32(buf[4:8], jak1ControlExited)
	if _, err := j.controlConn.WriteToUDP(buf, to); err != nil {
		logf("failed to send jak1 EXITED to %v: %v", to, err)
	}
}

// StopAll kills a running Jak1 child without notifying the mod -- used
// during mad2relauncher's own shutdown, same rationale as sm64.go's StopAll.
func (j *Jak1Controller) StopAll() {
	j.mu.Lock()
	cmd := j.cmd
	j.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return
	}
	if pgid, err := syscall.Getpgid(cmd.Process.Pid); err == nil {
		syscall.Kill(-pgid, syscall.SIGKILL)
	} else {
		cmd.Process.Kill()
	}
	os.Remove(j.inputPath)
}
