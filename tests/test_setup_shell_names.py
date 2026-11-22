"""Keep the Zephyr shell's stringified command names usable from a terminal."""
from pathlib import Path
import re
import unittest


class SetupShellNames(unittest.TestCase):
    def test_setup_commands_are_single_shell_words(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'systems/zephyr/shell/aster_cmd.c').read_text(encoding='utf-8')
        table = source.split('sub_sys,', 1)[1].split('SHELL_SUBCMD_SET_END', 1)[0]
        names = re.findall(r'SHELL_CMD(?:_ARG)?\(\s*([^,]+?)\s*,', table)
        self.assertCountEqual([name for name in names if name.startswith('setup')],
                              ['setup', 'setup-source', 'setup-status'])
