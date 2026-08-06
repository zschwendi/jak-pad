#!/usr/bin/env python3

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Tuple
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOAT = ROOT / "goal_src/jak1/levels/village1/fishermans-boat.gc"
AMBIENT = ROOT / "goal_src/jak1/engine/entity/ambient.gc"
GAME_INFO = ROOT / "goal_src/jak1/engine/game/game-info.gc"
LEVEL = ROOT / "goal_src/jak1/engine/level/level.gc"


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
    continue_level: str
    current_level: str
    boat_state: str
    source_interactable: bool
    collision_level: str
    target_grounded: bool
    orientation_synchronized: bool
    history_synchronized: bool
    title_events: Tuple[str, ...]
    seen_titles: Tuple[str, ...]
    trace: Tuple[str, ...]


def consume_level_title(
    title: Optional[str], ambient_level: str, current_level: str, seen_titles: Tuple[str, ...]
) -> Tuple[Tuple[str, ...], Tuple[str, ...]]:
    if title is None or ambient_level != current_level or title in seen_titles:
        return (), seen_titles
    return (title,), (*seen_titles, title)


def complete_ride(
    clone_results: Iterable[bool],
    destination_statuses: Iterable[str],
    source_level: str,
    destination_level: str,
    destination_continue: str,
    animated_boat: Vector,
    animated_target: Vector,
    dock: Vector,
    arrival_title: Optional[str],
    seen_titles: Tuple[str, ...] = (),
) -> RideResult:
    trace = [f"continue:{source_level}", f"owner:{source_level}"]
    for cloned in clone_results:
        trace.append(f"clone:{str(cloned).lower()}")
        if cloned:
            break
        trace.extend(("spool", "suspend"))
    else:
        raise RuntimeError("clone never became available")

    trace.append("animation")
    for status in destination_statuses:
        trace.append(f"level:{destination_level}:{status}")
        if status == "active":
            break
        trace.append("suspend")
    else:
        raise RuntimeError("destination never became resident")

    correction = tuple(dock[index] - animated_boat[index] for index in range(3))
    target = tuple(animated_target[index] + correction[index] for index in range(3))
    trace.extend(
        (
            "snap-boat",
            "reset-rbody",
            "end-clone-mode",
            "restore-target",
            "sync-orientation",
            "ground-target",
            "sync-collision-history",
            f"continue:{destination_continue}",
            "arrival-state-ready",
            "frame-boundary",
            f"owner:{destination_level}",
        )
    )
    title_events, seen_titles = consume_level_title(
        arrival_title, destination_level, destination_level, seen_titles
    )
    trace.extend(f"title:{title}" for title in title_events)
    trace.append(f"docked:{destination_level}")
    return RideResult(
        boat=dock,
        target=target,
        continue_level=destination_continue,
        current_level=destination_level,
        boat_state=f"docked:{destination_level}",
        source_interactable=False,
        collision_level=destination_level,
        target_grounded=True,
        orientation_synchronized=True,
        history_synchronized=True,
        title_events=title_events,
        seen_titles=seen_titles,
        trace=tuple(trace),
    )


class Jak1FishermansBoatRelocationContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = BOAT.read_text()
        cls.ambient = AMBIENT.read_text()
        cls.game_info = GAME_INFO.read_text()
        cls.level = LEVEL.read_text()
        cls.finalizer = extract_form(cls.source, "(defbehavior fishermans-boat-complete-ride")
        cls.leave_dock = extract_form(cls.source, "(defbehavior fishermans-boat-leave-dock?")
        cls.to_misty = extract_form(cls.source, "(defstate fishermans-boat-ride-to-misty")
        cls.to_village = extract_form(cls.source, "(defstate fishermans-boat-ride-to-village1")
        cls.docked_misty = extract_form(cls.source, "(defstate fishermans-boat-docked-misty")
        cls.docked_village = extract_form(cls.source, "(defstate fishermans-boat-docked-village")

    def test_clone_and_destination_residency_fail_closed(self) -> None:
        for ride in (self.to_misty, self.to_village):
            retry = "(while (not (and *target* (send-event *target* 'clone-anim self)))"
            self.assertEqual(ride.count(retry), 1)
            self.assertNotIn("(when (send-event *target* 'clone-anim self)", ride)
            self.assertLess(ride.index(retry), ride.index("(spool-push *art-control*"))
            self.assertLess(ride.index("(spool-push *art-control*"), ride.index("(suspend)"))
            self.assertLess(ride.index("(suspend)"), ride.index("(ja-play-spooled-anim"))

        wait = "(while (or (not *target*) (!= (level-status *level* arg0) 'active))"
        self.assertEqual(self.finalizer.count(wait), 1)
        self.assertLess(self.finalizer.index(wait), self.finalizer.index("(fishermans-boat-set-dock-point arg1)"))

        with self.assertRaisesRegex(RuntimeError, "clone never became available"):
            complete_ride(
                (False,), ("active",), "village1", "misty", "misty-start",
                (0.0, 0.0, 0.0), (1.0, 2.0, 3.0), (100.0, 0.0, 200.0), "MISTY ISLAND"
            )
        with self.assertRaisesRegex(RuntimeError, "destination never became resident"):
            complete_ride(
                (True,), ("loading", "loaded"), "village1", "misty", "misty-start",
                (0.0, 0.0, 0.0), (1.0, 2.0, 3.0), (100.0, 0.0, 200.0), "MISTY ISLAND"
            )

    def test_first_misty_ride_commits_without_death_respawn_or_second_boarding(self) -> None:
        source_dock = (-88473.6, 0.0, 425984.0)
        misty_dock = (49971.2, 0.0, 2924544.0)
        target_relative = (4096.0, 8192.0, -2048.0)
        stale_target = tuple(source_dock[index] + target_relative[index] for index in range(3))

        result = complete_ride(
            (False, True), ("loading", "active"), "village1", "misty", "misty-start",
            source_dock, stale_target, misty_dock, "MISTY ISLAND"
        )
        self.assertEqual(result.boat, misty_dock)
        expected_target = tuple(misty_dock[index] + target_relative[index] for index in range(3))
        for actual, expected in zip(result.target, expected_target):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual((result.continue_level, result.current_level), ("misty-start", "misty"))
        self.assertEqual((result.boat_state, result.collision_level), ("docked:misty", "misty"))
        self.assertTrue(result.target_grounded)
        self.assertTrue(result.orientation_synchronized)
        self.assertTrue(result.history_synchronized)
        self.assertFalse(result.source_interactable)
        self.assertEqual(result.title_events, ("MISTY ISLAND",))
        self.assertNotIn("death", result.trace)
        self.assertNotIn("respawn", result.trace)
        self.assertNotIn("second-boarding", result.trace)

    def test_atomic_completion_survives_stale_clone_exit_and_first_reevaluation(self) -> None:
        end_mode = "(send-event *target* 'end-mode)"
        restore = "(send-event *target* 'trans 'restore (-> self old-target-pos))"
        orientation = "(quaternion-copy! (-> *target* control quat-for-control) (-> *target* control quat))"
        history = "(set! (-> *target* control trans-old index quad) (-> *target* control trans quad))"
        commit = "(set-continue! *game-info* arg2)"
        frame_boundary = self.finalizer.rindex("(suspend)")

        self.assertLess(self.finalizer.index(end_mode), self.finalizer.index(restore))
        self.assertLess(self.finalizer.index(restore), self.finalizer.index(orientation))
        self.assertLess(self.finalizer.index(orientation), self.finalizer.index(history))
        self.assertLess(self.finalizer.index(history), self.finalizer.index(commit))
        self.assertLess(self.finalizer.index(commit), frame_boundary)
        for state_write in (
            "(set! (-> self waiting-for-player) #t)",
            "(set! (-> self ignition) #f)",
            "(set! (-> self anchored) #t)",
            "(set! (-> self propeller enable) #t)",
        ):
            self.assertEqual(self.finalizer.count(state_write), 1)
            self.assertLess(self.finalizer.index(state_write), frame_boundary)
            self.assertNotIn(state_write, self.to_misty)
            self.assertNotIn(state_write, self.to_village)

        result = complete_ride(
            (True,), ("active",), "village1", "misty", "misty-start",
            (0.0, 0.0, 0.0), (2.0, 3.0, 4.0), (100.0, 0.0, 200.0), "MISTY ISLAND"
        )
        self.assertLess(result.trace.index("end-clone-mode"), result.trace.index("restore-target"))
        self.assertLess(result.trace.index("continue:misty-start"), result.trace.index("frame-boundary"))
        self.assertLess(result.trace.index("frame-boundary"), result.trace.index("owner:misty"))
        self.assertLess(result.trace.index("owner:misty"), result.trace.index("title:MISTY ISLAND"))

        level_owner = extract_form(self.level, "(defmethod level-get-target-inside")
        self.assertLess(
            level_owner.index("(-> *game-info* current-continue level)"),
            level_owner.index("inside-boxes?"),
        )

    def test_collision_and_safe_ground_history_match_the_destination(self) -> None:
        required = (
            "(set! (-> self root-overlay trans-old index quad) (-> self root-overlay trans quad))",
            "(set! (-> *target* control trans-old index quad) (-> *target* control trans quad))",
            "(set! (-> *target* control last-trans-any-surf quad) (-> *target* control trans quad))",
            "(set! (-> *target* control last-trans-leaving-surf quad) (-> *target* control trans quad))",
            "(set! (-> *target* control highest-jump-mark quad) (-> *target* control trans quad))",
            "(set! (-> *target* control old-status) (-> *target* control status))",
            "(set! (-> *target* control prev-status) (-> *target* control status))",
        )
        for statement in required:
            self.assertEqual(self.finalizer.count(statement), 1)
        self.assertLess(
            self.finalizer.index("(move-to-ground (-> *target* control)"),
            self.finalizer.index("(set! (-> *target* control last-trans-any-surf quad)"),
        )
        self.assertLess(
            self.finalizer.index("(logior! (-> *target* control status)"),
            self.finalizer.index("(set! (-> *target* control old-status)"),
        )

    def test_misty_title_is_consumed_once_on_arrival_and_not_departure(self) -> None:
        title_gate = extract_form(self.ambient, "(defun ambient-level-name-hint-for-other-level?")
        title_consumer = extract_form(self.ambient, "(defun ambient-type-hint")
        title_init = extract_form(self.ambient, "(defbehavior level-hint-init-by-other")
        seen_writer = extract_form(self.game_info, "(defmethod mark-text-as-seen")
        self.assertIn("(target-level (-> *target* current-level))", title_gate)
        self.assertIn("(unless (ambient-level-name-hint-for-other-level?", title_consumer)
        self.assertIn("(mark-text-as-seen *game-info* arg0)", title_init)
        self.assertIn("(set-bit (-> this text-ids-seen)", seen_writer)

        arrival = complete_ride(
            (True,), ("active",), "village1", "misty", "misty-start",
            (0.0, 0.0, 0.0), (2.0, 3.0, 4.0), (100.0, 0.0, 200.0), "MISTY ISLAND"
        )
        departure_events, departure_seen = consume_level_title(
            "MISTY ISLAND", "misty", "misty", arrival.seen_titles
        )
        self.assertEqual(arrival.title_events, ("MISTY ISLAND",))
        self.assertEqual(departure_events, ())
        self.assertEqual(departure_seen, arrival.seen_titles)

    def test_both_directions_share_one_authoritative_completion_path(self) -> None:
        self.assertEqual(self.to_misty.count("(fishermans-boat-complete-ride 'misty 4 \"misty-start\")"), 1)
        self.assertEqual(self.to_village.count("(fishermans-boat-complete-ride 'village1 0 \"village1-hut\")"), 1)
        self.assertIn("(go fishermans-boat-docked-misty)", self.to_misty)
        self.assertIn("(go fishermans-boat-docked-village)", self.to_village)
        self.assertIn("(go fishermans-boat-ride-to-village1)", self.docked_misty)
        self.assertIn("(go fishermans-boat-ride-to-misty)", self.docked_village)
        self.assertIn(
            "(< (vector-vector-distance (target-pos 0) (-> self rbody position)) 6144.0)",
            self.leave_dock,
        )
        self.assertIn("(cpad-pressed? 0 circle)", self.leave_dock)
        self.assertIn("(process-grab? *target*)", self.leave_dock)
        self.assertEqual(self.docked_misty.count("(fishermans-boat-leave-dock?)"), 1)
        self.assertEqual(self.docked_village.count("(fishermans-boat-leave-dock?)"), 1)
        self.assertNotIn("(fishermans-boat-leave-dock?)", self.to_misty)
        self.assertNotIn("(fishermans-boat-leave-dock?)", self.to_village)

        village_dock = (-88473.6, 0.0, 425984.0)
        misty_dock = (49971.2, 0.0, 2924544.0)
        outbound = complete_ride(
            (True,), ("active",), "village1", "misty", "misty-start",
            village_dock, (-84377.6, 8192.0, 423936.0), misty_dock, "MISTY ISLAND"
        )
        inbound = complete_ride(
            (True,), ("loaded", "active"), "misty", "village1", "village1-hut",
            misty_dock, (51000.0, 8000.0, 2923000.0), village_dock, "SANDOVER VILLAGE",
            ("SANDOVER VILLAGE",)
        )
        self.assertEqual((outbound.boat_state, inbound.boat_state), ("docked:misty", "docked:village1"))
        self.assertEqual((outbound.collision_level, inbound.collision_level), ("misty", "village1"))
        self.assertFalse(outbound.source_interactable)
        self.assertFalse(inbound.source_interactable)
        self.assertEqual(inbound.title_events, ())


if __name__ == "__main__":
    unittest.main()
