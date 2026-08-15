#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROGRESS = ROOT / "goal_src/jak2/engine/ui/progress/progress.gc"
PROGRESS_STATIC = ROOT / "goal_src/jak2/engine/ui/progress/progress-static.gc"
CITY_SCENES = ROOT / "goal_src/jak2/levels/city/ctywide-scenes.gc"
TITLE_OBS = ROOT / "goal_src/jak2/levels/title/title-obs.gc"
RUNTIME = ROOT / "game/kernel/core/jak2_runtime.cpp"
RUNTIME_HEADER = ROOT / "game/kernel/core/jak2_runtime.h"
BOOT_TEST = ROOT / "game/kernel/core/jak2_boot_test.cpp"


def extract_goal_form(source: str, marker: str) -> str:
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


class Jak2ScenePreviewContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.progress = PROGRESS.read_text()
        cls.progress_static = PROGRESS_STATIC.read_text()
        cls.city_scenes = CITY_SCENES.read_text()
        cls.title_obs = TITLE_OBS.read_text()
        cls.runtime = RUNTIME.read_text()
        cls.runtime_header = RUNTIME_HEADER.read_text()
        cls.boot = BOOT_TEST.read_text()

    def test_release_helper_reuses_scene_players_no_save_setup(self) -> None:
        helper = extract_goal_form(self.progress, "(defun scene-player-preview")
        self.assertIn("(play-clean 'debug)", helper)
        self.assertIn("(-> *game-info* demo-state)", helper)
        self.assertIn("(the-as uint 100)", helper)
        self.assertIn("(-> *game-info* secrets)", helper)
        self.assertIn("(game-secrets scene-player-1)", helper)
        self.assertIn(
            "(process-spawn scene-player :init scene-player-init info #t continue)", helper
        )
        self.assertIn("(set-master-mode 'game)", helper)
        for persistent_write in (
            "purchase-secrets",
            "auto-save",
            "save-user",
            "memcard",
            "task-node-open",
        ):
            self.assertNotIn(persistent_write, helper)

    def test_exact_name_lookup_uses_all_authored_scene_player_acts(self) -> None:
        lookup = extract_goal_form(self.progress, "(defun pc-preview-scene-by-name")
        for act in (
            "*hud-select-scene-act1*",
            "*hud-select-scene-act2*",
            "*hud-select-scene-act3*",
        ):
            self.assertIn(act, lookup)
        self.assertIn("(string? (-> scene-info info))", lookup)
        self.assertIn("(pair? (-> scene-info info))", lookup)
        self.assertIn(
            "(scene-player-preview name (-> (the hud-scene-info scene-info) continue))",
            lookup,
        )

    def test_menu_and_host_helper_share_one_preview_path(self) -> None:
        responder = extract_goal_form(
            self.progress,
            "(defmethod respond-progress ((this menu-select-scene-option)",
        )
        self.assertIn("(scene-player-preview (-> s5-1 info) (-> s5-1 continue))", responder)
        self.assertNotIn("(play-clean 'debug)", responder)

    def test_city_help_kid_authored_contract_is_exact(self) -> None:
        selection = extract_goal_form(
            self.progress_static,
            "(new 'static 'hud-scene-info\n"
            '                                    :name "city-help-kid-intro"',
        )
        self.assertIn(':continue "ctyslumb-fort"', selection)
        self.assertIn(':info "city-help-kid-intro"', selection)

        scene = extract_goal_form(
            self.city_scenes,
            "(scene-method-16\n  (new 'static 'scene\n    :name \"city-help-kid-intro\"",
        )
        self.assertIn(':entity "hal-help-kid-1"', scene)
        self.assertIn(':anim "city-help-kid-intro"', scene)
        self.assertIn(':load-point-obj "ctyslumb-fort"', scene)
        for level in ("ctyslumb", "ctywide", "ctykora"):
            self.assertIn(f":name '{level}", scene)
        for actor in (
            "sidekick-highres",
            "jak-highres",
            "kor-highres",
            "kid-highres",
        ):
            actor_form = extract_goal_form(
                scene, f"(new 'static 'scene-actor\n        :name \"{actor}\""
            )
            self.assertIn(":level 'ctykora", actor_form)

    def test_runtime_request_uses_authored_title_start_then_consumes_in_progress(self) -> None:
        self.assertIn("GOAL_JAK2_SCENE_PREVIEW_NAME_MAX 63", self.runtime_header)
        self.assertIn("GOAL_JAK2_RUNTIME_REQUEST_FAILED = 6", self.runtime_header)
        self.assertIn("goal_jak2_runtime_request_scene_preview", self.runtime_header)
        self.assertIn("while (length <= GOAL_JAK2_SCENE_PREVIEW_NAME_MAX", self.runtime)
        self.assertNotIn("std::strnlen", self.runtime)
        stable_title_start = self.runtime.index("bool stable_title_for_scene_preview_start()")
        stable_title_end = self.runtime.index(
            "\nbool progress_ready_for_scene_preview()", stable_title_start
        )
        stable_title = self.runtime[stable_title_start:stable_title_end]
        self.assertIn("g_metrics.title_control_process", stable_title)
        self.assertIn('std::strcmp(g_metrics.master_mode, "game") == 0', stable_title)
        self.assertIn('std::strcmp(g_metrics.title_control_state, "wait") == 0', stable_title)
        self.assertIn("!g_metrics.progress_process", stable_title)
        self.assertNotIn("read_progress_menu", stable_title)

        progress_ready_start = self.runtime.index("bool progress_ready_for_scene_preview()")
        progress_ready_end = self.runtime.index(
            "\ngoal_jak2_runtime_status prepare_pending_scene_preview_input()",
            progress_ready_start,
        )
        progress_ready = self.runtime[progress_ready_start:progress_ready_end]
        self.assertIn('std::strcmp(g_metrics.master_mode, "progress") == 0', progress_ready)
        self.assertIn("g_metrics.progress_process", progress_ready)

        title_wait = extract_goal_form(self.title_obs, "(defstate wait (title-control)")
        self.assertIn("(title-menu)", title_wait)
        title_idle = extract_goal_form(self.title_obs, "(defstate idle (title-control)")
        self.assertIn("(title-progress 'title)", title_idle)

        prepare_start = self.runtime.index(
            "goal_jak2_runtime_status prepare_pending_scene_preview_input()"
        )
        prepare_end = self.runtime.index(
            "\ngoal_jak2_runtime_status run_pending_scene_preview()", prepare_start
        )
        prepare = self.runtime[prepare_start:prepare_end]
        self.assertIn("if (!g_scene_preview_pending)", prepare)
        self.assertIn("ScenePreviewPhase::kAwaitPadReady", prepare)
        self.assertIn("goal_pad_state_neutral(&pad)", prepare)
        self.assertIn("goal_pad_read_count(0)", prepare)
        self.assertIn("pad.buttons = GOAL_PAD_START", prepare)
        self.assertIn("kScenePreviewNeutralWarmupReads = 4", self.runtime)
        self.assertIn(
            "goal_pad_read_count(0) - g_scene_preview_pad_read_baseline >=\n"
            "          kScenePreviewNeutralWarmupReads",
            prepare,
        )
        self.assertIn("kScenePreviewStartPressFrames = 2", self.runtime)
        self.assertIn(
            "g_scene_preview_start_press_frames == kScenePreviewStartPressFrames", prepare
        )
        self.assertIn("ScenePreviewPhase::kReleaseStart", prepare)
        self.assertIn("ScenePreviewPhase::kAwaitProgress", prepare)
        self.assertIn("goal_pad_set_state(0, &pad)", prepare)
        self.assertLess(
            prepare.index("if (!g_scene_preview_pending)"),
            prepare.index("goal_pad_set_state(0, &pad)"),
        )
        self.assertLess(
            prepare.index("goal_pad_read_count(0) -"),
            prepare.index("pad.buttons = GOAL_PAD_START"),
        )
        self.assertIn(
            'g_error = "Jak 2 scene preview could not override controller port 0";\n'
            "    reset_scene_preview_request();",
            prepare,
        )

        run_start = self.runtime.index("goal_jak2_runtime_status run_pending_scene_preview()")
        run_end = self.runtime.index("\n}\n\n}  // namespace", run_start)
        run_preview = self.runtime[run_start:run_end]
        self.assertIn(
            "g_scene_preview_phase != ScenePreviewPhase::kAwaitProgress", run_preview
        )
        self.assertIn("!progress_ready_for_scene_preview()", run_preview)
        self.assertIn(
            'goal_aot_call_symbol("pc-preview-scene-by-name", name, 0, 0, &result)',
            self.runtime,
        )
        tick = self.runtime.index("goal_jak2_runtime_status goal_jak2_runtime_tick")
        dispatch = self.runtime.index("call_goal_on_stack(Ptr<Function>(g_dispatcher)", tick)
        input_override = self.runtime.index("prepare_pending_scene_preview_input()", tick)
        preview = self.runtime.index("run_pending_scene_preview()", dispatch)
        self.assertLess(input_override, dispatch)
        self.assertLess(dispatch, preview)
        self.assertIn("had_pending_preview && !g_scene_preview_pending", self.runtime[preview:])

    def test_boot_cli_uses_a_unique_temp_save_and_detects_any_persistence(self) -> None:
        self.assertIn('arg == "--preview-scene"', self.boot)
        self.assertIn("goal_jak2_runtime_request_scene_preview", self.boot)
        self.assertIn("std::filesystem::temp_directory_path()", self.boot)
        self.assertIn("snapshot_save_tree(saves_path, &saves_before_preview", self.boot)
        self.assertIn("snapshot_save_tree(saves_path, &saves_after_preview", self.boot)
        self.assertIn("saves_after_preview != saves_before_preview", self.boot)
        self.assertIn("std::filesystem::last_write_time", self.boot)
        self.assertIn("metrics.title_control_process", self.boot)
        self.assertIn('std::strcmp(metrics.master_mode, "game") == 0', self.boot)
        self.assertIn('std::strcmp(metrics.title_control_state, "wait") == 0', self.boot)
        self.assertIn("!metrics.progress_process", self.boot)
        self.assertIn('preview_scene == "city-help-kid-intro"', self.boot)
        self.assertIn('metrics.scene_entity, "hal-help-kid-1"', self.boot)
        self.assertIn("metrics.animation_diagnostics_valid", self.boot)
        self.assertIn("preview_first_aframe = metrics.animation_aframe", self.boot)
        self.assertIn(
            "std::fabs(metrics.animation_aframe - preview_first_aframe) > 0.001f",
            self.boot,
        )
        self.assertIn("for (int index = 1; index <= 4; ++index)", self.boot)
        self.assertIn("actor.level_index != 2", self.boot)
        self.assertIn("actor.merc_pris_bucket != 205", self.boot)
        self.assertIn("!actor.merc_joint_count", self.boot)
        for flag in (
            "SPAWN_ATTEMPTED",
            "POOL_ALLOCATED",
            "DRAW_CONTROL",
            "JOINT_CONTROL",
            "MERC_GEOMETRY",
        ):
            self.assertIn(f"GOAL_JAK2_SCENE_ACTOR_{flag}", self.boot)
        for level in ("ctyslumb", "ctywide", "ctykora"):
            self.assertIn(f'level == "{level}"', self.boot)


if __name__ == "__main__":
    unittest.main()
