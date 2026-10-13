import sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from check_target import check
class TargetIdentity(unittest.TestCase):
 def test_same_cpu_wrong_hardware_choice_is_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);(root/'zephyr').mkdir();p=root/'zephyr/.config'
   common='CONFIG_ASTER_TARGET_ID="xtensa"\nCONFIG_ASTER_CPU_ARCH_XTENSA=y\nCONFIG_ASTER_AOT_TARGET_ID=1\n'
   p.write_text(common+'CONFIG_ASTER_HW_WAVESHARE_35B=y\n')
   with self.assertRaisesRegex(ValueError,'hardware identity drift'):check('lckfb-s3',root)
   p.write_text(common+'CONFIG_ASTER_HW_LCKFB_S3=y\n');check('lckfb-s3',root)
   with self.assertRaisesRegex(ValueError,'hardware identity drift'):check('waveshare-35b',root)

 def test_build_root_cannot_silently_override_selected_profile(self):
  from firmware import check_build_fragments
  with tempfile.TemporaryDirectory() as d:
   build=Path(d);selected=build/'explicit.conf';selected.write_text('CONFIG_ASTER_HW_LCKFB_S3=y\n')
   (build/'aster-services.conf').write_text('')
   check_build_fragments(build,selected)
   (build/'leftover-waveshare.conf').write_text('CONFIG_ASTER_HW_WAVESHARE_35B=y\n')
   with self.assertRaisesRegex(ValueError,'auto-merges'):check_build_fragments(build,selected)
