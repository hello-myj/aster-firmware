import json,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from release_check import require_packages
from quickstart_check import commands
class ReleaseChecks(unittest.TestCase):
 def test_missing_release_app_is_failure(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'missing.ast'
   with self.assertRaisesRegex(ValueError,'Required integration package missing'):require_packages([p])
 def test_documented_commands_use_actual_project_and_no_board_target(self):
  rows=commands(ROOT/'docs/quickstart.md',Path('/sdk'),Path('/sim'),Path('/project'),Path('/out'),'node','python')
  self.assertEqual(rows[0][2:4],['init','/project']);self.assertIn('xtensa',rows[0]);self.assertEqual(rows[1][-1],'--aot')
  self.assertEqual(rows[2][3],'/project/dist/xtensa/test.quickstart.ast');self.assertIn('--headless',rows[3])
 def test_old_package_hashes_are_fixed(self):
  import hashlib
  base=ROOT/'tests/fixtures/compatibility';j=json.loads((base/'manifest.json').read_text())
  for p in j['packages']:self.assertEqual(hashlib.sha256((base/p['file']).read_bytes()).hexdigest(),p['sha256'])
