"""Load the real AOT text header across deliberately incoherent bus aliases."""
import unittest
from test_waveshare import ROOT, run_c


class AOTText(unittest.TestCase):
    def test_dual_bus_header_uses_data_alias_and_checks_literal_bounds(self):
        source = (ROOT / 'modules/wamr/core/iwasm/aot/aot_loader.c').read_text()
        start = source.index('static bool\nload_text_section(')
        function = source[start:source.index('static bool\nload_function_section(', start)]
        for mirror in (0, 1):
            with self.subTest(dual_bus=mirror):
                run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define WASM_ENABLE_DEBUG_AOT 0
#define WASM_MEM_DUAL_BUS_MIRROR ''' + str(mirror) + r'''
typedef uint8_t uint8;
typedef uint32_t uint32;
typedef struct {
 uint32 func_count,literal_size,code_size;
 bool is_indirect_mode;
 uint8 *literal;void *code;
} AOTModule;
static uint32_t instruction[8],data[8];
static unsigned mirrors;
static void *os_get_dbus_mirror(void *p) {
 uintptr_t offset=(uintptr_t)p-(uintptr_t)instruction;
 assert(offset<sizeof(instruction));mirrors++;
 return (uint8 *)data+offset;
}
static void set_error_buf(char *buf,uint32 len,const char *s) {
 if(buf && len) {strncpy(buf,s,len-1);buf[len-1]=0;}
}
#define CHECK_BUF(p,end,n) do {if((uint64_t)(n)>(uint64_t)((end)-(p))) goto fail;} while(0)
#define read_uint32(p,end,out) do {CHECK_BUF(p,end,4);memcpy(&(out),(p),4);(p)+=4;} while(0)
static uint32 get_plt_table_size(void) {return 0;}
static void init_plt_table(uint8 *p) {assert(p==(uint8 *)instruction+16);}
''' + function + r'''
int main(void) {
 char error[80]={0};AOTModule module={.func_count=1};
 uint8 *buf=(uint8 *)instruction,*end=buf+16;
 /* The instruction alias still contains a previous image / erased flash;
  * only the data alias contains the newly copied literal-size header. */
 memset(instruction,0xff,sizeof(instruction));memset(data,0,sizeof(data));
#if WASM_MEM_DUAL_BUS_MIRROR == 0
 instruction[0]=0;
#endif
 assert(load_text_section(buf,end,&module,error,sizeof(error)));
 assert(module.literal_size==0 && module.code==buf+4 && module.code_size==12);
 assert(module.literal==buf+4); /* execution keeps instruction-bus addresses */
 assert(mirrors==WASM_MEM_DUAL_BUS_MIRROR);
 data[0]=4;instruction[0]=4;
 assert(load_text_section(buf,end,&module,error,sizeof(error)));
 assert(module.code==buf+8 && module.code_size==8);
 data[0]=UINT32_MAX;instruction[0]=UINT32_MAX;
 assert(!load_text_section(buf,end,&module,error,sizeof(error)));
 data[0]=13;instruction[0]=13;
 assert(!load_text_section(buf,end,&module,error,sizeof(error)));
 assert(!load_text_section(buf,buf+3,&module,error,sizeof(error)));
 assert(!load_text_section(buf,buf,&module,error,sizeof(error)));
 return 0;
}
''')
