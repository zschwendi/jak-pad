#!/usr/bin/env python3

from dataclasses import dataclass
from pathlib import Path
from typing import Optional
import unittest


ROOT = Path(__file__).resolve().parents[2]
BASEBUTTON = ROOT / "goal_src/jak1/engine/common-obs/basebutton.gc"
VILLAGEP = ROOT / "goal_src/jak1/levels/village_common/villagep-obs.gc"

UNAVAILABLE = 0
CIRCLE_USE_AVAILABLE = 1
DESTINATION_LIST_ACTIVE = 2
TRANSITIONING = 3


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


def permitted_mask(min_slot: int, max_slot: int, level_slot: int) -> int:
    return sum(1 << slot for slot in range(5) if min_slot <= slot <= max_slot and slot != level_slot)


def next_slot_up(min_slot: int, max_slot: int, level_slot: int, slot: int) -> int:
    result = slot + 1
    if result > max_slot:
        result = min_slot
    if result == level_slot:
        result += 1
    if result > max_slot:
        result = min_slot
    return result


def next_slot_down(min_slot: int, max_slot: int, level_slot: int, slot: int) -> int:
    result = slot - 1
    if result < min_slot:
        result = max_slot
    if result == level_slot:
        result -= 1
    if result < min_slot:
        result = max_slot
    return result


@dataclass
class SnapshotModel:
    owner: Optional[object] = None
    sequence: int = 0
    state: int = UNAVAILABLE
    selected_slot: int = -1
    permitted: int = 0
    moving: int = 0

    def publish(self, owner: object, state: int, selected_slot: int, permitted: int, moving: int) -> None:
        payload = (state, selected_slot, permitted, moving)
        if self.owner is owner and payload == (self.state, self.selected_slot, self.permitted, self.moving):
            return
        self.owner = owner
        self.sequence += 1
        self.state, self.selected_slot, self.permitted, self.moving = payload
        self.sequence += 1

    def clear(self, owner: object) -> None:
        if self.owner is not owner:
            return
        self.sequence += 1
        self.owner = None
        self.state = UNAVAILABLE
        self.selected_slot = -1
        self.permitted = 0
        self.moving = 0
        self.sequence += 1


class Jak1WarpGateTouchContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.basebutton = BASEBUTTON.read_text()
        cls.villagep = VILLAGEP.read_text()

    def test_fixed_snapshot_abi_and_stable_symbols(self) -> None:
        expected_fields = (
            "(revision         int32 :offset-assert 4)",
            "(sequence         int32 :offset-assert 8)",
            "(state            int32 :offset-assert 12)",
            "(selected-slot    int32 :offset-assert 16)",
            "(permitted-mask   int32 :offset-assert 20)",
            "(selection-moving int32 :offset-assert 24)",
        )
        for field in expected_fields:
            self.assertIn(field, self.basebutton)
        self.assertIn(":size-assert #x1c", self.basebutton)
        self.assertIn("(define *goalpad-warp-gate-touch-snapshot*", self.basebutton)
        self.assertIn("'goalpad-warp-gate-touch-snapshot", self.basebutton)
        self.assertIn(":revision 1", self.basebutton)
        self.assertIn(":state 0", self.basebutton)
        self.assertNotIn("(-> *goalpad-warp-gate-touch-snapshot* revision)", self.basebutton)
        for name, value in (
            ("unavailable", UNAVAILABLE),
            ("circle-use-available", CIRCLE_USE_AVAILABLE),
            ("destination-list-active", DESTINATION_LIST_ACTIVE),
            ("transitioning", TRANSITIONING),
        ):
            self.assertIn(f"({name} {value})", self.basebutton)

    def test_publication_is_owner_aware_and_semantic_change_only(self) -> None:
        publish = extract_form(self.basebutton, "(defun goalpad-warp-gate-touch-publish!")
        clear = extract_form(self.basebutton, "(defun goalpad-warp-gate-touch-clear!")
        for comparison in ("state", "selected-slot", "permitted-mask", "selection-moving"):
            self.assertIn(f"(-> *goalpad-warp-gate-touch-snapshot* {comparison})", publish)
        self.assertIn("(= *goalpad-warp-gate-touch-owner* owner)", publish)
        self.assertEqual(publish.count("(+! (-> *goalpad-warp-gate-touch-snapshot* sequence) 1)"), 2)
        self.assertIn("(when (= *goalpad-warp-gate-touch-owner* owner)", clear)
        self.assertEqual(clear.count("(+! (-> *goalpad-warp-gate-touch-snapshot* sequence) 1)"), 2)
        self.assertIn("(set! *goalpad-warp-gate-touch-owner* (the-as warp-gate #f))", clear)
        self.assertIn("(set! (-> *goalpad-warp-gate-touch-snapshot* state) 0)", clear)

    def test_circle_availability_matches_the_exact_prompt_branch(self) -> None:
        idle = extract_form(self.villagep, "(defstate idle (warp-gate)")
        self.assertEqual(self.villagep.count("(goalpad-warp-gate-touch-state circle-use-available)"), 1)
        availability = idle.index("(goalpad-warp-gate-touch-state circle-use-available)")
        self.assertLess(idle.index("(hud-hidden?)"), availability)
        self.assertLess(idle.index("(can-grab-display? self)"), availability)
        self.assertLess(idle.index("(time-elapsed? (-> self state-time) (seconds 0.1))"), availability)
        self.assertLess(availability, idle.index("(cpad-pressed? 0 circle)"))
        self.assertIn("(set! touch-available? #t)", idle)
        self.assertIn("(when (not touch-available?) (goalpad-warp-gate-touch-clear! self))", idle)
        self.assertIn("(go-virtual active)", idle)

    def test_list_transition_and_lifecycle_publication_points(self) -> None:
        use = extract_form(self.basebutton, "(defstate use (warp-gate)")
        active = extract_form(self.villagep, "(defstate active (warp-gate)")
        hidden = extract_form(self.villagep, "(defstate hidden (warp-gate)")
        deactivate = extract_form(self.villagep, "(defmethod deactivate ((this warp-gate))")
        self.assertEqual(active.count("(goalpad-warp-gate-touch-state destination-list-active)"), 2)
        self.assertIn("(if s5-0 1 0)", active)
        self.assertIn("(when (zero? s2-0)", active)
        self.assertIn("(cpad-pressed? 0 right)", active)
        self.assertIn("(cpad-pressed? 0 left)", active)
        self.assertIn("(cpad-pressed? 0 circle)", active)
        self.assertIn("(cpad-pressed? 0 triangle)", active)
        self.assertIn("(= (-> self next-state name) 'use)", active)
        self.assertIn("(goalpad-warp-gate-touch-state transitioning)", active)
        self.assertIn("(go-virtual idle)", active)
        self.assertIn("(goalpad-warp-gate-touch-clear! self)", use)
        self.assertEqual(hidden.count("(goalpad-warp-gate-touch-clear! self)"), 2)
        self.assertIn("(goalpad-warp-gate-touch-clear! this)", deactivate)
        self.assertIn("((method-of-type process-drawable deactivate) this)", deactivate)

    def test_owner_generation_acquisition_loss_and_reentry(self) -> None:
        first_gate = object()
        second_gate = object()
        snapshot = SnapshotModel()

        snapshot.publish(first_gate, CIRCLE_USE_AVAILABLE, -1, 0, 0)
        self.assertEqual(snapshot.sequence, 2)
        snapshot.publish(first_gate, CIRCLE_USE_AVAILABLE, -1, 0, 0)
        self.assertEqual(snapshot.sequence, 2)

        snapshot.publish(second_gate, CIRCLE_USE_AVAILABLE, -1, 0, 0)
        self.assertEqual(snapshot.sequence, 4)
        snapshot.clear(first_gate)
        self.assertEqual((snapshot.owner, snapshot.sequence), (second_gate, 4))

        snapshot.clear(second_gate)
        self.assertEqual((snapshot.state, snapshot.selected_slot, snapshot.permitted, snapshot.moving), (0, -1, 0, 0))
        self.assertEqual(snapshot.sequence, 6)
        snapshot.publish(second_gate, CIRCLE_USE_AVAILABLE, -1, 0, 0)
        snapshot.publish(second_gate, DESTINATION_LIST_ACTIVE, 1, 0b11110, 0)
        snapshot.publish(second_gate, DESTINATION_LIST_ACTIVE, 1, 0b11110, 1)
        snapshot.publish(second_gate, TRANSITIONING, -1, 0, 0)
        self.assertEqual(snapshot.sequence, 14)

    def test_slot_wrap_and_permitted_masks(self) -> None:
        mask_source = extract_form(self.villagep, "(defun goalpad-warp-gate-touch-permitted-mask")
        up_source = extract_form(self.villagep, "(defun get-next-slot-up")
        down_source = extract_form(self.villagep, "(defun get-next-slot-down")
        self.assertIn("(>= slot (-> gate min-slot))", mask_source)
        self.assertIn("(<= slot (-> gate max-slot))", mask_source)
        self.assertIn("(!= slot (-> gate level-slot))", mask_source)
        self.assertIn("(logior! mask (ash 1 slot))", mask_source)
        self.assertIn("(if (= v0-0 (-> arg0 level-slot)) (+! v0-0 1))", up_source)
        self.assertIn("(if (= v0-0 (-> arg0 level-slot)) (+! v0-0 -1))", down_source)

        for level_slot, expected_mask, expected_first in (
            (0, 0b00010, 1),
            (1, 0b00001, 0),
            (2, 0b00011, 0),
            (3, 0b00111, 0),
            (4, 0b01111, 0),
        ):
            max_slot = max(1, level_slot)
            mask = permitted_mask(0, max_slot, level_slot)
            selected = next_slot_up(0, max_slot, level_slot, level_slot - 1)
            self.assertEqual(mask, expected_mask)
            self.assertEqual(selected, expected_first)
            self.assertNotEqual(mask & (1 << selected), 0)

        for max_slot in range(1, 5):
            for level_slot in range(max_slot + 1):
                mask = permitted_mask(0, max_slot, level_slot)
                up = next_slot_up(0, max_slot, level_slot, level_slot - 1)
                down = next_slot_down(0, max_slot, level_slot, level_slot + 1)
                for _ in range(max_slot):
                    self.assertNotEqual(mask & (1 << up), 0)
                    self.assertNotEqual(mask & (1 << down), 0)
                    up = next_slot_up(0, max_slot, level_slot, up)
                    down = next_slot_down(0, max_slot, level_slot, down)

        self.assertEqual(next_slot_up(1, 4, 2, 4), 1)
        self.assertEqual(next_slot_up(1, 4, 2, 1), 3)
        self.assertEqual(next_slot_down(1, 4, 2, 1), 4)
        self.assertEqual(next_slot_down(1, 4, 2, 3), 1)


if __name__ == "__main__":
    unittest.main()
