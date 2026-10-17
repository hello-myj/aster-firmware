#!/usr/bin/env python3
"""Verify an extracted complete SDK, then create/build/run an independent APP.

Run on its matching host with --sdk EXTRACTED/aster-sdk --output OUTSIDE_GIT.
Uses only the packaged APP compiler, runtime, simulator and bundled dependencies.
Linux requires the SDK's documented system Python baseline; Windows embeds Python.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    sdk, out = a.sdk.resolve(), a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    release = json.loads((sdk/'release.json').read_text())
    kit = json.loads((sdk/'development-kit.json').read_text())
    lock = json.loads((sdk/'contract/lock.json').read_text())
    assert release['abi'] == kit['abi'] == lock['abi']
    assert digest(sdk/'contract/api.json') == kit['contract_api_sha256']
    files = 0
    for line in (sdk/'SHA256SUMS.txt').read_text().splitlines():
        checksum, relative = line.split('  ', 1)
        path = (sdk/relative).resolve()
        assert path.is_relative_to(sdk) and digest(path) == checksum, relative
        files += 1
    for image in kit['components']['firmware']['images'].values():
        assert digest(sdk/image['path']) == image['sha256']
        identity = json.loads((sdk/image['build_identity']).read_text())
        assert identity['abi'] == kit['abi']
        assert identity['source']['commit'] == kit['components']['firmware']['revision']
    compatibility = json.loads((sdk/'simulator/src/sdk-compat.json').read_text())
    assert compatibility['abi'] == kit['abi']
    assert compatibility['sdkCommit'] == release['commit']
    assert release['development_kit']['components'] == kit['components']
    windows = sys.platform == 'win32'
    cli = sdk/('aster.exe' if windows else 'aster')
    project = out/'independent app'
    def run(*argv):
        result = subprocess.run([str(v) for v in argv], check=True, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=180)
        print(result.stdout, end='', flush=True)
        print(result.stderr, end='', flush=True)
        return result
    run(cli, '--version-json')
    run(cli, 'init', project, '--template', 'hello', '--id', 'aster.validation.kithello', '--target', 'xtensa')
    run(cli, 'build', project, '--aot')
    app = project/'dist/xtensa/aster.validation.kithello.ast'
    run(cli, 'verify', app, '--target', 'xtensa')
    runtime = sdk/'simulator/runtime'/('node.exe' if windows else 'node')
    diagnostic = out/'simulator.json'
    result = run(runtime, sdk/'simulator/src/cli.js', app, '--headless', '--frames', '5',
                 '--diagnostics', diagnostic)
    assert 'application lifecycle passed' in result.stdout
    result = {'status':'passed','host':sys.platform,'files_verified':files,
              'abi':kit['abi'],'sdk_revision':release['commit'],
              'firmware_revision':kit['components']['firmware']['revision'],
              'simulator_revision':kit['components']['simulator']['revision'],
              'scope':'Independent APP compile/AOT verification and WASM model execution; not device emulation'}
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
