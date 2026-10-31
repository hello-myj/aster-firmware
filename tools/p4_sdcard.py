#!/usr/bin/env python3
"""Stage fonts/packages for the experimental P4 port; never format or flash."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,default=ROOT/"build/osptek-p4/sd-desktop")
    parser.add_argument("--package",type=Path,action="append",default=[])
    parser.add_argument("--demo",action="store_true",help="Include the auto-start display diagnostic")
    args=parser.parse_args()
    sources=[(path,Path("aster/fonts")/path.name) for path in sorted((ROOT/"resources/fonts").glob("ui_*.cpfont"))]
    if not sources:parser.error("resources/fonts/ui_*.cpfont is missing")
    for name in ("NOTICE","DroidSansFallbackFull.LICENSE","faces.json"):
        path=ROOT/"resources/fonts"/name
        if path.is_file():sources.append((path,Path("aster/fonts")/name))
    for package in args.package:
        if package.suffix!=".ast" or not package.is_file():parser.error("package must be an existing .ast file")
        sources.append((package,Path("aster/install")/package.name))
    if args.demo:
        for name in ("app.wasm","app.aot"):
            path=ROOT/"build/osptek-p4/sdcard"/name
            if not path.is_file():parser.error("generate the display diagnostic with check_runtime.py first")
            sources.append((path,Path(name)))
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    manifest={"board":"osptek-p4-st7102","hardware_verified":False,"files":[]}
    for source,relative in sources:
        destination=output/relative;destination.parent.mkdir(parents=True,exist_ok=True)
        if destination.resolve()!=source.resolve():shutil.copy2(source,destination)
        digest=hashlib.sha256()
        with destination.open("rb") as file:
            for block in iter(lambda:file.read(1024*1024),b""):digest.update(block)
        manifest["files"].append({"path":relative.as_posix(),"bytes":destination.stat().st_size,"sha256":digest.hexdigest()})
    (output/"aster-staging.json").write_text(json.dumps(manifest,indent=2)+"\n",encoding="utf-8")
    print(f"Staged {len(sources)} files: {output}")
    print("Copy this directory's contents to a FAT SD card; install packages with 'aster app install'.")


if __name__=="__main__":main()
