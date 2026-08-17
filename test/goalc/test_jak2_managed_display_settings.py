import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def extract_form(source: str, marker: str) -> str:
    start = source.index(marker)
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(source)):
        char = source[index]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
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
    body = source.index("{", start)
    depth = 0
    for index in range(body, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {marker}")


class Jak2ManagedDisplaySettingsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.pckernel = (ROOT / "goal_src/jak2/pc/pckernel.gc").read_text()
        cls.pckernel_common = (ROOT / "goal_src/jak1/pc/pckernel-common.gc").read_text()
        cls.machine = (ROOT / "game/kernel/core/kernel_game_jak2.cpp").read_text()

    def test_jak2_installs_the_portable_display_queries(self) -> None:
        init_machine = extract_cpp_function(self.machine, "void InitMachineScheme()")
        self.assertIn("goal_kernel_core_install_portable_pc_settings_functions();", init_machine)

    def test_managed_display_normalizes_only_incompatible_aspect_state(self) -> None:
        normalize = extract_form(self.pckernel, "(defun normalize-managed-display-aspect!")
        initialize = extract_form(
            self.pckernel, "(defmethod initialize ((obj pc-settings-jak2))"
        )
        for expected in (
            "(pc-host-manages-display?)",
            "(-> obj use-vis?)",
            "(not (-> obj aspect-ratio-auto?))",
            "(!= (get-game-setting obj 'aspect-ratio) 'aspect4x3)",
            "(set-aspect! obj 4 3)",
            "(set-game-setting! obj 'aspect-ratio 'aspect4x3)",
            "(set! (-> obj aspect-ratio-auto?) #t)",
            "(commit-to-file obj)",
        ):
            self.assertIn(expected, normalize)
        self.assertLess(
            initialize.index("((method-of-type pc-settings initialize) obj)"),
            initialize.index("(normalize-managed-display-aspect! obj)"),
        )

    def test_portable_window_size_drives_the_existing_hor_plus_camera_path(self) -> None:
        update_from_os = extract_form(self.pckernel_common, "(defmethod update-from-os")
        self.assertIn("(pc-get-window-size", update_from_os)
        self.assertIn(
            "(and (not (-> obj use-vis?)) (-> obj aspect-ratio-auto?))", update_from_os
        )
        self.assertIn("(set-aspect-ratio! obj win-aspect)", update_from_os)


if __name__ == "__main__":
    unittest.main()
