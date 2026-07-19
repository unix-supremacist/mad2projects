package main

import (
	"encoding/binary"
	"fmt"
	"math/rand"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"sync"
	"syscall"
)

// Same course pool ../mad2mod/src/main.cpp's own SM64 sub-game challenge
// picks from (launch_sm64_level's sm64_levels[] / the chaos-event trigger
// site) -- reused rather than invented, since these are the courses that
// tool already verified work well as a short, self-contained mini-challenge.
var sm64LevelPool = []int{9, 24, 12, 5, 4, 7, 22, 8, 23, 10, 11, 36, 13, 14, 15, 27}

// Wire protocol shared with mad2sm64mod/include/mad2sm64_protocol.h --
// kept as a hand-written duplicate here rather than a cross-language
// shared source of truth (there isn't one -- one side is C, this is Go).
// Keep in sync by hand; the format is tiny and stable.
const (
	controlMagic  uint32 = 0x43344D32
	controlLaunch uint32 = 1
	controlStop   uint32 = 2
	controlExited uint32 = 3
)

// Sm64Controller owns the loopback UDP control listener that
// mad2sm64mod.dll (inside the Wine/Mad2 process) talks to, and the
// lifecycle of the native Linux SM64 child process it launches in
// response to a LAUNCH message -- see mad2sm64mod/src/sm64mod.cpp's file
// header for the full picture of how the two sides coordinate.
type Sm64Controller struct {
	sm64Path  string
	inputPort int
	conn      *net.UDPConn
	x11       *X11 // nil if X11 connection failed -- launching still works, just no window focus juggling

	mu        sync.Mutex
	cmd       *exec.Cmd
	replyAddr *net.UDPAddr
}

func NewSm64Controller(sm64Path string, inputPort, controlPort int, x11 *X11) (*Sm64Controller, error) {
	addr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: controlPort}
	conn, err := net.ListenUDP("udp", addr)
	if err != nil {
		return nil, fmt.Errorf("listen on control port %d: %w", controlPort, err)
	}
	return &Sm64Controller{sm64Path: sm64Path, inputPort: inputPort, conn: conn, x11: x11}, nil
}

// Serve pumps the control socket forever. Call with `go`.
func (s *Sm64Controller) Serve() {
	buf := make([]byte, 64)
	for {
		n, addr, err := s.conn.ReadFromUDP(buf)
		if err != nil {
			logf("control socket read error: %v", err)
			return
		}
		if n != 8 {
			continue
		}
		magic := binary.LittleEndian.Uint32(buf[0:4])
		if magic != controlMagic {
			continue
		}
		switch binary.LittleEndian.Uint32(buf[4:8]) {
		case controlLaunch:
			s.handleLaunch(addr)
		case controlStop:
			s.handleStop()
		}
	}
}

func (s *Sm64Controller) handleLaunch(replyAddr *net.UDPAddr) {
	s.mu.Lock()
	if s.cmd != nil {
		s.mu.Unlock()
		logf("LAUNCH received but SM64 is already running -- ignoring")
		return
	}
	if s.sm64Path == "" {
		s.mu.Unlock()
		logf("LAUNCH received but -sm64-path was not configured -- ignoring")
		return
	}
	s.replyAddr = replyAddr
	s.mu.Unlock()

	// Plain, no-argument launch boots to SM64's own title/file-select
	// screen -- these flags (sm64ex-alo's own CLI options, src/pc/cliopts.c)
	// are what ../mad2mod/src/main.cpp's launch_sm64_level used to warp
	// straight into a short, self-contained course instead, which is the
	// actual "SM64 mini-challenge" experience this effect is meant to be.
	level := sm64LevelPool[rand.Intn(len(sm64LevelPool))]
	cmd := exec.Command(s.sm64Path,
		"--skip-intro", "--level", strconv.Itoa(level),
		"--width", "1280", "--height", "720", "--fullscreen")
	cmd.Dir = filepath.Dir(s.sm64Path)
	// SDL_VIDEODRIVER=x11: belt-and-suspenders alongside jak1.go's own copy
	// of this -- see its comment for why (gamescope doesn't reliably strip
	// WAYLAND_DISPLAY from child processes, which can send an SDL window
	// to the outer Wayland session instead of gamescope's nested X11 one).
	cmd.Env = append(os.Environ(),
		fmt.Sprintf("MAD2SM64_INPUT_PORT=%d", s.inputPort),
		"SDL_VIDEODRIVER=x11")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	// Pdeathsig: same defense-in-depth reasoning as music.go's copy --
	// guarantees the kernel kills SM64 if mad2relauncher itself is ever
	// killed outright, independent of whether its own SIGTERM handler gets
	// a chance to run StopAll().
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true, Pdeathsig: syscall.SIGTERM}

	if err := cmd.Start(); err != nil {
		logf("failed to launch SM64 (%s): %v", s.sm64Path, err)
		s.mu.Lock()
		s.replyAddr = nil
		s.mu.Unlock()
		return
	}
	logf("launched SM64: %s (pid=%d, level=%d)", s.sm64Path, cmd.Process.Pid, level)

	s.mu.Lock()
	s.cmd = cmd
	s.mu.Unlock()

	if s.x11 != nil {
		go s.x11.FocusWindowAndHideMad2("Super Mario 64", [2]uint32{0, 0})
	}

	go func() {
		err := cmd.Wait()
		logf("SM64 exited: %v", err)

		s.mu.Lock()
		s.cmd = nil
		reply := s.replyAddr
		s.replyAddr = nil
		s.mu.Unlock()

		if s.x11 != nil {
			s.x11.RestoreMad2()
		}
		s.sendExited(reply)
	}()
}

func (s *Sm64Controller) handleStop() {
	s.mu.Lock()
	cmd := s.cmd
	s.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return
	}
	// cmd.Wait()'s goroutine in handleLaunch notices the exit and sends
	// EXITED itself -- nothing further to do here once the signal is sent.
	if pgid, err := syscall.Getpgid(cmd.Process.Pid); err == nil {
		syscall.Kill(-pgid, syscall.SIGTERM)
	} else {
		cmd.Process.Kill()
	}
}

func (s *Sm64Controller) sendExited(to *net.UDPAddr) {
	if to == nil {
		return
	}
	buf := make([]byte, 8)
	binary.LittleEndian.PutUint32(buf[0:4], controlMagic)
	binary.LittleEndian.PutUint32(buf[4:8], controlExited)
	if _, err := s.conn.WriteToUDP(buf, to); err != nil {
		logf("failed to send EXITED to %v: %v", to, err)
	}
}

// StopAll kills a running SM64 child without notifying the mod -- used
// during mad2relauncher's own shutdown (Ctrl+Q/SIGTERM), where the whole
// process tree (mod included, since it lives inside Mad2.exe) is going
// away regardless.
func (s *Sm64Controller) StopAll() {
	s.mu.Lock()
	cmd := s.cmd
	s.mu.Unlock()
	if cmd == nil || cmd.Process == nil {
		return
	}
	if pgid, err := syscall.Getpgid(cmd.Process.Pid); err == nil {
		syscall.Kill(-pgid, syscall.SIGKILL)
	} else {
		cmd.Process.Kill()
	}
}
