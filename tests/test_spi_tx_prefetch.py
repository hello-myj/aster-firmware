"""Execute the actual added staging/next-copy/cleanup blocks from the west patch."""
import unittest
from test_waveshare import ROOT,run_c

class SpiPrefetch(unittest.TestCase):
 def test_pipeline_bytes_ownership_budget_and_fallback(self):
  patch=(ROOT/'patches/aster_zephyr_v4.4.2_tx_prefetch.patch').read_text()
  added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
  start=added.index('if(data->tx_prefetch_enabled &&')
  prepare=added[start:added.index('\n#endif',start)]
  start=added.index('if(staged && ctx->tx_buf')
  ahead=added[start:added.index('\n#endif',start)]
  toggle=next(line for line in added.splitlines() if 'if(staged) data->tx_stage_index^=' in line)
  cleanup=next(line for line in added.splitlines() if 'k_free(data->tx_stages);' in line)
  run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
struct context {const uint8_t *tx_buf,*rx_buf;size_t tx_len;unsigned tx_count,rx_count;};
struct data {struct context ctx;bool tx_prefetch_enabled;size_t dma_chunk,dfs;uint8_t *tx_stages;size_t tx_stage_bytes;uint8_t tx_stage_index;};
struct config {bool dma_enabled;};
static bool failed,internal,onwire,large_bands;
#define CONFIG_SPI_ESP32_TX_PREFETCH_LARGE_BANDS large_bands
#define IS_ENABLED(x) (x)
static const void *active;static size_t peak;static int live;
static void *k_malloc(size_t n){if(failed)return NULL;peak=n;live++;return malloc(n);}
static void k_free(void *p){assert(!onwire);if(p){live--;free(p);}}
static bool esp_ptr_dma_capable(const void *p){(void)p;return internal;}
static void *copy(void *dst,const void *src,size_t n){assert(!onwire||dst!=active);return __builtin_memcpy(dst,src,n);}
#define memcpy copy
static void setup(struct data *data,struct config *cfg){
'''+prepare+r'''
}
static void next(struct data *data){struct context *ctx=&data->ctx;bool staged=data->tx_stages!=NULL;
'''+ahead+r'''
}
static void complete(struct data *data){bool staged=data->tx_stages!=NULL;
'''+toggle+r'''
}
static void release(struct data *data){
'''+cleanup+r'''
}
int main(void){
 static uint8_t source[307200];uint32_t seed=42;
 for(size_t i=0;i<sizeof(source);i++){seed=seed*1664525u+1013904223u;source[i]=seed>>24;}
 struct config cfg={true};
 for(unsigned variant=0;variant<2;variant++){large_bands=variant;peak=0;
 struct data d={.ctx={source,NULL,sizeof(source),1,0},.tx_prefetch_enabled=true,.dma_chunk=16384,.dfs=1};
 setup(&d,&cfg);assert(d.tx_stages&&peak==(variant?32768:16384)&&d.tx_stage_bytes==(variant?16384:8192)&&live==1);
 size_t position=0;unsigned blocks=0;
 while(d.ctx.tx_len){
  size_t n=MIN(d.ctx.tx_len,d.tx_stage_bytes);const uint8_t *wire=d.tx_stages+d.tx_stage_index*d.tx_stage_bytes;
  assert(!memcmp(wire,source+position,n));active=wire;onwire=true;
  d.ctx.tx_len-=n;d.ctx.tx_buf=d.ctx.tx_len?source+position+n:NULL;
  next(&d);assert(!memcmp(wire,source+position,n));onwire=false;complete(&d);position+=n;blocks++;
 }
 assert(position==sizeof(source)&&blocks==(variant?19:38));release(&d);assert(!d.tx_stages&&!d.tx_stage_bytes&&!live);
 d.ctx=(struct context){source,NULL,sizeof(source),1,0};failed=true;setup(&d,&cfg);assert(!d.tx_stages&&d.ctx.tx_len==sizeof(source));failed=false;
 d.dma_chunk=8192;setup(&d,&cfg);assert(d.tx_stage_bytes==(variant?8192:4096)&&peak==(variant?16384:8192));release(&d);
 d.dma_chunk=4092;setup(&d,&cfg);assert(!d.tx_stages);
 d.dma_chunk=16384;d.ctx.tx_count=2;setup(&d,&cfg);assert(!d.tx_stages);
 d.ctx.tx_count=1;d.ctx.rx_buf=source;setup(&d,&cfg);assert(!d.tx_stages);
 d.ctx.rx_buf=NULL;d.ctx.tx_len=100;setup(&d,&cfg);assert(!d.tx_stages);
 d.ctx.tx_len=sizeof(source);internal=true;setup(&d,&cfg);assert(!d.tx_stages&&!live);internal=false;
 }
 return 0;
}
''')
