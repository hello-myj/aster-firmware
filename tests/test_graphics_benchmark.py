import importlib.util
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('graphics_benchmark',Path(__file__).resolve().parents[1]/'tools/graphics_benchmark.py')
bench=importlib.util.module_from_spec(spec);spec.loader.exec_module(bench)

class GraphicsBenchmark(unittest.TestCase):
 def test_completion_requires_matching_mode_and_complete_intervals(self):
  line='GFXBENCH DONE mode=smallring frames=128 draw_total_ms=256 draw_max_ms=4 interval_total_ms=381 intervals=127 gfx_calls=800 errors=0'
  result=bench.parse_done(line,'smallring');self.assertEqual(result['steady_frame_abi_calls'],1056)
  for value in (line.replace('intervals=127','intervals=126'),line.replace('errors=0','errors=1')):
   with self.assertRaises(ValueError):bench.parse_done(value,'smallring')
  with self.assertRaises(ValueError):bench.parse_done(line,'text')
 def test_unrelated_scenes_can_have_different_pixels(self):
  rows=[{'mode':m,'screenshot_rgb565_sha256':v} for m,v in [('individual','a'),('batched','a'),('text','b'),('smallring','b'),('image','c'),('partial','d')]]
  self.assertEqual(len(bench.verify_pixels(rows)),2)
  rows[-3]['screenshot_rgb565_sha256']='bad'
  with self.assertRaises(ValueError):bench.verify_pixels(rows)
 def test_failed_transfer_and_wait_are_not_hidden(self):
  line='axs tim: n=64 avg_rect=128 B avg_completed_panel=0 B totals wait=10 us gather=20 us cache=30 us write=40 us errors=64'
  self.assertEqual(bench.parse_profiler(line)['axs_transfer_samples'],[[128,0,10,20,30,40,64]])
  self.assertEqual(bench.parse_profiler('')['axs_transfer_samples'],[])

 def test_image_fixture_paths_resolve_to_bundled_assets(self):
  import re,tomllib
  root=Path(__file__).resolve().parents[1]/'tests/fixtures/graphics'
  for mode in ('image','resources'):
   directory=root/mode
   manifest=tomllib.loads((directory/'app.toml').read_text())['app']
   source=(directory/'src/main.c').read_text()
   paths=re.findall(r'"(/assets/[^"\n]+)"\s*,\s*(\d+)u?',source)
   self.assertTrue(paths)
   for path,length in paths:
    self.assertEqual(len(path.encode()),int(length))
    relative=path.removeprefix('/assets/')
    self.assertIn(relative,manifest['assets'])
    self.assertTrue((directory/relative).is_file())

 def test_capture_retries_transport_but_not_missing_frame(self):
  from unittest.mock import patch
  events=[];calls=[]
  def capture(port):
   calls.append(port)
   if len(calls)==1:raise RuntimeError('missing screenshot chunks: 1/2')
   return (1,1,b'\0\0')
  with patch.object(bench.time,'sleep'):
   self.assertEqual(bench.capture_checked(capture,'serial',lambda *e:events.append(e)),(1,1,b'\0\0'))
   self.assertEqual(len(events),1)
   def absent(port):raise RuntimeError('SHOT err no-frame')
   with self.assertRaisesRegex(RuntimeError,'no-frame'):bench.capture_checked(absent,'serial',lambda *e:events.append(e))
   def broken(port):raise RuntimeError('screenshot length or CRC mismatch')
   with self.assertRaisesRegex(RuntimeError,'CRC'):bench.capture_checked(broken,'serial',lambda *e:events.append(e))
   self.assertEqual(len(events),3)
