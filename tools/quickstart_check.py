"""Read the exact argument arrays documented in the quickstart, without a shell."""
import json,re
from pathlib import Path
def commands(document,sdk,sim,project,output,node,python):
    match=re.search(r'<!-- quickstart-check:start -->\s*```json\s*(.*?)\s*```\s*<!-- quickstart-check:end -->',Path(document).read_text(),re.S)
    if not match:raise ValueError('Quickstart executable command block missing')
    rows=json.loads(match[1]);values={'python':str(python),'sdk':str(sdk),'sim':str(sim),'project':str(project),'output':str(output),'node':str(node)}
    if not rows or any(not isinstance(row,list) or not row or any(not isinstance(x,str) for x in row) for row in rows):raise ValueError('Invalid quickstart argument arrays')
    return [[arg.format(**values) for arg in row] for row in rows]
