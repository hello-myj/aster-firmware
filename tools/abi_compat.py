#!/usr/bin/env python3
"""Snapshot compiler-evaluated wasm32 ABI; compare against a reviewed baseline."""
import argparse, hashlib, json, re, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def run(cmd):
    return subprocess.run(cmd, text=True, capture_output=True, check=True).stdout

def snapshot(root, clang):
    root = Path(root).resolve()
    headers = root / 'contract/include'
    include = ''.join(f'#include <aster/{p.name}>\n' for p in sorted((headers/'aster').glob('*.h')))
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp)/'probe.c'; source.write_text(include)
        flags = [str(clang), '--target=wasm32-wasi', '-I'+str(headers)]
        tree = json.loads(run(flags+['-Xclang','-ast-dump=json','-fsyntax-only',str(source)]))
        nodes = {}
        def walk(n):
            if 'id' in n and (n.get('inner') or n['id'] not in nodes): nodes[n['id']] = n
            for child in n.get('inner',[]): walk(child)
        walk(tree)
        typedefs = {n['name']: n for n in nodes.values() if n.get('kind')=='TypedefDecl' and n.get('name','').startswith('aster_')}
        def record(n):
            for child in n.get('inner',[]):
                tag = child.get('ownedTagDecl') or child.get('decl')
                if tag and tag['kind']=='RecordDecl': return nodes[tag['id']]
                found=record(child)
                if found: return found
        records = {name:record(n) for name,n in typedefs.items() if record(n)}
        expressions = {}; metadata = {}
        def probe(key, expr): expressions[key]=expr
        for name in sorted(typedefs):
            probe('type:'+name+':size',f'sizeof({name})')
            probe('type:'+name+':align',f'_Alignof({name})')
        for name in ['int8_t','uint8_t','int16_t','uint16_t','int32_t','uint32_t','int64_t','uint64_t','float','double','void *','size_t']:
            probe('width:'+name, f'sizeof({name}) * 8')
        def fields(rec, prefix=''):
            result=[]
            for f in rec.get('inner',[]):
                if f.get('kind')!='FieldDecl':continue
                if not f.get('name'):raise ValueError('anonymous field requires explicit ABI probe support')
                path=prefix+f['name']; typ=f['type'].get('desugaredQualType',f['type']['qualType'])
                result.append((path,re.sub(r'\([^)]* at [^)]*\)','(anonymous)',typ)))
                nested=records.get(typ.removeprefix('struct ').removeprefix('union '))
                match=re.search(r' at .*:(\d+):(\d+)\)',typ)
                if match:
                    nested=next((r for r in nodes.values() if r.get('kind')=='RecordDecl' and r.get('completeDefinition') and r.get('loc',{}).get('line')==int(match[1]) and r.get('loc',{}).get('col')==int(match[2])),None)
                    if nested is None:raise ValueError('cannot resolve nested ABI field '+typ)
                if nested:result.extend(fields(nested,path+'.'))
            return result
        for name, rec in sorted(records.items()):
            metadata[name]={}
            for path, typ in fields(rec):
                key=f'field:{name}:{path}'
                probe(key+':offset',f'__builtin_offsetof({name}, {path})')
                probe(key+':size',f'sizeof((({name} *)0)->{path})')
                metadata[name][path]=typ
        enums=sorted({n['name'] for n in nodes.values() if n.get('kind')=='EnumConstantDecl' and n.get('name','').startswith('ASTER_')})
        for name in enums:probe('constant:'+name,name)
        macros=run(flags+['-dM','-E',str(source)])
        for line in macros.splitlines():
            m=re.match(r'#define (ASTER_\w+) (.+)$',line)
            if m and re.fullmatch(r'[\w\s()+*/%<>=!&|^~.,?:\-]+',m[2]) and not re.search(r'\b(__attribute__|extern|static|inline|void)\b',m[2]):
                probe('constant:'+m[1],m[1])
        keys=sorted(expressions)
        source.write_text(include+'\n'+''.join(f'const unsigned long long p{i} = (unsigned long long)({expressions[key]});\n' for i,key in enumerate(keys)))
        ir=run(flags+['-S','-emit-llvm','-o','-',str(source)])
        values={keys[int(i)]:int(v) for i,v in re.findall(r'@p(\d+) = .*?constant i64 (-?\d+)',ir)}
        if len(values)!=len(keys):raise ValueError('compiler did not evaluate every ABI probe')
    native=(root/'systems/zephyr/runtime/aster_native.c').read_text()
    signatures=dict(re.findall(r'\{\s*"(aster_[a-z0-9_]+)"\s*,[^,]+,\s*"([^"]+)"',native))
    prototypes={n['name']: n['type']['qualType'] for n in nodes.values() if n.get('kind')=='FunctionDecl' and n.get('name','').startswith('aster_') and n['name'] in signatures}
    libc=(root/'modules/wamr/core/iwasm/libraries/libc-builtin/libc_builtin_wrapper.c').read_text()
    table=libc.split('native_symbols_libc_builtin[] = {',1)[1].split('};',1)[0]
    libc_names=set(re.findall(r'REG_NATIVE_FUNC\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,',table))
    libc_names.update(re.findall(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,',table))
    imports={name:True for name in sorted(libc_names | signatures.keys())}
    version=(headers/'aster/system.h').read_text()
    abi='.'.join(re.search(r'#define\s+ASTER_ABI_'+k+r'\s+(\d+)',version)[1] for k in ['MAJOR','MINOR','PATCH'])
    return {'schema':1,'abi':abi,'target':'wasm32-wasi','imports':imports,'signatures':signatures,'prototypes':prototypes,'values':values,'fields':metadata,'package':json.loads((root/'contract/package-format.json').read_text()),'header_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((headers/'aster').glob('*.h'))}}

def compare(old,new):
    changes=[]
    for group in ['imports','signatures','prototypes','values','fields','package']:
        before=old[group];after=new[group]
        for key in sorted(set(before)|set(after)):
            if group=='values' and key.startswith('constant:ASTER_ABI_'):continue
            if before.get(key)==after.get(key) and (key in before)==(key in after):continue
            category='breaking'
            if key not in before and group!='package':category='requires-minimum-abi' if group in ['imports','signatures','prototypes'] else 'optional-addition'
            changes.append({'category':category,'group':group,'key':key,'before':before.get(key),'after':after.get(key)})
    if not changes:changes.append({'category':'internal-or-semantic-review','message':'No machine-visible ABI change; behavior still requires tests and review.'})
    breaking=any(c['category']=='breaking' for c in changes)
    needs_minor=any(c['category']=='requires-minimum-abi' for c in changes)
    o=tuple(map(int,old['abi'].split('.')));n=tuple(map(int,new['abi'].split('.')))
    errors=[]
    if breaking:errors.append('Breaking ABI/layout/package change: blocked even if version/hash is updated; use a reviewed new contract generation.')
    if needs_minor and (n[0]!=o[0] or n[1]<=o[1]):errors.append('New hard imports require a higher minimum ABI minor and APP compatibility review.')
    if n<o:errors.append('ABI version regressed')
    return {'schema':1,'baseline_abi':old['abi'],'candidate_abi':new['abi'],'changes':changes,'errors':errors,'semantic_review_required':True,'compatible':not errors}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,default=ROOT);p.add_argument('--clang',required=True);p.add_argument('--baseline',type=Path);p.add_argument('--snapshot',type=Path,required=True);p.add_argument('--report',type=Path)
    a=p.parse_args();current=snapshot(a.root,a.clang);a.snapshot.parent.mkdir(parents=True,exist_ok=True);a.snapshot.write_text(json.dumps(current,indent=2,sort_keys=True)+'\n')
    if a.baseline:
        result=compare(json.loads(a.baseline.read_text()),current)
        if not a.report:p.error('--baseline requires --report')
        a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2))
        return 0 if result['compatible'] else 1
    return 0
if __name__=='__main__':raise SystemExit(main())
