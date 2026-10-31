
#include "pf_loader.h"

extern "C" void app_main(void) {
    pf_init();
    while(1){
        pf_loop();
    }
}
