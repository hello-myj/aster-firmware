#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import json
import sys
from pathlib import Path
source,target=map(Path,sys.argv[1:])
target.parent.mkdir(parents=True,exist_ok=True)
target.write_text('/* Generated. SPDX-License-Identifier: MIT */\nconst char aster_setup_page[]=\n'+
    '\n'.join(json.dumps(line,ensure_ascii=False) for line in source.read_text(encoding='utf-8').splitlines(keepends=True))+';\n',encoding='utf-8')
