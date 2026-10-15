"""Native ABI ownership/bounds and compressed-input wraparound under sanitizers."""
import pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class AudioStream(unittest.TestCase):
    def test_input_and_native_bridge(self):
        with tempfile.TemporaryDirectory() as d:
            exe = str(pathlib.Path(d)/'stream')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer','-g',
                '-I'+str(ROOT/'contract/include'),str(ROOT/'tests/audio_stream_host.c'),'-o',exe],check=True)
            subprocess.run([exe],check=True,timeout=10)
