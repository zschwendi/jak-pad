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
        cls.desktop_seams = (ROOT / "game/kernel/core/desktop_seams.cpp").read_text()
        cls.runtime = (ROOT / "game/kernel/core/jak2_runtime.cpp").read_text()
        cls.runtime_header = (ROOT / "game/kernel/core/jak2_runtime.h").read_text()
        cls.display = (ROOT / "goal_src/jak2/engine/gfx/hw/display.gc").read_text()
        cls.video = (ROOT / "goal_src/jak2/engine/gfx/hw/video.gc").read_text()

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

    def test_existing_jak2_goal_code_accepts_the_host_selected_refresh_rate(self) -> None:
        setter = extract_form(self.pckernel_common, "(defmethod set-frame-rate!")
        for expected in (
            "(pc-host-manages-display?)",
            "(pc-get-active-display-refresh-rate)",
            "(pc-set-frame-rate effective-rate)",
            "(set! (-> obj target-fps) effective-rate)",
            "(set-game-setting! obj 'video-mode 'custom)",
        ):
            self.assertIn(expected, setter)

        self.assertIn("(/ 300.0 (-> *pc-settings* target-fps))", self.display)
        self.assertIn("(sound-set-fps (-> *pc-settings* target-fps))", self.video)

    def test_runtime_uses_the_generated_pc_settings_method_and_portable_refresh_seam(self) -> None:
        apply_rate = extract_cpp_function(self.runtime, "bool apply_goal_target_frame_rate")
        self.assertIn('#include "pckernel_common_generated.h"', self.runtime)
        self.assertIn("goal_pckernel_common__method_set_frame_rate_bang_pc_settings_", apply_rate)
        self.assertIn("goal_kernel_core_set_portable_display_refresh_rate(target_frame_rate)", apply_rate)
        self.assertIn("goal_jak2_display_timing_set_target_frame_rate", apply_rate)

        startup = extract_cpp_function(self.runtime, "goal_jak2_runtime_status goal_jak2_runtime_start")
        self.assertIn("requested_target_frame_rate == 120 &&", startup)
        self.assertIn("variable or 60 Hz delivery must use tick_at", self.runtime_header)

        failed_start = extract_cpp_function(self.runtime, "goal_jak2_runtime_status fail_start")
        self.assertIn("goal_kernel_core_set_portable_display_refresh_rate(60)", failed_start)

        refresh_rate = extract_cpp_function(
            self.desktop_seams, "s64 portable_pc_get_refresh_rate()"
        )
        self.assertIn("g_portable_display_refresh_rate.load", refresh_rate)


if __name__ == "__main__":
    unittest.main()
