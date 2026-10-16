"""Cue conversion must keep pitch, channel alignment and exact duration."""
import pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class AudioRate(unittest.TestCase):
 def test_conversion(self):
  with tempfile.TemporaryDirectory() as d:
   exe=str(pathlib.Path(d)/'rate')
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
     '-g',str(ROOT/'tests/audio_rate_host.c'),'-o',exe],check=True)
   subprocess.run([exe],check=True,timeout=10)
