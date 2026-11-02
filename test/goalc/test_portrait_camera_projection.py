#!/usr/bin/env python3
"""Source-level contract for the portrait (Vert+) gameplay camera in Jak 1 and Jak 2.

The checks parse the production `update-math-camera` forms and evaluate the frustum-slope
statements of the `with-pc` clauses with a deliberately tiny evaluator that rejects any form it
does not know. They do not prove that GOAL compiles the source or that AOT output matches it.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
JAK1_MATH_CAMERA = ROOT / "goal_src/jak1/engine/gfx/math-camera.gc"
JAK2_MATH_CAMERA = ROOT / "goal_src/jak2/engine/gfx/math-camera.gc"

ASPECT_4X3 = 4.0 / 3.0
ASPECT_16X9 = 16.0 / 9.0
CONSTANTS = {"ASPECT_4X3": ASPECT_4X3, "ASPECT_16X9": ASPECT_16X9}

# (source, math-camera argument name, expression for the field of view)
GAMES = {
    "jak1": (JAK1_MATH_CAMERA, "math-cam", ["->", "math-cam", "fov"]),
    "jak2": (JAK2_MATH_CAMERA, "arg0", "arg3"),
}

PORTRAIT_3X4 = 3.0 / 4.0
PORTRAIT_DUO = 1668.0 / 2388.0
WIDE_16X9 = ASPECT_16X9
ULTRAWIDE_21X9 = 21.0 / 9.0

# The gameplay clause before portrait support, kept only to prove the portrait assertions reject it.
OLD_GAMEPLAY_CLAUSE = """
(else
  (set! (-> CAM y-ratio) (* (1/ ASPECT_4X3) (-> CAM x-ratio)))
  (*! (-> CAM x-ratio) (/ (-> *pc-settings* aspect-ratio) ASPECT_4X3)))
"""

UNCHANGED_MOVIE_CLAUSE = """
((real-movie?)
  (if (<= (-> *pc-settings* aspect-ratio) ASPECT_16X9)
    (set! (-> CAM y-ratio) (* (1/ (-> *pc-settings* aspect-ratio)) (-> CAM x-ratio)))
    (begin
      (set! (-> CAM y-ratio) (* (1/ ASPECT_16X9) (-> CAM x-ratio)))
      (*! (-> CAM x-ratio) (/ (-> *pc-settings* aspect-ratio) ASPECT_16X9)))))
"""

TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|;[^\n]*|\(|\)|[^\s()";]+')
NUMBER = re.compile(r"-?\d+(?:\.\d+)?")


def parse(source: str) -> list:
    """Parse one s-expression, dropping `;` comments and keeping strings as single atoms."""
    stack: list[list] = [[]]
    for token in TOKEN.findall(source):
        if token.startswith(";"):
            continue
        if token == "(":
            stack.append([])
        elif token == ")":
            form = stack.pop()
            stack[-1].append(form)
        else:
            stack[-1].append(token)
    if len(stack) != 1 or len(stack[0]) != 1:
        raise AssertionError("expected exactly one balanced form")
    return stack[0][0]


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


def with_camera(template: str, camera: str) -> list:
    return parse(template.replace("CAM", camera))


class SlopeEvaluator:
    """Evaluates only the arithmetic subset used by the frustum-slope clauses."""

    def __init__(self, camera: str, x_ratio: float, y_ratio: float, aspect: float) -> None:
        self.objects = {
            camera: {"x-ratio": x_ratio, "y-ratio": y_ratio},
            "*pc-settings*": {"aspect-ratio": aspect},
        }

    def place(self, form) -> tuple[dict, str]:
        if not (isinstance(form, list) and len(form) == 3 and form[0] == "->"):
            raise AssertionError(f"unsupported place: {form}")
        _, owner, field = form
        if owner not in self.objects or field not in self.objects[owner]:
            raise AssertionError(f"unsupported field: {form}")
        return self.objects[owner], field

    def eval(self, form):
        if isinstance(form, str):
            if form in CONSTANTS:
                return CONSTANTS[form]
            if NUMBER.fullmatch(form):
                return float(form)
            raise AssertionError(f"unsupported atom: {form}")
        if not form or not isinstance(form[0], str):
            raise AssertionError(f"unsupported form: {form}")
        head, args = form[0], form[1:]
        if head == "->":
            owner, field = self.place(form)
            return owner[field]
        if head == "*" and len(args) >= 2:
            product = 1.0
            for arg in args:
                product *= self.eval(arg)
            return product
        if head == "/" and len(args) == 2:
            return self.eval(args[0]) / self.eval(args[1])
        if head == "1/" and len(args) == 1:
            return 1.0 / self.eval(args[0])
        if head == "<" and len(args) == 2:
            return self.eval(args[0]) < self.eval(args[1])
        if head == "<=" and len(args) == 2:
            return self.eval(args[0]) <= self.eval(args[1])
        if head == "if" and len(args) in (2, 3):
            if self.eval(args[0]):
                return self.eval(args[1])
            return self.eval(args[2]) if len(args) == 3 else None
        if head == "begin" and args:
            for arg in args:
                self.eval(arg)
            return None
        if head == "set!" and len(args) == 2:
            owner, field = self.place(args[0])
            owner[field] = self.eval(args[1])
            return None
        if head == "*!" and len(args) == 2:
            owner, field = self.place(args[0])
            owner[field] = owner[field] * self.eval(args[1])
            return None
        raise AssertionError(f"unsupported form: {form}")

    def run(self, body: list, camera: str) -> tuple[float, float]:
        for form in body:
            self.eval(form)
        return self.objects[camera]["x-ratio"], self.objects[camera]["y-ratio"]


def old_hor_plus(x0: float, aspect: float) -> tuple[float, float]:
    return x0 * aspect / ASPECT_4X3, x0 / ASPECT_4X3


def old_movie(x0: float, aspect: float) -> tuple[float, float]:
    if aspect <= ASPECT_16X9:
        return x0, x0 / aspect
    return x0 * aspect / ASPECT_16X9, x0 / ASPECT_16X9


class PortraitCameraProjectionTest(unittest.TestCase):
    X0 = 0.7673269879789604  # tan(0.5 * 75 degrees), a representative gameplay half-FOV slope

    @classmethod
    def setUpClass(cls) -> None:
        cls.clauses = {}
        for game, (path, camera, fov) in GAMES.items():
            update = parse(extract_form(path.read_text(), "(defun update-math-camera"))
            initial = [
                form
                for form in update
                if isinstance(form, list) and form[:2] == ["set!", ["->", camera, "x-ratio"]]
            ]
            assert initial == [
                ["set!", ["->", camera, "x-ratio"], ["tan", ["*", "0.5", fov]]]
            ], f"{game} initial horizontal slope changed: {initial}"
            with_pc = [form for form in update if isinstance(form, list) and form[:1] == ["with-pc"]]
            assert len(with_pc) == 1 and len(with_pc[0]) == 2, f"{game} with-pc shape changed"
            cond = with_pc[0][1]
            assert cond[0] == "cond", f"{game} with-pc no longer wraps a single cond"
            cls.clauses[game] = (camera, cond[1:])

    def clause(self, game: str, test) -> list:
        _, clauses = self.clauses[game]
        matches = [clause for clause in clauses if clause[0] == test]
        self.assertEqual(len(matches), 1, f"{game} clause {test}")
        return matches[0]

    def gameplay(self, game: str, aspect: float) -> tuple[float, float]:
        camera, _ = self.clauses[game]
        body = self.clause(game, "else")[1:]
        return SlopeEvaluator(camera, self.X0, self.X0 * 0.75, aspect).run(body, camera)

    def assertSlopes(self, actual: tuple[float, float], expected: tuple[float, float]) -> None:
        self.assertAlmostEqual(actual[0], expected[0], places=12)
        self.assertAlmostEqual(actual[1], expected[1], places=12)

    def test_cond_keeps_exactly_the_vis_movie_and_gameplay_clauses(self) -> None:
        for game in GAMES:
            with self.subTest(game=game):
                _, clauses = self.clauses[game]
                self.assertEqual(
                    [clause[0] for clause in clauses],
                    [["->", "*pc-settings*", "use-vis?"], ["real-movie?"], "else"],
                )

    def test_vis_clause_still_leaves_the_slopes_untouched(self) -> None:
        for game in GAMES:
            with self.subTest(game=game):
                self.assertEqual(self.clause(game, ["->", "*pc-settings*", "use-vis?"])[1:], [])

    def test_movie_clause_is_unchanged(self) -> None:
        for game in GAMES:
            with self.subTest(game=game):
                camera, _ = self.clauses[game]
                movie = self.clause(game, ["real-movie?"])
                self.assertEqual(movie, with_camera(UNCHANGED_MOVIE_CLAUSE, camera))
                for aspect in (PORTRAIT_3X4, ASPECT_4X3, WIDE_16X9, ULTRAWIDE_21X9):
                    evaluator = SlopeEvaluator(camera, self.X0, self.X0 * 0.75, aspect)
                    self.assertSlopes(evaluator.run(movie[1:], camera), old_movie(self.X0, aspect))

    def test_portrait_gameplay_keeps_horizontal_slope_and_extends_vertically(self) -> None:
        for game in GAMES:
            for aspect in (PORTRAIT_3X4, PORTRAIT_DUO, 0.5):
                with self.subTest(game=game, aspect=aspect):
                    x_ratio, y_ratio = self.gameplay(game, aspect)
                    self.assertEqual(x_ratio, self.X0)
                    self.assertAlmostEqual(y_ratio, self.X0 / aspect, places=12)
                    self.assertGreater(y_ratio, self.X0 * 0.75)
        x_ratio, y_ratio = self.gameplay("jak1", PORTRAIT_3X4)
        self.assertAlmostEqual(y_ratio, self.X0 * 4.0 / 3.0, places=12)

    def test_classic_4x3_gameplay_is_the_baseline(self) -> None:
        for game in GAMES:
            with self.subTest(game=game):
                self.assertSlopes(self.gameplay(game, ASPECT_4X3), (self.X0, self.X0 * 0.75))

    def test_wide_gameplay_keeps_the_existing_hor_plus_projection(self) -> None:
        for game in GAMES:
            for aspect in (2388.0 / 1668.0, WIDE_16X9, ULTRAWIDE_21X9, 32.0 / 9.0):
                with self.subTest(game=game, aspect=aspect):
                    self.assertSlopes(self.gameplay(game, aspect), old_hor_plus(self.X0, aspect))

    def test_portrait_assertions_reject_the_previous_gameplay_clause(self) -> None:
        for game in GAMES:
            with self.subTest(game=game):
                camera, _ = self.clauses[game]
                old_body = with_camera(OLD_GAMEPLAY_CLAUSE, camera)[1:]
                evaluator = SlopeEvaluator(camera, self.X0, self.X0 * 0.75, PORTRAIT_3X4)
                x_ratio, y_ratio = evaluator.run(old_body, camera)
                self.assertNotAlmostEqual(x_ratio, self.X0, places=6)
                self.assertNotAlmostEqual(y_ratio, self.X0 / PORTRAIT_3X4, places=6)
                self.assertNotEqual(self.clause(game, "else"), with_camera(OLD_GAMEPLAY_CLAUSE, camera))

    def test_evaluator_rejects_unknown_forms(self) -> None:
        evaluator = SlopeEvaluator("arg0", self.X0, self.X0, ASPECT_4X3)
        for form in ("(sqrtf 2.0)", "(-> arg0 fov)", "(set! (-> arg0 d) 1.0)", "UNKNOWN"):
            with self.subTest(form=form):
                with self.assertRaises(AssertionError):
                    evaluator.eval(parse(form) if form.startswith("(") else form)


if __name__ == "__main__":
    unittest.main()
