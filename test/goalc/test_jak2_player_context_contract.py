import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
READER = ROOT / "game/kernel/core/jak2_player_context_reader.h"
RUNTIME = ROOT / "game/kernel/core/jak2_runtime.cpp"
RUNTIME_HEADER = ROOT / "game/kernel/core/jak2_runtime.h"
GAME_CMAKE = ROOT / "game/CMakeLists.txt"


class Jak2PlayerContextContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reader = READER.read_text()
        cls.runtime = RUNTIME.read_text()
        cls.runtime_header = RUNTIME_HEADER.read_text()
        cls.game_cmake = GAME_CMAKE.read_text()

    def test_reader_pins_the_audited_target_layout(self):
        self.assertIn("constexpr std::size_t kTargetSize = 0x8b8;", self.reader)
        self.assertIn("constexpr std::size_t kFocusStatus = 196;", self.reader)
        self.assertIn("constexpr std::size_t kCamUserMode = 296;", self.reader)
        self.assertIn("constexpr uint32_t kPilotRiding = 1u << 14;", self.reader)
        self.assertIn("constexpr uint32_t kBoard = 1u << 18;", self.reader)
        self.assertIn("constexpr uint32_t kPilot = 1u << 20;", self.reader)

    def test_runtime_reads_the_direct_target_and_exact_symbols(self):
        self.assertIn('inputs.target = symbol_value_if_present("*target*");', self.runtime)
        self.assertNotIn('pointer_symbol_process("*target*")', self.runtime)
        self.assertIn(
            'inputs.target_type_symbol = goal_game_find_symbol("target", &inputs.target_type);',
            self.runtime,
        )
        self.assertIn('inputs.normal_symbol = goal_game_find_symbol("normal", nullptr);', self.runtime)
        self.assertIn(
            'inputs.look_around_symbol = goal_game_find_symbol("look-around", nullptr);',
            self.runtime,
        )

    def test_typed_c_abi_and_focused_unit_target_are_wired(self):
        self.assertIn("typedef enum goal_jak2_player_traversal_state", self.runtime_header)
        self.assertIn("typedef enum goal_jak2_player_look_state", self.runtime_header)
        self.assertIn("typedef struct goal_jak2_player_context_snapshot", self.runtime_header)
        self.assertIn("goal_jak2_runtime_get_player_context_snapshot(", self.runtime_header)
        self.assertIn("add_executable(jak2-player-context-reader-test", self.game_cmake)
        self.assertIn("add_test(NAME jak2-player-context-reader-test", self.game_cmake)


if __name__ == "__main__":
    unittest.main()
