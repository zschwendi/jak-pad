#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
LOADER = ROOT / "goal_src/jak2/engine/load/loader.gc"
GSOUND = ROOT / "goal_src/jak2/engine/sound/gsound.gc"
TITLE = ROOT / "goal_src/jak2/levels/title/title-obs.gc"


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


def selected_frame(
    current_frame: float,
    stream_frame: float,
    last_frame: float,
    movie_skip_frame: float,
    stream_is_playing: bool,
) -> float:
    if movie_skip_frame >= 0.0 or stream_is_playing:
        return max(0.0, min(stream_frame, last_frame))
    return max(0.0, min(current_frame, last_frame))


class Jak2SpooledAnimationSkipContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.loader = LOADER.read_text()
        cls.gsound = GSOUND.read_text()
        cls.title = TITLE.read_text()
        cls.play = extract_form(
            cls.loader, "(defbehavior ja-play-spooled-anim process-drawable"
        )
        cls.title_intro = extract_form(cls.title, "(scene-method-16")

    def test_synthetic_skip_position_drives_the_spooled_frame(self) -> None:
        selection = extract_form(self.play, "(f0-16 (if")
        self.assertIn(
            "(>= (-> *setting-control* user-current movie-skip-frame) 0.0)",
            selection,
        )
        self.assertIn("(str-id-is-playing? (the-as int (-> gp-0 sid)))", selection)
        self.assertLess(selection.index("movie-skip-frame"), selection.index("f26-0"))
        self.assertIn("(ja-frame-num 0)", selection)

    def test_title_skip_crosses_the_observed_last_block_tail_without_audio(self) -> None:
        title_startup = extract_form(self.title, "(defstate startup (title-control)")
        skip_command = extract_form(self.title_intro, "(1105")
        current_stream_position = extract_form(self.gsound, "(defun current-str-pos")
        self.assertIn("(set-setting! 'movie-skip-frame 'abs 1105.0 0)", title_startup)
        self.assertIn(
            "(send-event (handle->process (-> *game-info* controller 0)) 'pause)",
            skip_command,
        )
        self.assertIn("(set! (-> *game-info* demo-state) (the-as uint 2))", title_startup)
        self.assertIn(
            "(* 34.133335 (-> *setting-control* user-current movie-skip-frame))",
            current_stream_position,
        )

        artist_base = 1044.0
        skip_target = 1105.0
        local_skip_frame = skip_target - artist_base
        selected = selected_frame(
            current_frame=0.0,
            stream_frame=local_skip_frame,
            last_frame=80.0,
            movie_skip_frame=skip_target,
            stream_is_playing=False,
        )
        self.assertEqual(artist_base + selected, skip_target)

    def test_normal_frame_selection_is_unchanged(self) -> None:
        self.assertEqual(
            selected_frame(12.0, 61.0, 80.0, -1.0, False),
            12.0,
        )
        self.assertEqual(
            selected_frame(12.0, 61.0, 80.0, -1.0, True),
            61.0,
        )


if __name__ == "__main__":
    unittest.main()
