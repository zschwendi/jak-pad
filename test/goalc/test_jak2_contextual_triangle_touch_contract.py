#!/usr/bin/env python3

from dataclasses import dataclass
from pathlib import Path
from typing import Optional
import unittest


ROOT = Path(__file__).resolve().parents[2]
JAK2 = ROOT / "goal_src/jak2"

PROCESS_TASKABLE = JAK2 / "engine/process-drawable/process-taskable.gc"
PAD = JAK2 / "engine/ps2/pad.gc"
AMBIENT = JAK2 / "engine/ambient/ambient.gc"
VEHICLE = JAK2 / "levels/city/traffic/vehicle/vehicle-util.gc"
MECH = JAK2 / "engine/target/mech/mech.gc"
TURRET = JAK2 / "engine/target/target-turret.gc"
BURNING_BUSH = JAK2 / "levels/city/ctywide-obs.gc"
WARP = JAK2 / "levels/common/warp-gate.gc"
GAME_DGO = JAK2 / "dgos/game.gd"

TRIANGLE = 1 << 12


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


@dataclass
class SnapshotModel:
    owner: Optional[object] = None
    requested_buttons: int = 0
    sequence: int = 0
    heartbeat: int = 0

    def publish(self, owner: object, requested_buttons: int) -> None:
        if not requested_buttons:
            return
        if self.owner is owner and self.requested_buttons == requested_buttons:
            self.heartbeat += 1
            return
        self.sequence += 1
        self.owner = owner
        self.requested_buttons = requested_buttons
        self.heartbeat = 1
        self.sequence += 1

    def clear(self, owner: object) -> None:
        if self.owner is not owner:
            return
        self.sequence += 1
        self.owner = None
        self.requested_buttons = 0
        self.heartbeat = 0
        self.sequence += 1


class Jak2ContextualTriangleTouchContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.process_taskable = PROCESS_TASKABLE.read_text()
        cls.pad = PAD.read_text()
        cls.ambient = AMBIENT.read_text()
        cls.vehicle = VEHICLE.read_text()
        cls.mech = MECH.read_text()
        cls.turret = TURRET.read_text()
        cls.burning_bush = BURNING_BUSH.read_text()
        cls.warp = WARP.read_text()

    def test_fixed_revisioned_owner_snapshot_abi(self) -> None:
        snapshot_type = extract_form(
            self.process_taskable,
            "(deftype goalpad-face-prompt-touch-snapshot",
        )
        for field in (
            "(revision          int32  :offset-assert 4)",
            "(sequence          int32  :offset-assert 8)",
            "(requested-buttons uint32 :offset-assert 12)",
            "(owner             handle :offset-assert 16)",
            "(heartbeat         int32  :offset-assert 24)",
        ):
            self.assertIn(field, snapshot_type)
        self.assertIn(":method-count-assert 9", snapshot_type)
        self.assertIn(":size-assert #x1c", snapshot_type)

        snapshot = extract_form(
            self.process_taskable,
            "(define *goalpad-face-prompt-touch-snapshot*",
        )
        self.assertIn(":revision 1", snapshot)
        self.assertIn(":sequence 0", snapshot)
        self.assertIn(":requested-buttons 0", snapshot)
        self.assertIn(":heartbeat 0", snapshot)
        self.assertNotIn(":owner", snapshot)
        self.assertIn(";; dgos: GAME, COMMON", self.process_taskable)
        self.assertIn('"process-taskable.o"', GAME_DGO.read_text())

    def test_triangle_is_the_ordinary_pad_button_mask(self) -> None:
        buttons = extract_form(self.pad, "(defenum pad-buttons")
        self.assertIn("(triangle 12)", buttons)
        self.assertEqual(TRIANGLE, 0x1000)

    def test_publish_clear_seqlock_and_heartbeat_contract(self) -> None:
        publish = extract_form(
            self.process_taskable,
            "(defun goalpad-face-prompt-touch-publish!",
        )
        clear = extract_form(
            self.process_taskable,
            "(defun goalpad-face-prompt-touch-clear!",
        )
        self.assertIn("(and owner (nonzero? (the-as uint requested-buttons)))", publish)
        self.assertIn("(= *goalpad-face-prompt-touch-owner* owner)", publish)
        self.assertEqual(
            publish.count("(+! (-> *goalpad-face-prompt-touch-snapshot* sequence) 1)"),
            2,
        )
        self.assertIn("(+! (-> *goalpad-face-prompt-touch-snapshot* heartbeat) 1)", publish)
        self.assertIn("(set! (-> *goalpad-face-prompt-touch-snapshot* heartbeat) 1)", publish)
        self.assertIn("(process->handle owner)", publish)

        self.assertIn("(when (= *goalpad-face-prompt-touch-owner* owner)", clear)
        self.assertEqual(
            clear.count("(+! (-> *goalpad-face-prompt-touch-snapshot* sequence) 1)"),
            2,
        )
        self.assertIn("requested-buttons) 0", clear)
        self.assertIn("owner) (the-as handle 0)", clear)
        self.assertIn("heartbeat) 0", clear)

    def test_owner_heartbeat_even_sequence_and_stale_clear_model(self) -> None:
        first = object()
        second = object()
        snapshot = SnapshotModel()

        snapshot.publish(first, 0)
        self.assertEqual((snapshot.sequence, snapshot.owner), (0, None))
        snapshot.publish(first, TRIANGLE)
        self.assertEqual((snapshot.sequence, snapshot.heartbeat), (2, 1))
        self.assertEqual(snapshot.sequence % 2, 0)
        snapshot.publish(first, TRIANGLE)
        self.assertEqual((snapshot.sequence, snapshot.heartbeat), (2, 2))
        snapshot.publish(second, TRIANGLE)
        self.assertEqual((snapshot.owner, snapshot.sequence, snapshot.heartbeat), (second, 4, 1))
        snapshot.clear(first)
        self.assertEqual((snapshot.owner, snapshot.sequence), (second, 4))
        snapshot.clear(second)
        self.assertEqual(
            (snapshot.owner, snapshot.requested_buttons, snapshot.sequence, snapshot.heartbeat),
            (None, 0, 6, 0),
        )
        self.assertEqual(snapshot.sequence % 2, 0)

    def test_six_authored_display_gates_publish_only_after_drawing(self) -> None:
        cases = (
            (
                self.process_taskable,
                "(defstate idle (process-taskable)",
                "(print-game-text",
                "self",
            ),
            (
                self.vehicle,
                "(defmethod check-player-get-on ((this vehicle))",
                "(print-game-text",
                "this",
            ),
            (self.mech, "(defstate idle (mech)", "(print-game-text", "self"),
            (self.turret, "(defstate idle (base-turret)", "(print-game-text", "self"),
            (
                self.burning_bush,
                "(defstate idle (burning-bush)",
                "(s4-1 *temp-string*",
                "self",
            ),
            (self.warp, "(defstate idle (warp-gate)", "(print-game-text", "self"),
        )
        for source, marker, draw_marker, owner in cases:
            with self.subTest(marker=marker):
                form = extract_form(source, marker)
                self.assertEqual(form.count("(can-display-query?"), 1)
                self.assertEqual(
                    form.count("(goalpad-face-prompt-touch-publish!"),
                    1,
                )
                self.assertGreaterEqual(
                    form.count("(goalpad-face-prompt-touch-clear!"),
                    2,
                )
                gate = form.index("(can-display-query?")
                draw = form.index(draw_marker, gate)
                publish = form.index("(goalpad-face-prompt-touch-publish!", draw)
                self.assertLess(gate, draw)
                self.assertLess(draw, publish)
                self.assertIn(
                    f"(goalpad-face-prompt-touch-publish! {owner} (pad-buttons triangle))",
                    form,
                )

        self.assertNotIn("goalpad-face-prompt", self.ambient)
        authored_files = {
            path.relative_to(ROOT).as_posix()
            for path in ROOT.glob("goal_src/jak2/**/*.gc")
            if "goalpad-face-prompt" in path.read_text()
        }
        expected_files = {
            "goal_src/jak2/engine/process-drawable/process-taskable.gc",
            "goal_src/jak2/engine/target/mech/mech.gc",
            "goal_src/jak2/engine/target/target-turret.gc",
            "goal_src/jak2/levels/city/ctywide-obs.gc",
            "goal_src/jak2/levels/city/traffic/vehicle/vehicle-util.gc",
            "goal_src/jak2/levels/common/warp-gate.gc",
        }
        self.assertEqual(authored_files, expected_files)

        query_files = {
            path.relative_to(ROOT).as_posix(): path.read_text().count("(can-display-query?")
            for path in ROOT.glob("goal_src/jak2/**/*.gc")
            if "(can-display-query?" in path.read_text()
        }
        self.assertEqual(set(query_files), expected_files)
        self.assertEqual(sum(query_files.values()), 6)

    def test_acceptance_exit_and_non_display_paths_clear_the_lease(self) -> None:
        state_cases = (
            (self.process_taskable, "(defstate idle (process-taskable)", "(go-virtual"),
            (self.mech, "(defstate idle (mech)", "(go-virtual pickup"),
            (self.turret, "(defstate idle (base-turret)", "(go-virtual setup)"),
            (self.burning_bush, "(defstate idle (burning-bush)", "(go-virtual"),
            (self.warp, "(defstate idle (warp-gate)", "(go-virtual use"),
        )
        for source, marker, transition_marker in state_cases:
            with self.subTest(marker=marker):
                form = extract_form(source, marker)
                exit_behavior = form.index(":exit (behavior ()")
                self.assertIn(
                    "(goalpad-face-prompt-touch-clear! self)",
                    form[exit_behavior:],
                )
                acceptance = form.index("(cpad-pressed? 0 triangle)")
                acceptance_clear = form.index(
                    "(goalpad-face-prompt-touch-clear! self)",
                    acceptance,
                )
                transition = form.index(transition_marker, acceptance_clear)
                self.assertGreater(acceptance_clear, acceptance)
                self.assertGreater(transition, acceptance_clear)
                self.assertIn(
                    "(if goalpad-face-prompt?\n",
                    form,
                )

        vehicle = extract_form(
            self.vehicle,
            "(defmethod check-player-get-on ((this vehicle))",
        )
        self.assertEqual(
            vehicle.count("(goalpad-face-prompt-touch-clear! this)"),
            3,
        )
        hijack = vehicle.index("(send-event *target* 'change-mode 'pilot this 0 #f)")
        hijack_clear = vehicle.index("(goalpad-face-prompt-touch-clear! this)", hijack)
        edge_grab = vehicle.index("(send-event *target* 'pilot-edge-grab *pilot-edge-grab-info*)")
        edge_grab_clear = vehicle.index("(goalpad-face-prompt-touch-clear! this)", edge_grab)
        self.assertGreater(hijack_clear, hijack)
        self.assertGreater(edge_grab_clear, edge_grab)
        self.assertIn("(if goalpad-face-prompt?\n", vehicle)

        deactivate = extract_form(self.vehicle, "(defmethod deactivate ((this vehicle))")
        self.assertIn("(goalpad-face-prompt-touch-clear! this)", deactivate)


if __name__ == "__main__":
    unittest.main()
