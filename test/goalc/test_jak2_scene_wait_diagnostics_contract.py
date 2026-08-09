#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCENE = ROOT / "goal_src/jak2/engine/scene/scene.gc"
LOADER = ROOT / "goal_src/jak2/engine/load/loader.gc"
RUNTIME = ROOT / "game/kernel/core/jak2_runtime.cpp"
RUNTIME_HEADER = ROOT / "game/kernel/core/jak2_runtime.h"
SOUND_HEADER = ROOT / "game/kernel/core/sound_rpc_jak2.h"

NONE = 0
PROGRESS = 1
TARGET_GRAB = 2
SETTING_OR_ENTRY_GUI = 3
GROUND_TIME = 4
LEVELS = 5
ART_FILE = 6
ART_GUI = 7
READY = 8

GUI_UNKNOWN = 0
GUI_PENDING = 1
GUI_READY = 2
GUI_ACTIVE = 3
GUI_HIDE = 4
GUI_STOP = 5


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


def final_art_gate(file_status: str, gui_status: int) -> int:
    if file_status not in ("active", "locked"):
        return ART_FILE
    if gui_status not in (GUI_READY, GUI_ACTIVE):
        return ART_GUI
    return READY


class Jak2SceneWaitDiagnosticsContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.scene = SCENE.read_text()
        cls.loader = LOADER.read_text()
        cls.runtime = RUNTIME.read_text()
        cls.runtime_header = RUNTIME_HEADER.read_text()
        cls.sound_header = SOUND_HEADER.read_text()
        cls.wait = extract_form(cls.scene, "(defstate wait (scene-player)")

    def test_gate_values_are_stable_and_match_the_native_api(self) -> None:
        for name, value in (
            ("none", NONE),
            ("progress", PROGRESS),
            ("target-grab", TARGET_GRAB),
            ("setting-or-entry-gui", SETTING_OR_ENTRY_GUI),
            ("ground-time", GROUND_TIME),
            ("levels", LEVELS),
            ("art-file", ART_FILE),
            ("art-gui", ART_GUI),
            ("ready", READY),
        ):
            self.assertIn(f"({name} {value})", self.scene)

        for name, value in (
            ("NONE", NONE),
            ("PROGRESS", PROGRESS),
            ("TARGET_GRAB", TARGET_GRAB),
            ("SETTING_OR_ENTRY_GUI", SETTING_OR_ENTRY_GUI),
            ("GROUND_TIME", GROUND_TIME),
            ("LEVELS", LEVELS),
            ("ART_FILE", ART_FILE),
            ("ART_GUI", ART_GUI),
            ("READY", READY),
        ):
            self.assertIn(f"GOAL_JAK2_SCENE_WAIT_{name} = {value}", self.runtime_header)

    def test_wait_entry_resets_every_diagnostic(self) -> None:
        enter = extract_form(self.wait, ":enter (behavior")
        for field in (
            "*pc-scene-wait-gate*",
            "*pc-scene-wait-entry-gui-id*",
            "*pc-scene-wait-entry-gui-status*",
            "*pc-scene-wait-art-file-status*",
            "*pc-scene-wait-art-gui-id*",
            "*pc-scene-wait-art-gui-channel*",
            "*pc-scene-wait-art-gui-action*",
            "*pc-scene-wait-art-gui-status*",
        ):
            self.assertIn(f"(set! {field}", enter)

    def test_existing_waits_publish_the_gate_that_still_suspends(self) -> None:
        code = extract_form(self.wait, ":code (behavior")
        for gate in (
            "progress",
            "target-grab",
            "setting-or-entry-gui",
            "ground-time",
            "levels",
        ):
            marker = f"(pc-scene-wait-gate {gate})"
            self.assertIn(marker, code)
            self.assertIn("(suspend)", code[code.index(marker) :])

        self.assertIn("(get-status *gui-control* (-> self gui-id))", code)
        self.assertIn("(gui-status active)", code)

    def test_final_art_wait_records_actual_file_and_gui_results(self) -> None:
        art = extract_form(self.wait, "(let ((v1-161 (file-status")
        self.assertIn("(set! *pc-scene-wait-art-file-status* v1-161)", art)
        self.assertIn("(or (= v1-161 'active) (= v1-161 'locked))", art)
        self.assertIn("(pc-scene-wait-gate art-file)", art)
        self.assertIn("(lookup-gui-connection-id", art)
        self.assertIn("(lookup-gui-connection", art)
        self.assertIn("(set! *pc-scene-wait-art-gui-id* a1-26)", art)
        self.assertIn("(-> connection channel)", art)
        self.assertIn("(-> connection action)", art)
        self.assertIn("(get-status *gui-control* a1-26)", art)
        self.assertIn("(pc-scene-wait-gate art-gui)", art)
        self.assertIn("(or (= v1-167 (gui-status ready))", art)
        self.assertIn("(= v1-167 (gui-status active))", art)

        art_gate = self.wait.index(art)
        ready = self.wait.index("(pc-scene-wait-gate ready)", art_gate)
        self.assertLess(art_gate, self.wait.index("(set-blackout-frames (seconds 0.1))", art_gate))
        self.assertLess(self.wait.index("(suspend)", art_gate), ready)
        self.assertLess(ready, self.wait.index("(go-virtual play-anim)", ready))

    def test_synthetic_art_gate_matrix_does_not_skip_either_wait(self) -> None:
        for file_status in ("pending", "inactive", "reserved", "missing"):
            for gui_status in range(GUI_UNKNOWN, GUI_STOP + 1):
                self.assertEqual(final_art_gate(file_status, gui_status), ART_FILE)

        for file_status in ("active", "locked"):
            for gui_status in (GUI_UNKNOWN, GUI_PENDING, GUI_HIDE, GUI_STOP):
                self.assertEqual(final_art_gate(file_status, gui_status), ART_GUI)
            for gui_status in (GUI_READY, GUI_ACTIVE):
                self.assertEqual(final_art_gate(file_status, gui_status), READY)

    def test_current_port_cannot_infer_gui_readiness_from_successful_str_reads(self) -> None:
        get_status = extract_form(self.loader, "(defmethod get-status ((this gui-control)")
        self.assertIn("(-> *sound-iop-info* stream-name s4-0 name)", get_status)
        self.assertIn("(-> *sound-iop-info* stream-status s4-0)", get_status)
        self.assertIn("(stream-status ststatus-one ststatus-six)", get_status)
        self.assertIn("(file-status *art-control* (-> gp-0 name)", get_status)
        self.assertIn("Music and streaming", self.sound_header)
        self.assertIn("remain unsupported", self.sound_header)

    def test_native_metrics_copy_every_goal_diagnostic(self) -> None:
        fields = (
            "scene_wait_diagnostics_valid",
            "scene_wait_gate",
            "scene_wait_entry_gui_id",
            "scene_wait_entry_gui_status",
            "scene_wait_art_file_status",
            "scene_wait_art_file_status_name",
            "scene_wait_art_gui_id",
            "scene_wait_art_gui_channel",
            "scene_wait_art_gui_action",
            "scene_wait_art_gui_status",
        )
        for field in fields:
            self.assertIn(field, self.runtime_header)
            self.assertIn(f"g_metrics.{field}", self.runtime)

        for symbol in (
            "*pc-scene-wait-gate*",
            "*pc-scene-wait-entry-gui-id*",
            "*pc-scene-wait-entry-gui-status*",
            "*pc-scene-wait-art-file-status*",
            "*pc-scene-wait-art-gui-id*",
            "*pc-scene-wait-art-gui-channel*",
            "*pc-scene-wait-art-gui-action*",
            "*pc-scene-wait-art-gui-status*",
        ):
            self.assertIn(f'goal_game_find_symbol("{symbol}"', self.runtime)


if __name__ == "__main__":
    unittest.main()
