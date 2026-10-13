import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import check_docs
import export_contract


class DocumentationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for folder in ('contract/include/aster', 'hardware', 'docs', 'boards/example'):
            (self.root / folder).mkdir(parents=True)
        (self.root / 'contract/include/aster/system.h').write_text(
            '#define ASTER_ABI_MAJOR 3u\n#define ASTER_ABI_MINOR 7u\n#define ASTER_ABI_PATCH 0u\n')
        for name in ('lckfb-s3', 'waveshare-35b'):
            (self.root / f'hardware/{name}.json').write_text(json.dumps({'public': {'id': name, 'cpu': 'xtensa'}}))
        self.architecture = '当前 ABI 为 3.7.0。\n<!-- hardware-models:start -->\n- `lckfb-s3`\n- `waveshare-35b`\n<!-- hardware-models:end -->'
        (self.root / 'docs/architecture.md').write_text(self.architecture)

    def test_current_operational_docs_match_contract(self):
        check_docs.check()

    def test_abi_and_new_hardware_require_documentation_updates(self):
        path = self.root / 'docs/architecture.md'
        path.write_text(self.architecture.replace('3.7.0', '3.4'))
        with self.assertRaisesRegex(ValueError, 'current ABI'):
            check_docs.check(self.root)
        path.write_text(self.architecture)
        (self.root / 'hardware/new.json').write_text(json.dumps({'public': {'id': 'new-board', 'cpu': 'xtensa'}}))
        with self.assertRaisesRegex(ValueError, 'hardware list'):
            check_docs.check(self.root)

    def test_board_build_dirs_are_valid_but_sdk_board_targets_fail(self):
        path = self.root / 'boards/example/README.md'
        path.write_text('python tools/firmware.py build --build-dir build/waveshare-35b\naster deploy dist/xtensa/app.ast --target xtensa')
        check_docs.check(self.root)
        for text in ('aster build APP --target waveshare-35b',
                     'aster deploy dist/waveshare-35b/app.ast --target xtensa'):
            path.write_text(text)
            with self.assertRaises(ValueError):
                check_docs.check(self.root)

    def test_bad_docs_abort_contract_export_without_writing_an_archive(self):
        output = self.root / 'dist/contract.zip'
        with patch.object(sys, 'argv', ['export_contract.py', '--output', str(output)]):
            with patch.object(check_docs, 'check', side_effect=ValueError('stale docs')):
                with self.assertRaisesRegex(ValueError, 'stale docs'):
                    export_contract.main()
        self.assertFalse(output.exists())
