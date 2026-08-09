#!/usr/bin/env python3
"""Source-backed transport/stream-clock contracts, not runtime gameplay proof."""

from dataclasses import dataclass
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOAT = ROOT / "goal_src/jak1/levels/village1/fishermans-boat.gc"
GONDOLA = ROOT / "goal_src/jak1/levels/village3/village3-obs.gc"
LOADER = ROOT / "goal_src/jak1/engine/load/loader.gc"
VAG_STREAM = ROOT / "game/kernel/core/vag_stream.cpp"
SOUND_RPC = ROOT / "game/kernel/core/sound_rpc.cpp"
UPSTREAM_ISO = ROOT / "game/overlord/jak1/iso.cpp"
UPSTREAM_SRPC = ROOT / "game/overlord/jak1/srpc.cpp"


def extract_form(source: str, marker: str) -> str:
    start = source.index(marker)
    depth = 0
    in_string = False
    escaped = False
    in_comment = False
    for index in range(start, len(source)):
        char = source[index]
        if in_comment:
            if char == "\n":
                in_comment = False
            continue
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == ";":
            in_comment = True
        elif char == '"':
            in_string = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated form: {marker}")


def extract_cpp_function(source: str, marker: str) -> str:
    start = source.index(marker)
    open_brace = source.index("{", start)
    depth = 0
    in_string = False
    escaped = False
    line_comment = False
    block_comment = False
    for index in range(open_brace, len(source)):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if line_comment:
            if char == "\n":
                line_comment = False
            continue
        if block_comment:
            if char == "*" and following == "/":
                block_comment = False
            continue
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == "/" and following == "/":
            line_comment = True
        elif char == "/" and following == "*":
            block_comment = True
        elif char == '"':
            in_string = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {marker}")


@dataclass
class ClockModel:
    sound_id: int = 0
    position: int = -1
    running: bool = False
    paused: bool = False

    def play_missing(self, sound_id: int) -> None:
        self.sound_id = sound_id
        self.position = 0
        self.running = True
        self.paused = False

    def frame(self) -> None:
        if self.running and not self.paused:
            self.position += 1024 // 60

    def stop(self) -> None:
        self.sound_id = 1
        self.position = -1
        self.running = False
        self.paused = False


class Jak1SpooledAnimationClockContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.boat = BOAT.read_text()
        cls.gondola = GONDOLA.read_text()
        cls.loader = LOADER.read_text()
        cls.vag_stream = VAG_STREAM.read_text()
        cls.sound_rpc = SOUND_RPC.read_text()
        cls.upstream_iso = UPSTREAM_ISO.read_text()
        cls.upstream_srpc = UPSTREAM_SRPC.read_text()

    def test_boat_and_gondola_commit_only_after_spooled_animation(self) -> None:
        boat_finalizer = "(fishermans-boat-complete-ride"
        for marker in (
            "(defstate fishermans-boat-ride-to-misty",
            "(defstate fishermans-boat-ride-to-village1",
        ):
            ride = extract_form(self.boat, marker)
            self.assertEqual(ride.count("(ja-play-spooled-anim"), 1)
            self.assertLess(ride.index("(ja-play-spooled-anim"), ride.index(boat_finalizer))

        for marker in ("(defstate ride-up (gondola)", "(defstate ride-down (gondola)"):
            ride = extract_form(self.gondola, marker)
            self.assertEqual(ride.count("(ja-play-spooled-anim"), 1)
            self.assertLess(ride.index("(ja-play-spooled-anim"), ride.index("(move-to-ground"))
            self.assertLess(ride.index("(move-to-ground"), ride.index("(set-continue!"))

        player = extract_form(self.loader, "(defbehavior ja-play-spooled-anim")
        timeline = player[player.index("(let ((sv-72") : player.index("(set! sv-24 f28-0)")]
        self.assertLess(timeline.index("(execute-commands-up-to"), timeline.index("(suspend)"))
        self.assertIn("(current-str-pos spool-sound)", player)

    def test_portable_missing_stream_uses_upstream_fake_clock_contract(self) -> None:
        self.assertIn("gFakeVAGClockRunning = true", self.upstream_iso)
        self.assertIn(
            "gFakeVAGClock += (s32)(1024 / Gfx::g_global_settings.target_fps)",
            self.upstream_srpc,
        )

        play = extract_cpp_function(self.vag_stream, "void play(const VagStreamEntry*")
        self.assertIn("if (!g_installed || !vag)", play)
        self.assertEqual(play.count("start_fake_clock(vag, sound_id, priority)"), 2)
        self.assertLess(
            play.index("if (!g_installed || !vag)"),
            play.index("start_fake_clock(vag, sound_id, priority)"),
        )
        self.assertLess(
            play.index("if (!g_active)"),
            play.rindex("start_fake_clock(vag, sound_id, priority)"),
        )

        frame = extract_cpp_function(self.vag_stream, "void frame()")
        self.assertIn("if (g_fake_clock_running && !g_fake_clock_paused)", frame)
        self.assertIn("g_fake_clock += kFakeClockStep", frame)
        self.assertIn("constexpr s32 kFakeClockStep = 1024 / 60", self.vag_stream)

        spool_request = extract_cpp_function(self.sound_rpc, "void play_spool_request(")
        missing = spool_request[spool_request.index("if (!vag)") :]
        self.assertNotIn("return;", missing[: missing.index("vag_stream::play")])

        rpc_play = extract_cpp_function(self.sound_rpc, "void rpc_play(")
        missing = rpc_play[rpc_play.index("if (!vag)") :]
        self.assertNotIn("continue;", missing[: missing.index("if (cmd->result == 0)")])

    def test_missing_stream_clock_advances_pauses_resumes_and_stops(self) -> None:
        clock = ClockModel()
        clock.play_missing(0x10001)
        for _ in range(60):
            clock.frame()
        self.assertEqual((clock.sound_id, clock.position), (0x10001, 1020))

        clock.paused = True
        for _ in range(30):
            clock.frame()
        self.assertEqual(clock.position, 1020)

        clock.paused = False
        clock.frame()
        self.assertEqual(clock.position, 1037)

        clock.stop()
        clock.frame()
        self.assertEqual((clock.sound_id, clock.position, clock.running), (1, -1, False))

        pause = extract_cpp_function(self.vag_stream, "void pause()")
        unpause = extract_cpp_function(self.vag_stream, "void unpause()")
        stop = extract_cpp_function(self.vag_stream, "void stop(const VagStreamEntry*")
        self.assertIn("g_fake_clock_paused = true", pause)
        self.assertIn("g_fake_clock_paused = false", unpause)
        self.assertIn("stop_fake_clock()", stop)

    def test_real_stream_keeps_the_sample_clock_path(self) -> None:
        play = extract_cpp_function(self.vag_stream, "void play(const VagStreamEntry*")
        real_start = play.index("begin(vag, sound_id, volume, priority, trans)")
        fallback = play.index("if (!g_active)", real_start)
        real_clock = play.index("g_real_clock_running = true", fallback)
        self.assertLess(real_start, fallback)
        self.assertLess(fallback, real_clock)
        self.assertIn("g_fake_clock_running = false", play[fallback:real_clock])
        self.assertIn("g_vag_id = (s32)g_stream.sound_id", play[real_clock:])


if __name__ == "__main__":
    unittest.main()
