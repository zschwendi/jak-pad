#!/usr/bin/env python3

from copy import deepcopy
from dataclasses import dataclass
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
KERNEL_DEFS = ROOT / "goal_src/jak1/kernel-defs.gc"
PCKERNEL = ROOT / "goal_src/jak1/pc/pckernel.gc"
PCKERNEL_COMMON = ROOT / "goal_src/jak1/pc/pckernel-common.gc"
PROGRESS = ROOT / "goal_src/jak1/pc/progress-pc.gc"
DESKTOP_MACHINE = ROOT / "game/kernel/common/kmachine.cpp"
PORTABLE_MACHINE = ROOT / "game/kernel/core/desktop_seams.cpp"
JAK1_MACHINE = ROOT / "game/kernel/core/kernel_game_jak1.cpp"
HOST_SMOKE = ROOT / "game/kernel/core/host_smoke_test.cpp"
GFX_HOST_TEST = ROOT / "game/kernel/core/jak1_gfx_host_test.cpp"


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


def option_names(form: str) -> list[str]:
    return re.findall(r":name\s+\(text-id\s+([^\s)]+)\)", form)


def extract_cpp_function(source: str, marker: str) -> str:
    start = source.index(marker)
    body_start = source.index("{", start)
    depth = 0
    for index in range(body_start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated C++ function: {marker}")


@dataclass
class SettingsModel:
    use_vis: bool
    aspect_auto: bool
    aspect: float
    aspect_scale: float
    aspect_reciprocal: float
    custom_x: int
    custom_y: int
    game_aspect: str
    unrelated: dict[str, object]
    commits: int = 0


def normalize_managed_display_aspect(settings: SettingsModel, managed: bool) -> None:
    if not managed or (
        not settings.use_vis and settings.aspect_auto and settings.game_aspect == "aspect4x3"
    ):
        return
    settings.aspect = 4.0 / 3.0
    settings.aspect_scale = 1.0
    settings.aspect_reciprocal = 1.0
    settings.custom_x = 4
    settings.custom_y = 3
    settings.use_vis = False
    settings.game_aspect = "aspect4x3"
    settings.aspect_auto = True
    settings.commits += 1


class Jak1ManagedDisplaySettingsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.kernel_defs = KERNEL_DEFS.read_text()
        cls.pckernel = PCKERNEL.read_text()
        cls.pckernel_common = PCKERNEL_COMMON.read_text()
        cls.progress = PROGRESS.read_text()
        cls.desktop_machine = DESKTOP_MACHINE.read_text()
        cls.portable_machine = PORTABLE_MACHINE.read_text()
        cls.jak1_machine = JAK1_MACHINE.read_text()
        cls.host_smoke = HOST_SMOKE.read_text()
        cls.gfx_host_test = GFX_HOST_TEST.read_text()

    def test_capability_is_declared_and_has_explicit_desktop_and_portable_values(self) -> None:
        self.assertIn("(define-extern pc-host-manages-display? (function symbol))", self.kernel_defs)

        desktop_value = extract_cpp_function(self.desktop_machine, "u64 pc_host_manages_display()")
        desktop_install = extract_cpp_function(
            self.desktop_machine, "void init_common_pc_port_functions("
        )
        self.assertIn("return bool_to_symbol(false);", desktop_value)
        self.assertIn(
            'make_func_symbol_func("pc-host-manages-display?", (void*)pc_host_manages_display);',
            desktop_install,
        )

        portable_value = extract_cpp_function(
            self.portable_machine, "u64 portable_pc_host_manages_display()"
        )
        portable_install = extract_cpp_function(
            self.portable_machine, "void goal_kernel_core_install_portable_display_functions()"
        )
        self.assertIn("return goal_bool(true);", portable_value)
        self.assertIn('goal_game_make_function_symbol("pc-host-manages-display?",', portable_install)
        self.assertIn('"pc-host-manages-display?",', self.jak1_machine)
        self.assertIn(
            "portable kernel host reports that it manages display presentation",
            self.gfx_host_test,
        )

        init_machine = extract_cpp_function(self.jak1_machine, "void InitMachineScheme()")
        self.assertLess(
            init_machine.index("goal_kernel_core_install_machine_stubs"),
            init_machine.index("goal_kernel_core_install_portable_display_functions"),
        )

    def test_desktop_graphics_rows_are_unchanged_and_managed_rows_are_reduced(self) -> None:
        desktop = extract_form(self.progress, "(define *graphic-options-pc*")
        managed = extract_form(self.progress, "(define *graphic-options-managed-display*")
        self.assertEqual(
            option_names(desktop),
            [
                "game-resolution",
                "display-mode",
                "display",
                "vsync",
                "aspect-ratio",
                "msaa",
                "frame-rate",
                "ps2-options",
                "back",
            ],
        )
        self.assertEqual(
            option_names(managed),
            ["game-resolution", "aspect-ratio", "frame-rate", "ps2-options", "back"],
        )
        for removed in ("display-mode", "display", "vsync", "msaa"):
            self.assertNotIn(f"(text-id {removed})", managed)

        init_options = extract_form(self.progress, "(defun init-game-options")
        self.assertIn(
            "(if (pc-host-manages-display?) *graphic-options-managed-display* *graphic-options-pc*)",
            init_options,
        )
        self.assertIn(
            "(set! (-> *graphic-options-managed-display* 2 value-to-modify) "
            "(&-> *progress-carousell* int-backup))",
            init_options,
        )

    def test_managed_aspect_submenu_can_only_select_fit_to_screen_or_back(self) -> None:
        desktop = extract_form(self.progress, "(define *aspect-ratio-options*")
        managed = extract_form(self.progress, "(define *aspect-ratio-options-managed-display*")
        self.assertEqual(option_names(managed), ["fit-to-screen", "back"])
        self.assertEqual(managed.count("(game-option-type aspect-new)"), 1)
        self.assertNotIn(":param1", managed)
        self.assertNotIn(":param2", managed)
        self.assertIn("(text-id aspect4x3-ps2)", desktop)
        self.assertIn("(text-id aspect16x9-ps2)", desktop)

        init_options = extract_form(self.progress, "(defun init-game-options")
        self.assertIn(
            "(if (pc-host-manages-display?) "
            "*aspect-ratio-options-managed-display* *aspect-ratio-options*)",
            init_options,
        )

    def test_migration_is_managed_only_and_commits_only_incompatible_native_state(self) -> None:
        migration = extract_form(self.pckernel, "(defun normalize-managed-display-aspect!")
        initialize = extract_form(self.pckernel, "(defmethod initialize ((obj pc-settings-jak1))")
        self.assertIn("(pc-host-manages-display?)", migration)
        self.assertIn("(-> obj use-vis?)", migration)
        self.assertIn("(not (-> obj aspect-ratio-auto?))", migration)
        self.assertIn("(!= (get-game-setting obj 'aspect-ratio) 'aspect4x3)", migration)
        self.assertIn("(set-aspect! obj 4 3)", migration)
        self.assertIn("(set-game-setting! obj 'aspect-ratio 'aspect4x3)", migration)
        self.assertIn("(set! (-> obj aspect-ratio-auto?) #t)", migration)
        self.assertEqual(migration.count("(commit-to-file obj)"), 1)
        self.assertLess(
            initialize.index("((method-of-type pc-settings initialize) obj)"),
            initialize.index("(normalize-managed-display-aspect! obj)"),
        )

    def test_migration_is_idempotent_and_preserves_unrelated_settings(self) -> None:
        unrelated = {
            "fps": 100,
            "msaa": 8,
            "vsync": False,
            "window": (1234, 777),
            "volumes": (73.0, 42.0, 88.0),
            "camera": (True, False, True, False),
            "language": 6,
            "cheats": 0x1234,
        }
        fixed = SettingsModel(
            use_vis=False,
            aspect_auto=False,
            aspect=21.0 / 9.0,
            aspect_scale=1.75,
            aspect_reciprocal=4.0 / 7.0,
            custom_x=21,
            custom_y=9,
            game_aspect="aspect4x3",
            unrelated=deepcopy(unrelated),
        )
        normalize_managed_display_aspect(fixed, managed=True)
        self.assertEqual(
            (
                fixed.use_vis,
                fixed.aspect_auto,
                fixed.aspect,
                fixed.aspect_scale,
                fixed.aspect_reciprocal,
                fixed.custom_x,
                fixed.custom_y,
                fixed.game_aspect,
                fixed.commits,
            ),
            (False, True, 4.0 / 3.0, 1.0, 1.0, 4, 3, "aspect4x3", 1),
        )
        self.assertEqual(fixed.unrelated, unrelated)
        normalize_managed_display_aspect(fixed, managed=True)
        self.assertEqual(fixed.commits, 1)
        self.assertEqual(fixed.unrelated, unrelated)

        use_vis = SettingsModel(
            use_vis=True,
            aspect_auto=True,
            aspect=4.0 / 3.0,
            aspect_scale=1.0,
            aspect_reciprocal=1.0,
            custom_x=4,
            custom_y=3,
            game_aspect="aspect4x3",
            unrelated=deepcopy(unrelated),
        )
        normalize_managed_display_aspect(use_vis, managed=True)
        self.assertEqual(use_vis.commits, 1)
        self.assertFalse(use_vis.use_vis)
        self.assertTrue(use_vis.aspect_auto)
        self.assertEqual(use_vis.unrelated, unrelated)

        inconsistent_game_aspect = SettingsModel(
            use_vis=False,
            aspect_auto=True,
            aspect=4.0 / 3.0,
            aspect_scale=1.0,
            aspect_reciprocal=1.0,
            custom_x=4,
            custom_y=3,
            game_aspect="aspect16x9",
            unrelated=deepcopy(unrelated),
        )
        normalize_managed_display_aspect(inconsistent_game_aspect, managed=True)
        self.assertEqual(inconsistent_game_aspect.commits, 1)
        self.assertEqual(inconsistent_game_aspect.game_aspect, "aspect4x3")
        self.assertEqual(inconsistent_game_aspect.unrelated, unrelated)

        compatible = SettingsModel(
            use_vis=False,
            aspect_auto=True,
            aspect=4.0 / 3.0,
            aspect_scale=1.0,
            aspect_reciprocal=1.0,
            custom_x=4,
            custom_y=3,
            game_aspect="aspect4x3",
            unrelated=deepcopy(unrelated),
        )
        compatible_before = deepcopy(compatible)
        normalize_managed_display_aspect(compatible, managed=True)
        normalize_managed_display_aspect(compatible, managed=True)
        self.assertEqual(compatible, compatible_before)

        desktop = SettingsModel(
            use_vis=True,
            aspect_auto=False,
            aspect=16.0 / 9.0,
            aspect_scale=4.0 / 3.0,
            aspect_reciprocal=0.75,
            custom_x=16,
            custom_y=9,
            game_aspect="aspect16x9",
            unrelated=deepcopy(unrelated),
        )
        before = deepcopy(desktop)
        normalize_managed_display_aspect(desktop, managed=False)
        self.assertEqual(desktop, before)

    def test_portable_size_contract_keeps_classic_4x3_and_modern_host_size_native_auto(self) -> None:
        self.assertIn("g_portable_display_width{640}", self.portable_machine)
        self.assertIn("g_portable_display_height{480}", self.portable_machine)
        self.assertIn("goal_kernel_core_set_portable_display_size(1366, 1024)", self.host_smoke)
        self.assertIn("display_width == 640 && display_height == 480", self.host_smoke)
        update_from_os = extract_form(self.pckernel_common, "(defmethod update-from-os")
        self.assertIn("(and (not (-> obj use-vis?)) (-> obj aspect-ratio-auto?))", update_from_os)
        self.assertIn("(set-aspect-ratio! obj win-aspect)", update_from_os)


if __name__ == "__main__":
    unittest.main()
