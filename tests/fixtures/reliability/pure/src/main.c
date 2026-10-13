#include <aster/aster.h>
static volatile uint32_t x;
int aster_app_init(void){return 0;}
void aster_app_event(const aster_event_t *e){(void)e;for(;;)x++;}
