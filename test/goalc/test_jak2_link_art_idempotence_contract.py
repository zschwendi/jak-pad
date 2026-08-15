#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
LOADER = ROOT / "goal_src/jak2/engine/load/loader.gc"
JOINT = ROOT / "goal_src/jak2/engine/anim/joint.gc"


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


def link_fixture(master: list[object | None], animation: object, preferred: int) -> bool:
    if animation in master:
        return True
    if preferred < len(master) and master[preferred] is None:
        master[preferred] = animation
        return True
    for index in range(len(master) - 1, -1, -1):
        if master[index] is None:
            master[index] = animation
            return True
    return False


class Jak2LinkArtIdempotenceContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.loader = LOADER.read_text()
        cls.joint = JOINT.read_text()
        cls.link_art = extract_form(cls.loader, "(defmethod link-art!")

    def test_existing_animation_is_a_successful_link(self) -> None:
        identical = extract_form(
            self.link_art,
            "(if (= (-> v1-8 data a0-5) s4-0)",
        )
        self.assertLess(identical.index("(set! s2-0 #t)"), identical.index("(goto cfg-24)"))

    def test_streamed_art_reaches_link_art_twice(self) -> None:
        relocate = extract_form(self.joint, "(defmethod relocate ((this art-group)")
        update = extract_form(self.loader, "(defmethod update ((this external-art-buffer)")
        link_file = extract_form(self.loader, "(defmethod link-file")
        self.assertIn("(link-art! this)", relocate)
        self.assertIn("(link-file this (-> this art-group))", update)
        self.assertIn("(link-art! arg0)", link_file)

    def test_synthetic_relink_keeps_one_master_slot(self) -> None:
        model = object()
        idle = object()
        streamed = object()
        master = [model, idle, None, None]

        self.assertTrue(link_fixture(master, streamed, 2))
        self.assertTrue(link_fixture(master, streamed, 2))
        self.assertEqual(master.count(streamed), 1)


if __name__ == "__main__":
    unittest.main()
