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

// Same "no --width/--height CLI flags, opens at a hardcoded size" situation
// as Jak1's gk -- see jak1.go's jak1WindowSize comment. Mario Legacy's gk is
// itself a fork of the same OpenGOAL PC port, so the same fixed 1280x720
// resize-to-match-gamescope approach applies unchanged.
var marioLegacyWindowSize = [2]uint32{1280, 720}

// Wire protocol shared with mad2companionmod/include/mad2mariolegacy_protocol.h
// -- hand duplicated the same way jak1.go's constants are (one side is C++,
// this is Go; see that file's own comment). Distinct magic/ports from both
// SM64's and Jak1's own, so a stray packet from either of those effects can
// never be misread here even though this process listens on several UDP
// sockets at once.
const (
	marioLegacyControlMagic  uint32 = 0x4D324D32
	marioLegacyControlLaunch uint32 = 1
	marioLegacyControlStop   uint32 = 2
	marioLegacyControlExited uint32 = 3

	marioLegacyInputMagic uint32 = 0x4D314D32
	// magic(4) + seq(4) + button0(2) + leftx(1) + lefty(1) + rightx(1) + righty(1)
	marioLegacyInputPacketSize = 14
)

// MarioLegacyController is Jak1Controller's exact twin for the vendored
// extern/mario-legacy fork (OpenGOAL-Mods/Super-Mario-Legacy, a *complete
// separate* jak-project fork -- its own gk/goalc/extractor, own goal_src,
// libsm64 statically linked in -- not something layered onto our own
// extern/jak-project checkout). Confirmed live (this session) that its
// engine/ps2/pad.gc is byte-identical to stock jak1's aside from the exact
// same controller-injection patch already applied to extern/jak-project --
// so this controller's shape and wire format are unchanged from Jak1's, and
// the C++-side XInput translation (mariolegacymod.cpp's own XInputToPs2Pad,
// byte-for-byte the same logic as jak1mod.cpp's) produces an identical
// packet layout.
//
// One real difference, found live rather than assumed: this fork's release
// layout nests everything under a `data/` subdirectory (gk/extractor/goalc
// sit at the repo root, but goal_src/decompiler_out/etc. are all under
// data/) -- confirmed its GOAL file-stream opens (the very read our pad.gc
// patch does) resolve relative to `<repo root>/data/`, not the repo root
// itself like extern/jak-project's own gk does (that fork keeps goal_src
// directly at the repo root, no data/ wrapper). So inputPath here has an
// extra "data" component Jak1Controller's doesn't -- see NewMarioLegacyController.
type MarioLegacyController struct {
	marioLegacyDir string // extern/mario-legacy -- repo root; gk itself lives at <marioLegacyDir>/gk (prebuilt, no build/game/ subdir -- this is a release tarball, not our own source build)
	inputPort      int

	controlConn *net.UDPConn
	inputConn   *net.UDPConn
	inputPath   string // <marioLegacyDir>/data/mad2_input.dat -- what the GOAL patch polls (see file header for the "data/" prefix)
	x11         *X11   // nil if X11 connection failed -- launching still works, just no window focus juggling

	mu        sync.Mutex
	cmd       *exec.Cmd
	replyAddr *net.UDPAddr
}

func NewMarioLegacyController(marioLegacyDir string, inputPort, controlPort int, x11 *X11) (*MarioLegacyController, error) {
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

	m := &MarioLegacyController{
		marioLegacyDir: marioLegacyDir,
		inputPort:      inputPort,
		controlConn:    controlConn,
		inputConn:      inputConn,
		inputPath:      filepath.Join(marioLegacyDir, "data", "mad2_input.dat"),
		x11:            x11,
	}
	// Defensive: same stale-file guard as Jak1Controller's own constructor.
	os.Remove(m.inputPath)
	return m, nil
}

// ServeControl -- identical shape to Jak1Controller.ServeControl.
func (m *MarioLegacyController) ServeControl() {
	buf := make([]byte, 64)
	for {
		n, addr, err := m.controlConn.ReadFromUDP(buf)
		if err != nil {
			logf("mariolegacy control socket read error: %v", err)
			return
		}
		if n != 8 {
			continue
		}
		magic := binary.LittleEndian.Uint32(buf[0:4])
		if magic != marioLegacyControlMagic {
			continue
		}
		switch binary.LittleEndian.Uint32(buf[4:8]) {
		case marioLegacyControlLaunch:
			m.handleLaunch(addr)
		case marioLegacyControlStop:
			m.handleStop()
		}
	}
}

// ServeInput -- identical shape to Jak1Controller.ServeInput.
func (m *MarioLegacyController) ServeInput() {
	buf := make([]byte, 64)
	for {
		n, _, err := m.inputConn.ReadFromUDP(buf)
		if err != nil {
			logf("mariolegacy input socket read error: %v", err)
			return
		}
		if n != marioLegacyInputPacketSize {
			continue
		}
		if binary.LittleEndian.Uint32(buf[0:4]) != marioLegacyInputMagic {
			continue
		}
		if err := m.writeInputFileAtomic(buf[:marioLegacyInputPacketSize]); err != nil {
			logf("mariolegacy input file write failed: %v", err)
		}
	}
}

func (m *MarioLegacyController) writeInputFileAtomic(data []byte) error {
	tmp := m.inputPath + ".tmp"
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, m.inputPath)
}

func (m *MarioLegacyController) handleLaunch(replyAddr *net.UDPAddr) {
	m.mu.Lock()
	if m.cmd != nil {
		m.mu.Unlock()
		logf("mariolegacy LAUNCH received but Mario Legacy is already running -- ignoring")
		return
	}
	m.replyAddr = replyAddr
	m.mu.Unlock()

	// gk sits directly at the repo root in this vendored tree -- unlike
	// extern/jak-project's own build/game/gk, this is a prebuilt release
	// tarball with no build step (see extern/mario-legacy's own setup notes
	// in justfile/CLAUDE.md), so there's no build/ subdirectory to look under.
	gkPath := filepath.Join(m.marioLegacyDir, "gk")
	if _, err := os.Stat(gkPath); err != nil {
		logf("mariolegacy LAUNCH received but gk not found at %s (see justfile's mario-legacy setup notes) -- ignoring", gkPath)
		m.mu.Lock()
		m.replyAddr = nil
		m.mu.Unlock()
		return
	}

	// Isolated save location, same rationale as Jak1Controller's saveDir --
	// a chaos-triggered run has no business touching any real playthrough's
	// save data (not that this fork's saves would even be compatible with
	// extern/jak-project's -- different game entirely from GOAL's POV).
	saveDir := filepath.Join(m.marioLegacyDir, "saves_chaos")
	if err := os.MkdirAll(saveDir, 0755); err != nil {
		logf("mariolegacy failed to create save dir %s: %v -- ignoring LAUNCH", saveDir, err)
		m.mu.Lock()
		m.replyAddr = nil
		m.mu.Unlock()
		return
	}

	// Reuses jak1.go's own checkpoint-name list (jak1Checkpoints) rather than
	// a second copy -- confirmed live this session that this fork's
	// engine/level/level-info.gc continue-point names are the same jak1
	// level set (Mario replaces Jak's model/moveset in place, it doesn't
	// rename or restructure the level list). NOTE: unlike JakChallenge, this
	// fork does NOT have our kernel-defs.gc/game-info.gc/level.gc
	// title-screen-skip patch (diffed live against extern/jak-project's
	// ported version -- absent here entirely, this fork's game-info.gc never
	// looks at *kernel-boot-level* at all) -- so -level here currently has NO
	// effect on where the game boots to; it will sit at the title screen
	// like Jak1 did before that patch was ported (see progress.md Round 6).
	// Passed anyway (harmless) so porting that patch later is a pure
	// GOAL-side change with nothing to update here.
	checkpoint := jak1Checkpoints[rand.Intn(len(jak1Checkpoints))]
	cmd := exec.Command(gkPath,
		"--game", "jak1", "--config-path", saveDir,
		"--", "-boot", "-fakeiso", "-level", checkpoint)
	// cwd=marioLegacyDir (repo root, containing gk/extractor/goalc/data/) --
	// matters for the same one thing as Jak1Controller's cmd.Dir: our
	// mad2_input.dat is written/read relative to it (well, relative to its
	// data/ subdirectory -- see inputPath). gk finds its own data/ folder
	// relative to its own binary path regardless of cwd, same as confirmed
	// for extern/jak-project's gk.
	cmd.Dir = m.marioLegacyDir
	// SDL_VIDEODRIVER=x11 -- identical gamescope-nested-Wayland-vs-X11
	// reasoning as jak1.go's own copy; this fork's gk is built against the
	// same SDL and hits the same issue.
	cmd.Env = append(os.Environ(), "SDL_VIDEODRIVER=x11")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGTERM}

	if err := cmd.Start(); err != nil {
		logf("failed to launch Mario Legacy (%s): %v", gkPath, err)
		m.mu.Lock()
		m.replyAddr = nil
		m.mu.Unlock()
		return
	}
	logf("launched Mario Legacy: %s (pid=%d, checkpoint=%s)", gkPath, cmd.Process.Pid, checkpoint)

	m.mu.Lock()
	m.cmd = cmd
	m.mu.Unlock()

	if m.x11 != nil {
		// Window title is "OpenGOAL" here too (confirmed via `strings gk`) --
		// same as extern/jak-project's own gk, since this is the same
		// upstream PC-port window-creation code. Only one companion process
		// is expected active at a time in practice (mad2effects doesn't
		// stack SM64Challenge/JakChallenge/MarioLegacyChallenge against each
		// other today), so the shared title isn't disambiguated further here.
		go m.x11.FocusWindowAndHideMad2("OpenGOAL", marioLegacyWindowSize)
	}

	go func() {
		err := cmd.Wait()
		logf("Mario Legacy exited: %v", err)

		m.mu.Lock()
		m.cmd = nil
		reply := m.replyAddr
		m.replyAddr = nil
		m.mu.Unlock()

		if m.x11 != nil {
			m.x11.RestoreMad2()
		}
		os.Remove(m.inputPath)
		m.sendExited(reply)
	}()
}

func (m *MarioLegacyController) handleStop() {
	m.mu.Lock()
	cmd := m.cmd
	m.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return
	}
	if pgid, err := syscall.Getpgid(cmd.Process.Pid); err == nil {
		syscall.Kill(-pgid, syscall.SIGTERM)
	} else {
		cmd.Process.Kill()
	}
}

func (m *MarioLegacyController) sendExited(to *net.UDPAddr) {
	if to == nil {
		return
	}
	buf := make([]byte, 8)
	binary.LittleEndian.PutUint32(buf[0:4], marioLegacyControlMagic)
	binary.LittleEndian.PutUint32(buf[4:8], marioLegacyControlExited)
	if _, err := m.controlConn.WriteToUDP(buf, to); err != nil {
		logf("failed to send mariolegacy EXITED to %v: %v", to, err)
	}
}

// StopAll -- identical shape to Jak1Controller.StopAll.
func (m *MarioLegacyController) StopAll() {
	m.mu.Lock()
	cmd := m.cmd
	m.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return
	}
	if pgid, err := syscall.Getpgid(cmd.Process.Pid); err == nil {
		syscall.Kill(-pgid, syscall.SIGKILL)
	} else {
		cmd.Process.Kill()
	}
	os.Remove(m.inputPath)
}
