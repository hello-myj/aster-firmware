"""RGB565 coverage must not brighten identical colors or escape its endpoints."""
import unittest
from test_waveshare import ROOT, run_c
class Color565(unittest.TestCase):
 def test_all_channels_coverage_identity_symmetry_and_rounding(self):
  run_c(r'''
#include <assert.h>
#include "HEADER"
int main(void) {
 for(unsigned bits=5;bits<=6;bits++) {
  unsigned limit=(1u<<bits)-1,shift=bits==6?5:11;
  for(unsigned d=0;d<=limit;d++)for(unsigned s=0;s<=limit;s++)for(unsigned a=0;a<=255;a++) {
   uint16_t bg=d<<shift,fg=s<<shift;
   unsigned reference=(d*(255-a)+s*a+127)/255;
   assert(blend565(bg,fg,a)==(reference<<shift));
   assert(blend565(bg,fg,a)==blend565(fg,bg,255-a));
  }
 }
 for(unsigned c=0;c<=65535;c++)for(unsigned a=0;a<=255;a++)
  assert(blend565(c,c,a)==c);
 return 0;
}
'''.replace('HEADER',str(ROOT/'systems/zephyr/core/aster_color565.h')))
