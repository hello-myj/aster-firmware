import copy, json, sys, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from abi_compat import compare
class Compatibility(unittest.TestCase):
    def setUp(self):self.old=json.loads((ROOT/'contract/abi-baseline.json').read_text())
    def test_same_machine_contract_still_requires_semantic_review(self):
        r=compare(self.old,self.old);self.assertTrue(r['compatible']);self.assertTrue(r['semantic_review_required'])
    def test_layout_cannot_be_hidden_by_version_and_hash_change(self):
        n=copy.deepcopy(self.old);key=next(k for k in n['values'] if k.startswith('field:') and k.endswith(':offset'))
        n['values'][key]+=4;n['abi']='4.0.0';n['header_sha256']={}
        self.assertFalse(compare(self.old,n)['compatible'])
    def test_import_addition_requires_minor_raise(self):
        n=copy.deepcopy(self.old);n['signatures']['aster_future']='(i)i'
        self.assertFalse(compare(self.old,n)['compatible'])
        major, minor, _patch = (int(part) for part in self.old['abi'].split('.'))
        n['abi'] = f'{major}.{minor + 1}.0'
        self.assertTrue(compare(self.old,n)['compatible'])
    def test_signature_enum_integer_and_package_mutations_block(self):
        for group,key in [('signatures',next(iter(self.old['signatures']))),('values','constant:ASTER_EINVAL'),('values','width:uint32_t'),('package','header_sizes')]:
            n=copy.deepcopy(self.old);n[group][key]='changed'
            self.assertFalse(compare(self.old,n)['compatible'],(group,key))
    def test_optional_addition_classification(self):
        n=copy.deepcopy(self.old);n['values']['constant:ASTER_FEATURE_FUTURE']=31
        self.assertEqual(compare(self.old,n)['changes'][0]['category'],'optional-addition')
