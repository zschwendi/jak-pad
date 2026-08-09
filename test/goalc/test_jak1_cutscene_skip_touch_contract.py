#!/usr/bin/env python3

from dataclasses import dataclass
from pathlib import Path
from typing import Optional
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROCESS_TASKABLE = ROOT / "goal_src/jak1/engine/common-obs/process-taskable.gc"
SAGE_FINALBOSS = ROOT / "goal_src/jak1/levels/finalboss/sage-finalboss.gc"

UNAVAILABLE = 0
TRIANGLE_QUERY_AVAILABLE = 1
CREDITS_TRIANGLE_AVAILABLE = 2


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
    sequence: int = 0
    state: int = UNAVAILABLE
    heartbeat: int = 0

    def publish(self, owner: object, state: int) -> None:
        if self.owner is owner and self.state == state:
            self.heartbeat += 1
            return
        self.sequence += 1
        self.owner = owner
        self.state = state
        self.heartbeat = 1
        self.sequence += 1

    def clear(self, owner: object) -> None:
        if self.owner is not owner:
            return
        self.sequence += 1
        self.owner = None
        self.state = UNAVAILABLE
        self.heartbeat = 0
        self.sequence += 1


class Jak1CutsceneSkipTouchContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = PROCESS_TASKABLE.read_text()
        cls.sage_finalboss = SAGE_FINALBOSS.read_text()

    def test_fixed_revisioned_owner_snapshot_abi(self) -> None:
        for field in (
            "(revision  int32  :offset-assert 4)",
            "(sequence  int32  :offset-assert 8)",
            "(state     int32  :offset-assert 12)",
            "(owner     handle :offset-assert 16)",
            "(heartbeat int32  :offset-assert 24)",
        ):
            self.assertIn(field, self.source)
        self.assertIn(":size-assert #x1c", self.source)
        self.assertIn(":revision 2", self.source)
        self.assertIn("(unavailable 0)", self.source)
        self.assertIn("(triangle-query-available 1)", self.source)
        self.assertIn("(credits-triangle-available 2)", self.source)
        self.assertNotIn(
            "(-> *goalpad-cutscene-skip-touch-snapshot* revision)",
            extract_form(self.source, "(defun goalpad-cutscene-skip-touch-publish!"),
        )

    def test_publication_generation_and_clear_are_owner_aware(self) -> None:
        publish = extract_form(
            self.source, "(defun goalpad-cutscene-skip-touch-publish!"
        )
        clear = extract_form(
            self.source, "(defun goalpad-cutscene-skip-touch-clear!"
        )
        self.assertIn("(= *goalpad-cutscene-skip-touch-owner* owner)", publish)
        self.assertIn(
            "(= (-> *goalpad-cutscene-skip-touch-snapshot* state) state-value)",
            publish,
        )
        self.assertEqual(
            publish.count(
                "(+! (-> *goalpad-cutscene-skip-touch-snapshot* sequence) 1)"
            ),
            2,
        )
        self.assertIn(
            "(+! (-> *goalpad-cutscene-skip-touch-snapshot* heartbeat) 1)",
            publish,
        )
        self.assertIn("(process->handle owner)", publish)
        self.assertLess(
            publish.index(
                "(+! (-> *goalpad-cutscene-skip-touch-snapshot* sequence) 1)"
            ),
            publish.index("(set! *goalpad-cutscene-skip-touch-owner* owner)"),
        )
        self.assertIn(
            "(when (= *goalpad-cutscene-skip-touch-owner* owner)", clear
        )
        self.assertEqual(
            clear.count(
                "(+! (-> *goalpad-cutscene-skip-touch-snapshot* sequence) 1)"
            ),
            2,
        )
        self.assertIn("(set! (-> *goalpad-cutscene-skip-touch-snapshot* state) 0)", clear)
        self.assertIn("(set! (-> *goalpad-cutscene-skip-touch-snapshot* heartbeat) 0)", clear)

    def test_only_source_query_conditions_publish_and_response_clears(self) -> None:
        query = extract_form(
            self.source, "(defun goalpad-cutscene-skip-touch-query"
        )
        conditions = (
            "(= (-> owner skippable) #t)",
            "(= (-> *pc-settings* skip-movies?) #t)",
            "(= (-> owner query decision) 'undecided)",
        )
        for condition in conditions:
            self.assertIn(condition, query)
        self.assertLess(
            query.index("(goalpad-cutscene-skip-touch-publish!"),
            query.index("(get-response (-> owner query))"),
        )
        self.assertIn(
            "(goalpad-cutscene-skip-touch-state triangle-query-available)",
            query,
        )
        self.assertIn("(when (!= response 'undecided)", query)
        self.assertEqual(query.count("(goalpad-cutscene-skip-touch-clear! owner)"), 2)

        for unsupported in (
            "send-event",
            "go-virtual",
            "process-spawn",
            "othercam",
            "save",
            "load",
            "warp",
            "boat",
        ):
            self.assertNotIn(unsupported, query)

    def test_central_play_anim_callback_return_and_exit_own_the_lifecycle(self) -> None:
        code = extract_form(
            self.source, "(defbehavior process-taskable-play-anim-code"
        )
        exit_behavior = extract_form(
            self.source, "(defbehavior process-taskable-play-anim-exit"
        )
        self.assertEqual(
            self.source.count("(goalpad-cutscene-skip-touch-query arg0)"), 1
        )
        callback = code.index("(goalpad-cutscene-skip-touch-query arg0)")
        return_clear = code.index(
            "(goalpad-cutscene-skip-touch-clear! self)", callback
        )
        self.assertLess(callback, return_clear)
        self.assertIn("(goalpad-cutscene-skip-touch-clear! self)", exit_behavior)

    def test_owner_replacement_heartbeat_and_stale_owner_clear_model(self) -> None:
        first = object()
        second = object()
        snapshot = SnapshotModel()

        snapshot.publish(first, TRIANGLE_QUERY_AVAILABLE)
        self.assertEqual((snapshot.sequence, snapshot.heartbeat), (2, 1))
        snapshot.publish(first, TRIANGLE_QUERY_AVAILABLE)
        self.assertEqual((snapshot.sequence, snapshot.heartbeat), (2, 2))
        snapshot.publish(first, CREDITS_TRIANGLE_AVAILABLE)
        self.assertEqual(
            (snapshot.owner, snapshot.state, snapshot.sequence, snapshot.heartbeat),
            (first, CREDITS_TRIANGLE_AVAILABLE, 4, 1),
        )
        snapshot.publish(first, CREDITS_TRIANGLE_AVAILABLE)
        self.assertEqual((snapshot.sequence, snapshot.heartbeat), (4, 2))
        snapshot.publish(second, CREDITS_TRIANGLE_AVAILABLE)
        self.assertEqual((snapshot.owner, snapshot.sequence, snapshot.heartbeat), (second, 6, 1))
        snapshot.clear(first)
        self.assertEqual((snapshot.owner, snapshot.sequence), (second, 6))
        snapshot.clear(second)
        self.assertEqual(
            (snapshot.owner, snapshot.sequence, snapshot.state, snapshot.heartbeat),
            (None, 8, UNAVAILABLE, 0),
        )

    def test_end_credits_publish_only_while_incomplete_and_clear_every_exit(self) -> None:
        credits = extract_form(
            self.sage_finalboss, "(defstate sage-finalboss-credits"
        )
        loop = extract_form(credits, "(until (or s5-0 skip-credits?)")
        clear = "(goalpad-cutscene-skip-touch-clear! self)"
        draw = loop.index("(set! s5-0 (draw-end-credits (the int f30-0)))")
        natural_clear = loop.index(clear)
        gate = loop.index("(or *cheat-mode* (-> *pc-settings* speedrunner-mode?))")
        publish = loop.index("(goalpad-cutscene-skip-touch-publish!")

        self.assertLess(draw, natural_clear)
        self.assertLess(natural_clear, gate)
        self.assertLess(gate, publish)
        self.assertIn(
            "(goalpad-cutscene-skip-touch-state credits-triangle-available)",
            loop,
        )
        self.assertEqual(loop.count("(cpad-pressed? 0 triangle)"), 1)
        self.assertEqual(loop.count("(set! s5-0"), 1)
        self.assertNotIn("(set! s5-0 #t)", loop)
        self.assertNotIn("skip-movies?", credits)
        self.assertNotIn("go-virtual", loop)
        self.assertEqual(loop.count(clear), 3)

        triangle = loop.index("(cpad-pressed? 0 triangle)")
        skip_flag = loop.index("(set! skip-credits? #t)", triangle)
        consumed_clear = loop.index(clear, skip_flag)
        self.assertLess(triangle, skip_flag)
        self.assertLess(skip_flag, consumed_clear)

        loop_start = credits.index("(until (or s5-0 skip-credits?)")
        loop_end = loop_start + len(loop)
        mask_restore = credits.index("(set! (-> self mask) gp-0)")
        self.assertIn(clear, credits[loop_end:mask_restore])
        self.assertIn(clear, credits[credits.index(":exit"):credits.index(":code")])


if __name__ == "__main__":
    unittest.main()
