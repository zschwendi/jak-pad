#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SHARED_PROGRESS = ROOT / "goal_src/jak2/engine/ui/progress/progress.gc"
PC_PROGRESS = ROOT / "goal_src/jak2/pc/progress/progress-pc.gc"
PC_RESPONDER = ROOT / "goal_src/jak2/pc/progress/progress-generic-pc.gc"
PROGRESS_STATIC = ROOT / "goal_src/jak2/engine/ui/progress/progress-static.gc"
GAME_DGO = ROOT / "goal_src/jak2/dgos/game.gd"


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


class Jak2ContinueWithoutSavingContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        shared = SHARED_PROGRESS.read_text()
        pc = PC_PROGRESS.read_text()
        cls.pc_responder = PC_RESPONDER.read_text()
        cls.progress_static = PROGRESS_STATIC.read_text()
        cls.game_dgo = GAME_DGO.read_text()
        marker = "(defmethod respond-progress ((this menu-sub-menu-option)"
        cls.shared_respond = extract_form(shared, marker)
        cls.pc_respond = extract_form(pc, marker)

    def test_title_save_menu_keeps_no_save_as_the_fifth_option(self) -> None:
        save_options = extract_form(
            self.progress_static, "(define *save-options-title*"
        )
        self.assertEqual(save_options.count("(new 'static 'menu-memcard-slot-option)"), 4)
        self.assertEqual(
            save_options.count("(text-id progress-continue-without-saving)"), 1
        )
        self.assertIn(":next-state 'continue", save_options)

    def test_pc_runtime_dispatches_only_the_highlighted_option(self) -> None:
        respond_to_cpad = extract_form(
            self.pc_responder, "(defmethod respond-to-cpad ((this progress))"
        )
        self.assertIn(
            "(the-as menu-option (-> option-array (-> this option-index)))",
            respond_to_cpad,
        )

    def test_pc_responder_override_loads_after_the_shared_responder(self) -> None:
        shared = self.game_dgo.index('"progress.o"')
        pc = self.game_dgo.index('"progress-pc.o"')
        input_dispatch = self.game_dgo.index('"progress-generic-pc.o"')
        self.assertLess(shared, pc)
        self.assertLess(pc, input_dispatch)

    def test_no_save_never_schedules_a_card_state_transition(self) -> None:
        for respond in (self.shared_respond, self.pc_respond):
            self.assertNotIn("(get-state-check-card", respond)

    def test_no_save_confirm_starts_the_selected_normal_or_hero_intro(self) -> None:
        marker = (
            "((= (-> this name) "
            "(text-id progress-continue-without-saving))"
        )
        expected = (
            "(progress-intro-start "
            "(logtest? (-> *game-info* purchase-secrets) "
            "(game-secrets hero-mode)))"
        )
        for respond in (self.shared_respond, self.pc_respond):
            branch = extract_form(respond, marker)
            self.assertIn(expected, branch)
            self.assertNotIn("set-next-state", branch)
            self.assertNotIn("push-state", branch)

    def test_existing_title_and_secret_guards_remain_platform_specific(self) -> None:
        for respond, title, unavailable_index in (
            (self.shared_respond, "*title*", 3),
            (self.pc_respond, "*title-pc*", 4),
        ):
            self.assertIn("(-> *progress-state* secrets-unlocked)", respond)
            self.assertIn("(not (memcard-unlocked-secrets? #f))", respond)
            self.assertIn("(= (-> arg0 current-options) *save-options-title*)", respond)
            self.assertIn("(set-next-state arg0 'secrets-insufficient-space 0)", respond)
            self.assertIn(f"(= {title} (-> arg0 current-options))", respond)
            self.assertIn(f"(= (-> arg0 option-index) {unavailable_index})", respond)


if __name__ == "__main__":
    unittest.main()
