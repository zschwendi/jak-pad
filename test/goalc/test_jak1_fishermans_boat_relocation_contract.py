#!/usr/bin/env python3

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Tuple
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOAT = ROOT / "goal_src/jak1/levels/village1/fishermans-boat.gc"
AMBIENT = ROOT / "goal_src/jak1/engine/entity/ambient.gc"


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


Vector = Tuple[float, float, float]


@dataclass(frozen=True)
class RideResult:
    boat: Vector
    target: Vector
    trace: Tuple[str, ...]


def complete_ride(
    clone_results: Iterable[bool],
    destination_statuses: Iterable[str],
    animated_boat: Vector,
    animated_target: Vector,
    dock: Vector,
) -> RideResult:
    trace = []
    for cloned in clone_results:
        trace.append(f"clone:{str(cloned).lower()}")
        if cloned:
            break
        trace.extend(("spool", "suspend"))
    else:
        raise RuntimeError("clone never became available")

    trace.append("animation")
    for status in destination_statuses:
        trace.append(f"level:{status}")
        if status == "active":
            break
        trace.append("suspend")
    else:
        raise RuntimeError("destination never became resident")

    correction = tuple(dock[index] - animated_boat[index] for index in range(3))
    target = tuple(animated_target[index] + correction[index] for index in range(3))
    trace.extend(("snap-boat", "reset-rbody", "restore-target", "frame-boundary", "continue", "docked"))
    return RideResult(dock, target, tuple(trace))


class Jak1FishermansBoatRelocationContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = BOAT.read_text()
        cls.ambient = AMBIENT.read_text()
        cls.finalizer = extract_form(cls.source, "(defbehavior fishermans-boat-complete-ride")
        cls.to_misty = extract_form(cls.source, "(defstate fishermans-boat-ride-to-misty")
        cls.to_village = extract_form(cls.source, "(defstate fishermans-boat-ride-to-village1")

    def test_clone_retry_cannot_skip_streaming_animation(self) -> None:
        for ride in (self.to_misty, self.to_village):
            retry = "(while (not (and *target* (send-event *target* 'clone-anim self)))"
            self.assertEqual(ride.count(retry), 1)
            self.assertNotIn("(when (send-event *target* 'clone-anim self)", ride)
            self.assertLess(ride.index(retry), ride.index("(spool-push *art-control*"))
            self.assertLess(ride.index("(spool-push *art-control*"), ride.index("(suspend)"))
            self.assertLess(ride.index("(suspend)"), ride.index("(ja-play-spooled-anim"))

        result = complete_ride(
            (False, True),
            ("loading", "active"),
            (0.0, 0.0, 0.0),
            (2.0, 3.0, 4.0),
            (100.0, 0.0, 200.0),
        )
        self.assertLess(result.trace.index("clone:false"), result.trace.index("spool"))
        self.assertLess(result.trace.index("clone:true"), result.trace.index("animation"))
        self.assertLess(result.trace.index("animation"), result.trace.index("level:loading"))
        self.assertLess(result.trace.index("level:active"), result.trace.index("snap-boat"))

    def test_completion_waits_for_residency_then_commits_world_state(self) -> None:
        wait = "(while (!= (level-status *level* arg0) 'active)"
        self.assertEqual(self.finalizer.count(wait), 1)
        self.assertLess(self.finalizer.index(wait), self.finalizer.index("(fishermans-boat-set-dock-point arg1)"))
        self.assertLess(
            self.finalizer.index("(set! (-> self root-overlay trans x) (-> self dock-point x))"),
            self.finalizer.index("(fishermans-boat-reset-physics)"),
        )
        self.assertLess(
            self.finalizer.index("(fishermans-boat-set-path-point arg1)"),
            self.finalizer.index("(fishermans-boat-next-path-point)"),
        )
        self.assertLess(
            self.finalizer.index("(fishermans-boat-next-path-point)"),
            self.finalizer.index("(forward-up-nopitch->quaternion"),
        )
        self.assertLess(
            self.finalizer.index("(fishermans-boat-reset-physics)"),
            self.finalizer.index("(send-event *target* 'trans 'restore (-> self old-target-pos))"),
        )
        self.assertLess(
            self.finalizer.index("(send-event *target* 'trans 'restore (-> self old-target-pos))"),
            self.finalizer.rindex("(suspend)"),
        )
        self.assertLess(self.finalizer.rindex("(suspend)"), self.finalizer.index("(set-continue! *game-info* arg2)"))

        with self.assertRaisesRegex(RuntimeError, "destination never became resident"):
            complete_ride(
                (True,),
                ("loading", "failed"),
                (0.0, 0.0, 0.0),
                (1.0, 2.0, 3.0),
                (100.0, 0.0, 200.0),
            )

    def test_stale_animation_joint_is_corrected_for_boat_and_target(self) -> None:
        source_dock = (-88473.6, 0.0, 425984.0)
        misty_dock = (49971.2, 0.0, 2924544.0)
        target_relative = (4096.0, 8192.0, -2048.0)
        stale_target = tuple(source_dock[index] + target_relative[index] for index in range(3))

        result = complete_ride((True,), ("active",), source_dock, stale_target, misty_dock)
        self.assertEqual(result.boat, misty_dock)
        expected_target = tuple(misty_dock[index] + target_relative[index] for index in range(3))
        for actual, expected in zip(result.target, expected_target):
            self.assertAlmostEqual(actual, expected)
        self.assertLess(result.trace.index("restore-target"), result.trace.index("frame-boundary"))
        self.assertLess(result.trace.index("frame-boundary"), result.trace.index("continue"))
        self.assertLess(result.trace.index("continue"), result.trace.index("docked"))

    def test_arrival_relocates_target_before_destination_title_gate_rechecks(self) -> None:
        title_gate = extract_form(self.ambient, "(defun ambient-level-name-hint-for-other-level?")
        self.assertIn("(target-level (-> *target* current-level))", title_gate)
        restore = "(send-event *target* 'trans 'restore (-> self old-target-pos))"
        self.assertLess(self.finalizer.index(restore), self.finalizer.rindex("(suspend)"))
        self.assertLess(self.finalizer.rindex("(suspend)"), self.finalizer.index("(set-continue! *game-info* arg2)"))
        self.assertEqual(self.to_misty.count("(fishermans-boat-complete-ride 'misty 4 \"misty-start\")"), 1)

    def test_both_directions_use_the_same_authoritative_finalizer(self) -> None:
        self.assertEqual(self.to_misty.count("(fishermans-boat-complete-ride 'misty 4 \"misty-start\")"), 1)
        self.assertEqual(self.to_village.count("(fishermans-boat-complete-ride 'village1 0 \"village1-hut\")"), 1)
        self.assertLess(
            self.to_misty.index("(fishermans-boat-complete-ride 'misty 4 \"misty-start\")"),
            self.to_misty.index("(go fishermans-boat-docked-misty)"),
        )
        self.assertLess(
            self.to_village.index("(fishermans-boat-complete-ride 'village1 0 \"village1-hut\")"),
            self.to_village.index("(go fishermans-boat-docked-village)"),
        )

        village_dock = (-88473.6, 0.0, 425984.0)
        misty_dock = (49971.2, 0.0, 2924544.0)
        result = complete_ride((True,), ("loaded", "active"), misty_dock, (51000.0, 8000.0, 2923000.0), village_dock)
        self.assertEqual(result.boat, village_dock)
        for actual, expected in zip(result.target, (-87444.8, 8000.0, 424440.0)):
            self.assertAlmostEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
