#!/usr/bin/env python3
"""Fail-closed local/CI integration of pinned firmware, SDK and simulator."""
import argparse, hashlib, json, os, re, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def identity(root):
    def git(*a):return subprocess.check_output(['git','-C',str(root),*a],text=True).strip()
    files=subprocess.check_output(['git','-C',str(root),'ls-files','-z','--modified','--others','--exclude-standard']).decode().split('\0')
    return {'commit':git('rev-parse','HEAD'),'dirty':bool(git('status','--porcelain')),'changed_files':{f:digest(root/f) for f in sorted(set(files)) if f and (root/f).is_file()}}
def require_packages(paths):
    for p in paths:
        if not Path(p).is_file():raise ValueError('Required integration package missing: '+str(p))
def require_no_skips(log, hint):
    if re.search(r"\bskipped\s*=\s*[1-9]\d*", Path(log).read_text()):
        raise ValueError(hint)
def command(args, cwd, env, log):
    with log.open('w') as f:subprocess.run([str(a) for a in args],cwd=cwd,env=env,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=600)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',type=Path,required=True);p.add_argument('--sim',type=Path,required=True);p.add_argument('--node',default='node');p.add_argument('--output',type=Path,default=ROOT/'build/release-check');p.add_argument('--require-clean',action='store_true');p.add_argument('--matrix',type=Path,default=ROOT/'contract/release-matrix.json',help='Reviewed published or local candidate matrix; does not modify the published baseline');p.add_argument('--baseline',type=Path,default=ROOT/'contract/abi-baseline.json')
    a=p.parse_args();sdk=a.sdk.resolve();sim=a.sim.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);env=dict(os.environ)
    report={'schema':1,'status':'running','hardware':{'status':'not_run','reason':'Integration does not flash hardware.'},'steps':[]}
    def save(): (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    def step(name,args,cwd=ROOT,environment=env):
        report['steps'].append({'name':name,'status':'running'});save()
        command(args,cwd,environment,out/(name+'.log'));report['steps'][-1]['status']='passed';save()
    try:
        lock=json.loads(a.matrix.read_text());report['matrix']={'path':str(a.matrix.resolve()),'sha256':digest(a.matrix)};report['sources']={name:identity(root) for name,root in [('firmware',ROOT),('sdk',sdk),('simulator',sim)]}
        for name in ['sdk','simulator']:
            if report['sources'][name]['commit']!=lock[name]['revision']:raise ValueError('Source revision differs from reviewed matrix: '+name)
        if a.require_clean and any(r['dirty'] for r in report['sources'].values()):raise ValueError('Release requires clean checkouts; local dirty validation is not a release')
        profile=json.loads((sdk/'toolchains/catalog.json').read_text())['profiles'][lock['toolchain_profile']]
        host=profile['hosts']['linux-x64'];wamrc=sdk/host['wamrc'];clang=sdk/host['wasi']/'bin/clang'
        if digest(wamrc)!=host['wamrc_sha256']:raise ValueError('AOT compiler hash mismatch')
        if hashlib.sha256((sdk/'toolchains/catalog.json').read_bytes()).hexdigest()!=lock['toolchain_catalog_sha256']:raise ValueError('Toolchain matrix changed without review')
        report['toolchain']={'profile':lock['toolchain_profile'],'wamrc_sha256':digest(wamrc),'clang_sha256':digest(clang),'clang_version':subprocess.check_output([str(clang),'--version'],text=True),'node':subprocess.check_output([a.node,'--version'],text=True).strip()}
        if int(report['toolchain']['node'].lstrip('v').split('.')[0])<22:raise ValueError('Simulator release checks require Node >=22')
        step('firmware-docs',[sys.executable,ROOT/'tools/check_docs.py'])
        step('sdk-docs',[sys.executable,sdk/'tools/check_docs.py'],sdk)
        step('abi',[sys.executable,ROOT/'tools/abi_compat.py','--clang',clang,'--baseline',a.baseline,'--snapshot',out/'abi.json','--report',out/'abi-report.json'])
        # Contract equality includes real header bytes, not just the ABI string/hash file.
        sdk_lock=json.loads((sdk/'contract/lock.json').read_text())
        for file,sha in sdk_lock['sha256'].items():
            if digest(sdk/file)!=sha:raise ValueError('SDK lock mismatch '+file)
        for header in (ROOT/'contract/include/aster').glob('*.h'):
            if header.read_bytes()!=(sdk/'include/aster'/header.name).read_bytes():raise ValueError('Firmware/SDK header mismatch '+header.name)
        from export_contract import bundle_files
        if bundle_files()['contract/api.json']!=(sdk/'contract/api.json').read_bytes():raise ValueError('Firmware/SDK runtime API mismatch')
        step('host-build',[sys.executable,ROOT/'tools/host.py'])
        step('firmware-tests',[sys.executable,'-m','unittest','discover','-s',ROOT/'tests'])
        require_no_skips(out/'firmware-tests.log','Firmware release tests skipped: install requirements-ci.txt')
        step('sdk-tests',[sys.executable,'-m','unittest','discover','-s',sdk/'tests'],sdk)
        require_no_skips(out/'sdk-tests.log','SDK release tests skipped: install SDK requirements-assets.txt')
        step('fonts',[sys.executable,ROOT/'tools/font_metrics.py','--check'])
        step('resources',[sys.executable,ROOT/'tools/resources.py','--output',out/'sdcard'])
        report['resources']={str(p.relative_to(out/'sdcard')):digest(p) for p in (out/'sdcard').rglob('*') if p.is_file()}
        step('sim-contract',[a.node,sim/'scripts/check-sdk.js',sdk],sim)
        # Follow the documented quickstart commands from the same machine-readable block.
        project=out/'quickstart-app'
        if project.exists():raise ValueError('Use a fresh --output directory; do not reuse generated quickstart input')
        from quickstart_check import commands
        for i,args in enumerate(commands(ROOT/'docs/quickstart.md',sdk,sim,project,out,a.node,sys.executable)):
            step('quickstart-'+str(i+1),args)
        # Required release APPs are built from SDK source plus a fixed reference source.
        build_env=dict(env,ASTER_WORKSPACE=str(out/'apps'))
        step('sdk-examples',[sys.executable,sdk/'tools/build.py',sdk/'apps/game.2048',sdk/'apps/imucube',ROOT/'tests/fixtures/compatibility/breakout-source',sdk/'apps/scene-settings',sdk/'apps/scene-sprites','--target','xtensa','--aot'],sdk,build_env)
        packages=out/'apps/dist/xtensa'
        required={key:packages/name for key,name in lock['required_packages'].items()}
        # Scene is mandatory for current SDK even when validating with an older matrix.
        required.update(ASTER_SIM_SCENE_SETTINGS_AST=packages/'aster.scene.settings.ast',ASTER_SIM_SCENE_SPRITES_AST=packages/'aster.scene.sprites.ast')
        require_packages(required.values())
        parser_env=dict(env,ASTER_COMPAT_PACKAGES=json.dumps([str(x) for x in required.values()]))
        step('candidate-firmware-parser',[sys.executable,'-m','unittest','discover','-s',ROOT/'tests','-p','test_old_packages.py'],ROOT,parser_env)
        frozen=ROOT/'tests/fixtures/compatibility';inventory=json.loads((frozen/'manifest.json').read_text());require_packages(frozen/x['file'] for x in inventory['packages'])
        for item in inventory['packages']:
            if digest(frozen/item['file'])!=item['sha256']:raise ValueError('Frozen old package changed: '+item['file'])
            step('verify-old-'+Path(item['file']).stem,[sys.executable,sdk/'tools/aster.py','verify',frozen/item['file'],'--target','xtensa'],sdk)
        test_env=dict(env,ASTER_SIM_REQUIRE_PACKAGES='1',ASTER_SIM_REQUIRE_SCENE_PACKAGES='1',**{key:str(value) for key,value in required.items()})
        step('simulator-tests',[a.node,'--test',*sorted((sim/'test').glob('*.test.js'))],sim,test_env)
        summary=(out/'simulator-tests.log').read_text()
        skipped=re.search(r'^# skipped (\d+)$',summary,re.M)
        if not skipped or int(skipped[1]):raise ValueError('Release simulator tests require zero skipped cases')
        report['simulator_tests']={'skipped':0,'passed':int(re.search(r'^# pass (\d+)$',summary,re.M)[1])}
        # Old SDK WASM payloads are executed; simulator cannot validate their Xtensa AOT.
        for item in inventory['packages']:
            step('run-old-'+Path(item['file']).stem,[a.node,sim/'src/cli.js',frozen/item['file'],'--headless','--frames','5'],sim)
        report['packages']={str(path.relative_to(out)):digest(path) for path in [project/'dist/xtensa/test.quickstart.ast',*required.values()]}
        report['frozen_packages']=inventory
        report['status']='passed';save();print(out/'report.json');return 0
    except Exception as e:
        report['status']='failed';report['error']=str(e)
        if report['steps'] and report['steps'][-1]['status']=='running':report['steps'][-1]['status']='failed'
        save();print(str(e),file=sys.stderr);return 1
if __name__=='__main__':raise SystemExit(main())
