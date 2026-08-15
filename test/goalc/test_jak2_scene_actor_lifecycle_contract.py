#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCENE = ROOT / "goal_src/jak2/engine/scene/scene.gc"
RUNTIME = ROOT / "game/kernel/core/jak2_runtime.cpp"
RUNTIME_HEADER = ROOT / "game/kernel/core/jak2_runtime.h"
READER = ROOT / "game/kernel/core/jak2_runtime_metrics_reader.h"


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


class Jak2SceneActorLifecycleContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.scene = SCENE.read_text()
        cls.runtime = RUNTIME.read_text()
        cls.runtime_header = RUNTIME_HEADER.read_text()
        cls.reader = READER.read_text()

    def test_goal_flags_match_the_numeric_tail_abi(self) -> None:
        for name, goal_name, bit in (
            ("SPAWN_ATTEMPTED", "spawn-attempted", 0),
            ("POOL_ALLOCATED", "pool-allocated", 1),
            ("DRAW_CONTROL", "draw-control", 2),
            ("JOINT_CONTROL", "joint-control", 3),
            ("MERC_GEOMETRY", "merc-geometry", 4),
        ):
            self.assertIn(f"({goal_name} {bit})", self.scene)
            self.assertIn(
                f"GOAL_JAK2_SCENE_ACTOR_{name} = 1u << {bit}",
                self.runtime_header,
            )

    def test_reset_is_bounded_and_sticky(self) -> None:
        reset = extract_form(self.scene, "(defun pc-scene-actor-diagnostics-reset")
        self.assertIn("PC_SCENE_ACTOR_DIAGNOSTIC_MAX 8", self.scene)
        self.assertIn("PC_SCENE_ACTOR_DIAGNOSTIC_FIELDS 4", self.scene)
        self.assertIn("PC_SCENE_ACTOR_DIAGNOSTIC_CAPACITY 32", self.scene)
        self.assertIn("(+! *pc-scene-actor-sequence* 1)", reset)
        self.assertIn("(set! *pc-scene-actor-scene-name* (-> current-scene anim))", reset)
        self.assertIn("(-> current-scene actor length)", reset)
        self.assertIn("PC_SCENE_ACTOR_DIAGNOSTIC_MAX", reset)
        self.assertIn("#xffffffff", reset)

    def test_spawn_lifecycle_observes_exact_boundaries(self) -> None:
        spawn = extract_form(self.scene, "(defmethod scene-actor-method-9")
        draw = extract_form(self.scene, "(defmethod scene-player-method-25")
        initialize = extract_form(self.scene, "(defmethod scene-player-method-23")

        self.assertIn("(pc-scene-actor-diagnostics-reset gp-0)", initialize)
        self.assertIn("(pc-scene-actor-diagnostic-flag spawn-attempted)", draw)
        self.assertLess(
            draw.index("(pc-scene-actor-diagnostic-flag spawn-attempted)"),
            draw.index("(scene-actor-method-9 s3-0 this)"),
        )
        self.assertLess(
            spawn.index("(get-process *default-dead-pool* manipy #x4000)"),
            spawn.index("(pc-scene-actor-diagnostic-flag pool-allocated)"),
        )
        self.assertIn("(pc-scene-actor-note-process s4-0", draw)

    def test_draw_and_skeleton_snapshot_uses_existing_fields(self) -> None:
        note = extract_form(self.scene, "(defun pc-scene-actor-note-process")
        for fact in (
            "(-> proc draw)",
            "(-> proc skel)",
            "(-> draw level-index)",
            "(-> draw mgeo num-joints)",
            "(-> skel status)",
        ):
            self.assertIn(fact, note)

    def test_runtime_reader_is_numeric_bounded_and_retained(self) -> None:
        for symbol in (
            "*pc-scene-actor-sequence*",
            "*pc-scene-actor-scene-name*",
            "*pc-scene-actor-count*",
            "*pc-scene-actor-total-count*",
            "*pc-scene-actor-data*",
        ):
            self.assertIn(symbol, self.runtime)
        self.assertIn("fnv64(data + data_address, length)", self.reader)
        self.assertIn("merc_pris_bucket(actor.level_index)", self.reader)
        self.assertIn("retain_scene_actor_diagnostics", self.reader)
        self.assertIn("scene_actor_diagnostics_valid", self.runtime_header)
        self.assertLess(
            self.runtime_header.index("scene_wait_art_gui_status;"),
            self.runtime_header.index("scene_actor_diagnostics_valid;"),
        )


if __name__ == "__main__":
    unittest.main()
