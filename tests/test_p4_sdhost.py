"""Exercise the actual dual-slot owner adapter against a modeled IDF controller."""
import unittest
from test_waveshare import ROOT, run_c


class P4SDHost(unittest.TestCase):
    def test_storage_and_c5_release_only_their_own_slot(self):
        source=(ROOT/'systems/espidf/components/aster_connectivity/sdhost.c').read_text()
        source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_ARG -3
#define portMAX_DELAY 0xffffffffu
static int depth,mutex,initializations,global_deinitializations,fail_init;
static bool controller,registered[2];
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {return &mutex;}
static void xSemaphoreTake(SemaphoreHandle_t m,unsigned t) {(void)t;assert(m && !depth);depth=1;}
static void xSemaphoreGive(SemaphoreHandle_t m) {assert(m && depth);depth=0;}
static int sdmmc_host_init(void) {
 assert(!controller);initializations++;
 if(fail_init){fail_init=0;return ESP_ERR_NO_MEM;}
 controller=true;return 0;
}
static int sdmmc_host_deinit_slot(int slot) {
 assert(controller && slot>=0 && slot<2);
 if(!registered[slot])return ESP_ERR_INVALID_STATE;
 registered[slot]=false;if(!registered[0] && !registered[1])controller=false;return 0;
}
static int sdmmc_host_deinit(void) {assert(controller);global_deinitializations++;controller=false;return 0;}
''' + source + r'''
int main(void) {
 assert(!aster_storage_sdmmc_init());registered[0]=true;
 assert(!aster_hosted_sdmmc_init());registered[1]=true;assert(initializations==1);
 assert(!aster_hosted_sdmmc_deinit() && controller && registered[0]);
 assert(aster_storage_sdmmc_deinit(1)==ESP_ERR_INVALID_ARG && controller);
 assert(!aster_storage_sdmmc_deinit(0) && !controller);
 assert(aster_hosted_sdmmc_deinit()==ESP_ERR_INVALID_STATE);
 fail_init=1;assert(aster_hosted_sdmmc_init()==ESP_ERR_NO_MEM && !controller);
 assert(!aster_storage_sdmmc_init()); /* emulate failed SD slot initialization */
 assert(!aster_storage_sdmmc_deinit(0) && !controller && global_deinitializations==1);
 assert(!aster_storage_sdmmc_init()); /* SD slot fails while Hosted acquires slot 1 */
 assert(!aster_hosted_sdmmc_init());registered[1]=true;
 assert(aster_storage_sdmmc_deinit(0)==ESP_ERR_INVALID_STATE && controller);
 assert(!aster_hosted_sdmmc_deinit() && !controller && global_deinitializations==1);
 assert(!aster_hosted_sdmmc_init());registered[1]=true;
 assert(!aster_storage_sdmmc_init());registered[0]=true;
 assert(!aster_storage_sdmmc_deinit(0) && controller && registered[1]);
 assert(!aster_hosted_sdmmc_deinit() && !controller && !depth);
 return 0;
}
''')
