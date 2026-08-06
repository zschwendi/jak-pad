#!/usr/bin/env python3

from dataclasses import dataclass
from pathlib import Path
from typing import Optional
import unittest


ROOT = Path(__file__).resolve().parents[2]
JAK1 = ROOT / "goal_src/jak1"

PROCESS_TASKABLE = JAK1 / "engine/common-obs/process-taskable.gc"
PAD = JAK1 / "engine/ps2/pad.gc"
ORACLE = JAK1 / "levels/village_common/oracle.gc"
RACER = JAK1 / "levels/racer_common/racer.gc"
FLUTFLUT = JAK1 / "levels/flut_common/flutflut.gc"
BOAT = JAK1 / "levels/village1/fishermans-boat.gc"
MISTY_CANNON = JAK1 / "levels/misty/mistycannon.gc"
JUNGLE_MIRRORS = JAK1 / "levels/jungle/jungle-mirrors.gc"
GONDOLA = JAK1 / "levels/village3/village3-obs.gc"
FISHER = JAK1 / "levels/jungle/fisher.gc"
BILLY = JAK1 / "levels/swamp/billy.gc"
SUBTITLE = JAK1 / "pc/subtitle.gc"
BASEBUTTON = JAK1 / "engine/common-obs/basebutton.gc"
VILLAGEP = JAK1 / "levels/village_common/villagep-obs.gc"

TRIANGLE = 1 << 12
CIRCLE = 1 << 13
CROSS = 1 << 14
SQUARE = 1 << 15


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
    requested_buttons: int = 0
    sequence: int = 0
    heartbeat: int = 0

    def publish(self, owner: object, requested_buttons: int) -> None:
        if not requested_buttons:
            return
        if self.owner is owner and self.requested_buttons == requested_buttons:
            self.heartbeat += 1
            return
        self.sequence += 1
        self.owner = owner
        self.requested_buttons = requested_buttons
        self.heartbeat = 1
        self.sequence += 1

    def clear(self, owner: object) -> None:
        if self.owner is not owner:
            return
        self.sequence += 1
        self.owner = None
        self.requested_buttons = 0
        self.heartbeat = 0
        self.sequence += 1


class Jak1FacePromptTouchContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.process_taskable = PROCESS_TASKABLE.read_text()
        cls.pad = PAD.read_text()
        cls.oracle = ORACLE.read_text()
        cls.racer = RACER.read_text()
        cls.flutflut = FLUTFLUT.read_text()
        cls.boat = BOAT.read_text()
        cls.misty_cannon = MISTY_CANNON.read_text()
        cls.jungle_mirrors = JUNGLE_MIRRORS.read_text()
        cls.gondola = GONDOLA.read_text()
        cls.fisher = FISHER.read_text()
        cls.billy = BILLY.read_text()
        cls.subtitle = SUBTITLE.read_text()

    def test_fixed_revisioned_owner_snapshot_abi(self) -> None:
        snapshot_type = extract_form(
            self.process_taskable,
            "(deftype goalpad-face-prompt-touch-snapshot",
        )
        for field in (
            "(revision          int32  :offset-assert 4)",
            "(sequence          int32  :offset-assert 8)",
            "(requested-buttons uint32 :offset-assert 12)",
            "(owner             handle :offset-assert 16)",
            "(heartbeat         int32  :offset-assert 24)",
        ):
            self.assertIn(field, snapshot_type)
        self.assertIn(":method-count-assert 9", snapshot_type)
        self.assertIn(":size-assert #x1c", snapshot_type)

        snapshot = extract_form(
            self.process_taskable,
            "(define *goalpad-face-prompt-touch-snapshot*",
        )
        self.assertIn("'goalpad-face-prompt-touch-snapshot", snapshot)
        self.assertIn(":revision 1", snapshot)
        self.assertIn(":sequence 0", snapshot)
        self.assertIn(":requested-buttons 0", snapshot)
        self.assertNotIn(":owner", snapshot)
        self.assertIn(":heartbeat 0", snapshot)
        self.assertNotIn(
            "(-> *goalpad-face-prompt-touch-snapshot* revision)",
            self.process_taskable,
        )

    def test_face_masks_are_ordinary_pad_button_bytes(self) -> None:
        buttons = extract_form(self.pad, "(defenum pad-buttons")
        for declaration in (
            "(triangle 12)",
            "(circle 13)",
            "(x 14)",
            "(square 15)",
        ):
            self.assertIn(declaration, buttons)
        self.assertEqual((TRIANGLE, CIRCLE, CROSS, SQUARE), (0x1000, 0x2000, 0x4000, 0x8000))
        self.assertEqual(TRIANGLE | CROSS, 0x5000)

    def test_publish_clear_seqlock_and_heartbeat_contract(self) -> None:
        publish = extract_form(
            self.process_taskable,
            "(defun goalpad-face-prompt-touch-publish!",
        )
        clear = extract_form(
            self.process_taskable,
            "(defun goalpad-face-prompt-touch-clear!",
        )
        self.assertIn("(and owner (nonzero? (the-as uint requested-buttons)))", publish)
        self.assertIn("(= *goalpad-face-prompt-touch-owner* owner)", publish)
        self.assertIn(
            "(= (-> *goalpad-face-prompt-touch-snapshot* requested-buttons) (the-as uint requested-buttons))",
            publish,
        )
        self.assertEqual(
            publish.count("(+! (-> *goalpad-face-prompt-touch-snapshot* sequence) 1)"),
            2,
        )
        self.assertIn(
            "(+! (-> *goalpad-face-prompt-touch-snapshot* heartbeat) 1)",
            publish,
        )
        self.assertIn(
            "(set! (-> *goalpad-face-prompt-touch-snapshot* heartbeat) 1)",
            publish,
        )
        self.assertIn("(process->handle owner)", publish)

        self.assertIn("(when (= *goalpad-face-prompt-touch-owner* owner)", clear)
        self.assertEqual(
            clear.count("(+! (-> *goalpad-face-prompt-touch-snapshot* sequence) 1)"),
            2,
        )
        self.assertIn("requested-buttons) 0", clear)
        self.assertIn("owner) (the-as handle 0)", clear)
        self.assertIn("heartbeat) 0", clear)

    def test_owner_mask_heartbeat_and_stale_clear_model(self) -> None:
        first = object()
        second = object()
        snapshot = SnapshotModel()

        snapshot.publish(first, 0)
        self.assertEqual((snapshot.sequence, snapshot.owner), (0, None))
        snapshot.publish(first, CIRCLE)
        self.assertEqual((snapshot.sequence, snapshot.heartbeat), (2, 1))
        self.assertEqual(snapshot.sequence % 2, 0)
        snapshot.publish(first, CIRCLE)
        self.assertEqual((snapshot.sequence, snapshot.heartbeat), (2, 2))
        snapshot.publish(first, TRIANGLE | CROSS)
        self.assertEqual((snapshot.sequence, snapshot.heartbeat), (4, 1))
        snapshot.publish(second, SQUARE)
        self.assertEqual((snapshot.owner, snapshot.sequence, snapshot.heartbeat), (second, 6, 1))
        snapshot.clear(first)
        self.assertEqual((snapshot.owner, snapshot.sequence), (second, 6))
        snapshot.clear(second)
        self.assertEqual(
            (snapshot.owner, snapshot.requested_buttons, snapshot.sequence, snapshot.heartbeat),
            (None, 0, 8, 0),
        )
        self.assertEqual(snapshot.sequence % 2, 0)

    def test_common_talk_trade_and_oracle_circle_lease(self) -> None:
        idle = extract_form(self.process_taskable, "(defstate idle (process-taskable)")
        self.assertIn("(-> self talk-message)", idle)
        self.assertIn("(set! goalpad-face-prompt? #t)", idle)
        self.assertIn("(goalpad-face-prompt-touch-publish! self (pad-buttons circle))", idle)
        self.assertIn("(cpad-pressed? 0 circle)", idle)
        self.assertLess(
            idle.index("(print-game-text"),
            idle.index("(set! goalpad-face-prompt? #t)"),
        )
        self.assertLess(
            idle.index("(goalpad-face-prompt-touch-clear! self)", idle.index("(cpad-pressed? 0 circle)")),
            idle.index("(go-virtual play-anim)"),
        )

        oracle_idle = extract_form(self.oracle, "(defstate idle (oracle)")
        self.assertIn("(-> (method-of-type process-taskable idle) trans)", oracle_idle)
        self.assertIn("(-> (method-of-type process-taskable idle) exit)", oracle_idle)

    def test_circle_authored_source_inventory_and_lifecycle(self) -> None:
        cases = (
            (self.racer, "(defstate idle (racer)"),
            (self.flutflut, "(defstate idle (flutflut)"),
            (self.boat, "(defbehavior fishermans-boat-leave-dock?"),
            (self.misty_cannon, "(defstate mistycannon-waiting-for-player"),
            (self.jungle_mirrors, "(defstate periscope-wait-for-player"),
            (self.gondola, "(defstate idle (gondola)"),
        )
        for source, marker in cases:
            with self.subTest(marker=marker):
                form = extract_form(source, marker)
                self.assertIn("(pad-buttons circle)", form)
                self.assertIn("(goalpad-face-prompt-touch-publish! self", form)
                self.assertIn("(goalpad-face-prompt-touch-clear! self)", form)
                self.assertLess(form.index("(print-game-text"), form.index("(set! goalpad-face-prompt? #t)"))

        racer_idle = extract_form(self.racer, "(defstate idle (racer)")
        self.assertIn("(when (!= (-> self condition) 4)", racer_idle)
        self.assertIn("(or (cpad-pressed? 0 circle) (= (-> self condition) 4))", racer_idle)
        self.assertLess(
            racer_idle.index("(set! goalpad-face-prompt? #f)", racer_idle.index("(cpad-pressed? 0 circle)")),
            racer_idle.index("(go-virtual pickup"),
        )

        boat_village = extract_form(self.boat, "(defstate fishermans-boat-docked-village")
        boat_misty = extract_form(self.boat, "(defstate fishermans-boat-docked-misty")
        self.assertIn("(goalpad-face-prompt-touch-clear! self)", boat_village)
        self.assertIn("(goalpad-face-prompt-touch-clear! self)", boat_misty)

    def test_gondola_cell_notices_do_not_publish(self) -> None:
        gondola = extract_form(self.gondola, "(defstate idle (gondola)")
        need_cells = gondola.index("(text-id gondola-need-cells)")
        enough_cells = gondola.index("(text-id gondola-enough-cells)")
        prompt_text = gondola.index("(text-id press-to-use)")
        publish_flag = gondola.index("(set! goalpad-face-prompt? #t)")
        self.assertLess(need_cells, enough_cells)
        self.assertLess(enough_cells, prompt_text)
        self.assertLess(prompt_text, publish_flag)
        self.assertNotIn("goalpad-face-prompt", gondola[need_cells:enough_cells])

    def test_fisher_billy_gui_queries_are_specific_and_decision_aware(self) -> None:
        wrapper = extract_form(
            self.process_taskable,
            "(defun goalpad-face-prompt-touch-get-response",
        )
        self.assertIn("(= response 'undecided)", wrapper)
        self.assertIn("(hud-hidden?)", wrapper)
        self.assertIn("(-> query only-allow-cancel)", wrapper)
        self.assertIn("(pad-buttons triangle)", wrapper)
        self.assertIn("(pad-buttons x triangle)", wrapper)
        self.assertLess(
            wrapper.index("(get-response query)"),
            wrapper.index("goalpad-face-prompt-touch-publish!"),
        )
        self.assertIn("(else\n       (goalpad-face-prompt-touch-clear! owner))", wrapper)

        fisher_query = extract_form(self.fisher, "(defstate query (fisher)")
        fisher_done = extract_form(self.fisher, "(defstate fisher-done")
        billy_query = extract_form(self.billy, "(defstate query (billy)")
        billy_done = extract_form(self.billy, "(defstate billy-done")
        for form in (fisher_query, fisher_done, billy_query, billy_done):
            self.assertIn("goalpad-face-prompt-touch-get-response", form)
        for form in (fisher_done, billy_done):
            self.assertIn("goalpad-face-prompt-touch-clear! self", form)
        self.assertIn("(method-of-type process-taskable play-anim) exit", fisher_query)
        parent_exit = extract_form(
            self.process_taskable,
            "(defbehavior process-taskable-play-anim-exit",
        )
        self.assertIn("goalpad-face-prompt-touch-clear! self", parent_exit)

        generic = extract_form(self.process_taskable, "(defmethod get-response ((this gui-query))")
        cutscene = extract_form(self.process_taskable, "(defun goalpad-cutscene-skip-touch-query")
        play_anim = extract_form(self.process_taskable, "(defbehavior process-taskable-play-anim-code")
        fisher_play_anim = extract_form(self.fisher, "(defstate play-anim (fisher)")
        for form in (generic, cutscene, play_anim, fisher_play_anim):
            self.assertNotIn("goalpad-face-prompt-touch-get-response", form)
        self.assertIn("goalpad-cutscene-skip-touch-query", play_anim)
        self.assertIn("(get-response (-> self query))", fisher_play_anim)

    def test_first_intro_subtitle_square_only_tracks_drawn_notice(self) -> None:
        subtitle = extract_form(self.subtitle, "(defstate subtitle-process (subtitle)")
        self.assertIn("(= (-> self notice-id) (text-id subtitle-hint))", subtitle)
        self.assertIn("(not (-> *pc-settings* subtitles?))", subtitle)
        self.assertIn("(goalpad-face-prompt-touch-publish! self (pad-buttons square))", subtitle)
        first_notice = subtitle.index("(text-id subtitle-hint)", subtitle.index(":post"))
        draw = subtitle.index("(print-game-subtitle", first_notice)
        publish = subtitle.index("(goalpad-face-prompt-touch-publish! self (pad-buttons square))", draw)
        self.assertLess(draw, publish)

        acceptance = subtitle.index("(cpad-pressed? 0 square)")
        clear = subtitle.index("(goalpad-face-prompt-touch-clear! self)", acceptance)
        toggle = subtitle.index("(not! (-> *pc-settings* subtitles?))", acceptance)
        self.assertLess(clear, toggle)
        self.assertIn("(else\n         (goalpad-face-prompt-touch-clear! self))", subtitle)

    def test_warp_cutscene_combat_and_jak2_are_excluded(self) -> None:
        self.assertNotIn("goalpad-face-prompt", BASEBUTTON.read_text())
        self.assertNotIn("goalpad-face-prompt", VILLAGEP.read_text())
        self.assertIn("goalpad-warp-gate-touch-publish!", BASEBUTTON.read_text())
        self.assertIn("goalpad-cutscene-skip-touch-publish!", self.process_taskable)

        authored_files = {
            path.relative_to(ROOT).as_posix()
            for path in ROOT.glob("goal_src/**/*.gc")
            if "goalpad-face-prompt" in path.read_text()
        }
        self.assertEqual(
            authored_files,
            {
                "goal_src/jak1/engine/common-obs/process-taskable.gc",
                "goal_src/jak1/levels/flut_common/flutflut.gc",
                "goal_src/jak1/levels/jungle/fisher.gc",
                "goal_src/jak1/levels/jungle/jungle-mirrors.gc",
                "goal_src/jak1/levels/misty/mistycannon.gc",
                "goal_src/jak1/levels/racer_common/racer.gc",
                "goal_src/jak1/levels/swamp/billy.gc",
                "goal_src/jak1/levels/village1/fishermans-boat.gc",
                "goal_src/jak1/levels/village3/village3-obs.gc",
                "goal_src/jak1/pc/subtitle.gc",
            },
        )


if __name__ == "__main__":
    unittest.main()
