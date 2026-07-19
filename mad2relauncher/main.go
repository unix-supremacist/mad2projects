// mad2relauncher is a small shim that runs inside the same gamescope
// instance as Mad2.exe (see justfile's _play recipe, which execs this
// instead of `umu-run` directly). It has four jobs:
//
//  1. Crash relaunch: keep `umu-run <exe>` running, relaunching it on
//     every exit (crash or otherwise) until told to stop -- see
//     gameloop.go. Stops on Ctrl+Q (a global X11 hotkey grabbed on
//     gamescope's nested display, see x11.go), SIGTERM/SIGINT, or gamescope
//     itself dying -- armParentDeathSignal (parentwatch.go) arms
//     PR_SET_PDEATHSIG so the kernel sends us SIGTERM the instant our
//     parent process exits, by whatever means (crash, SIGKILL, the window
//     being closed), rather than relying on gamescope's own child-teardown
//     behavior to reach us -- it doesn't always: observed mad2music being
//     orphaned/double-launched and the game failing to relaunch when
//     gamescope died before this was added.
//
//  2. SM64 orchestration: when mad2sm64mod.dll (running inside the Wine
//     process, see ../mad2sm64mod) triggers its "SM64Challenge" chaos
//     effect, it can't launch SM64 itself -- SM64 needs to be a native
//     Linux process, not something inside Wine, to avoid stacking two
//     D3D9 translation layers. mad2relauncher listens on a loopback UDP
//     control port for LAUNCH/STOP requests, forks the native Linux
//     extern/sm64ex-alo binary, and does the window-focus juggling (raise
//     SM64, move Mad2 off-screen, and back on exit) via the same X11
//     connection -- see sm64.go/x11.go.
//
//  3. Jak1 orchestration: same idea as (2), for mad2companionmod's other
//     effect, "JakChallenge" -- forks extern/jak-project's gk (Jak & Daxter's
//     OpenGOAL runtime) instead of sm64ex-alo -- see jak1.go. One job
//     Sm64Controller doesn't have: gk has no socket primitive to receive
//     Mad2's streamed controller state directly (unlike the vendored,
//     source-patchable extern/sm64ex-alo), so Jak1Controller is also the
//     actual UDP receiving endpoint for that stream, bridging it into gk
//     via a file a GOAL-side patch polls once per frame -- see jak1.go's
//     file header and ../mad2companionmod/include/mad2jak1_protocol.h.
//
//  3b. Mario Legacy orchestration: MarioLegacyController is Jak1Controller's
//      exact twin for extern/mario-legacy, a *separate* vendored jak-project
//      fork (own gk/goalc/extractor, libsm64 statically linked in) that
//      swaps Mario in for Jak -- see mariolegacy.go and
//      ../mad2companionmod/include/mad2mariolegacy_protocol.h. Same
//      file-bridge mechanism as Jak1, on its own ports so a stray packet
//      from one effect can never be misread by another's listener.
//
//  4. Music/podcast supervision: unlike SM64/Jak1 (launched on demand), the
//     native Linux mad2music binary (see ../mad2music) is started once
//     alongside the game and kept running/relaunched on crash for the
//     whole session, same crash-relaunch shape as the game loop itself --
//     see music.go. mad2podcastmod.dll (../mad2podcastmod) talks straight
//     to mad2music's own loopback UDP control port to trigger a podcast
//     clip; mad2relauncher has no protocol role here, purely process
//     supervision.
package main

import (
	"flag"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	armParentDeathSignal()

	sm64Path := flag.String("sm64-path", "", "path to the native Linux sm64ex-alo binary (extern/sm64ex-alo/build/us_pc/sm64.us.f3dex2e)")
	inputPort := flag.Int("input-port", 47064, "loopback UDP port mad2sm64mod streams controller state to SM64 on")
	controlPort := flag.Int("control-port", 47065, "loopback UDP port mad2sm64mod uses to request SM64 launch/stop")
	jak1Dir := flag.String("jak1-dir", "", "path to the extern/jak-project directory (contains build/game/gk, goal_src/, etc.) -- if empty, JakChallenge is disabled")
	jak1InputPort := flag.Int("jak1-input-port", 47066, "loopback UDP port mad2sm64mod streams controller state to for JakChallenge")
	jak1ControlPort := flag.Int("jak1-control-port", 47067, "loopback UDP port mad2sm64mod uses to request Jak1 launch/stop")
	marioLegacyDir := flag.String("mariolegacy-dir", "", "path to the extern/mario-legacy directory (contains gk, goalc, extractor, data/, etc.) -- if empty, MarioLegacyChallenge is disabled")
	marioLegacyInputPort := flag.Int("mariolegacy-input-port", 47068, "loopback UDP port mad2companionmod streams controller state to for MarioLegacyChallenge")
	marioLegacyControlPort := flag.Int("mariolegacy-control-port", 47069, "loopback UDP port mad2companionmod uses to request Mario Legacy launch/stop")
	musicPath := flag.String("music-path", "", "path to the native Linux mad2music binary (build/mad2music) -- if empty, background music/podcast playback is disabled")
	musicAudioDir := flag.String("music-audio-dir", "", "absolute path to mad2music/audio (its music pool + podcast clips) -- required if -music-path is set")
	flag.Parse()

	if flag.NArg() < 1 {
		os.Stderr.WriteString("usage: mad2relauncher [flags] <exe>\n")
		flag.PrintDefaults()
		os.Exit(2)
	}
	exe := flag.Arg(0)

	closeLog := initLogger()
	defer closeLog()
	logf("mad2relauncher starting (exe=%s sm64Path=%q inputPort=%d controlPort=%d)", exe, *sm64Path, *inputPort, *controlPort)

	loop := NewGameLoop(exe)

	x11, err := OpenX11()
	if err != nil {
		logf("X11 unavailable (%v) -- Ctrl+Q stop-key and SM64 window focus juggling are disabled, everything else still works", err)
		x11 = nil
	}

	sm64, err := NewSm64Controller(*sm64Path, *inputPort, *controlPort, x11)
	if err != nil {
		logf("failed to start SM64 control listener: %v -- SM64Challenge will not work this session", err)
		sm64 = nil
	} else {
		go sm64.Serve()
	}

	var jak1 *Jak1Controller
	if *jak1Dir != "" {
		jak1, err = NewJak1Controller(*jak1Dir, *jak1InputPort, *jak1ControlPort, x11)
		if err != nil {
			logf("failed to start Jak1 control/input listeners: %v -- JakChallenge will not work this session", err)
			jak1 = nil
		} else {
			go jak1.ServeControl()
			go jak1.ServeInput()
		}
	} else {
		logf("-jak1-dir not set -- JakChallenge disabled this session")
	}

	var marioLegacy *MarioLegacyController
	if *marioLegacyDir != "" {
		marioLegacy, err = NewMarioLegacyController(*marioLegacyDir, *marioLegacyInputPort, *marioLegacyControlPort, x11)
		if err != nil {
			logf("failed to start Mario Legacy control/input listeners: %v -- MarioLegacyChallenge will not work this session", err)
			marioLegacy = nil
		} else {
			go marioLegacy.ServeControl()
			go marioLegacy.ServeInput()
		}
	} else {
		logf("-mariolegacy-dir not set -- MarioLegacyChallenge disabled this session")
	}

	var music *MusicLoop
	if *musicPath != "" {
		music = NewMusicLoop(*musicPath, *musicAudioDir)
		go music.Run()
	} else {
		logf("-music-path not set -- background music/podcast playback disabled this session")
	}

	stop := func() {
		logf("stop requested -- tearing down")
		if sm64 != nil {
			sm64.StopAll()
		}
		if jak1 != nil {
			jak1.StopAll()
		}
		if marioLegacy != nil {
			marioLegacy.StopAll()
		}
		if music != nil {
			music.Stop()
		}
		loop.Stop()
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		sig := <-sigCh
		logf("received signal %v", sig)
		stop()
	}()

	if x11 != nil {
		go func() {
			if err := x11.GrabStopKey(stop); err != nil {
				logf("Ctrl+Q grab failed: %v -- stop-key is disabled, use SIGTERM/closing gamescope instead", err)
			}
		}()
	}

	loop.Run()
	logf("mad2relauncher exiting")
}
