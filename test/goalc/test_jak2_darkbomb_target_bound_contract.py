#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
TARGET_DARKJAK = ROOT / "goal_src/jak2/engine/target/target-darkjak.gc"
TARGET_LIMIT = 128


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


def collect_bomb_targets(candidates: range) -> list[int]:
    targets: list[int] = []
    for candidate in candidates:
        if len(targets) < TARGET_LIMIT:
            targets.append(candidate)
    return targets


class Jak2DarkBombTargetBoundContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = TARGET_DARKJAK.read_text()
        cls.bomb = extract_form(source, "(defstate target-darkjak-bomb1 (target)")
        cls.collection = extract_form(cls.bomb, "(countdown (s5-0 sv-40)")

    def test_write_is_guarded_before_the_fixed_array_is_indexed(self) -> None:
        guard = "(when (and (< sv-48 128)"
        write = "(set! (-> sv-60 sv-48) (process->handle v1-53))"
        increment = "(+! sv-48 1)"

        self.assertEqual(self.bomb.count("(new 'static 'array handle 128"), 1)
        self.assertEqual(self.collection.count(guard), 1)
        self.assertEqual(self.collection.count(write), 1)
        self.assertLess(self.collection.index(guard), self.collection.index(write))
        self.assertLess(self.collection.index(write), self.collection.index(increment))
        self.assertNotIn("(if (< 128 sv-48)", self.collection)

    def test_the_129th_eligible_target_is_not_written(self) -> None:
        targets = collect_bomb_targets(range(TARGET_LIMIT + 1))

        self.assertEqual(len(targets), TARGET_LIMIT)
        self.assertEqual(targets[0], 0)
        self.assertEqual(targets[-1], TARGET_LIMIT - 1)
        self.assertNotIn(TARGET_LIMIT, targets)


if __name__ == "__main__":
    unittest.main()
