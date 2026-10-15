#!/usr/bin/env python3
"""Generate a bounded MP3 stream-check APP outside the source tree.

Use a legally available local MP3 with >=2s of audio. No music is bundled into
Git. The APP exercises actual decoder, backpressure, pause and exit cleanup;
it deliberately leaves the last empty stream open so runtime cleanup is tested.
"""
import argparse
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--sample', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    data = args.sample.read_bytes()
    if not 65536 <= len(data) <= 262144:
        p.error('MP3 sample must be 64..256 KiB (may be a prefix of a larger MP3)')
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        p.error('output must be an empty directory')
    (output/'src').mkdir(parents=True, exist_ok=True)
    (output/'src/main.c').write_bytes(Path(__file__).with_suffix('.c').read_bytes())
    (output/'src/sample.inc').write_text(','.join(map(str, data)))
    (output/'app.toml').write_text('''[app]
id="test.audio.stream"
name="Stream Check"
version="1.0.0"
abi="3.10.0"
abi_min="3.10"
aot=true
stack=16384
initial_memory=524288
max_memory=1048576
heap=65536
requires=["audio-out","audio-stream"]
sources=["src/main.c"]
''')
    subprocess.run(['python3', str(args.sdk.resolve()/'tools/build.py'),
                    str(output), '--target', 'xtensa'], check=True)


if __name__ == '__main__':
    main()
