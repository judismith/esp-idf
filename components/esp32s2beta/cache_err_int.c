// Copyright 2015-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 The cache has an interrupt that can be raised as soon as an access to a cached
 region (flash, psram) is done without the cache being enabled. We use that here
 to panic the CPU, which from a debugging perspective is better than grabbing bad
 data from the bus.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_attr.h"
#include "soc/extmem_reg.h"
#include "soc/dport_reg.h"
#include "soc/periph_defs.h"
#include "sdkconfig.h"
#include "esp32s2beta/dport_access.h"

void esp_cache_err_int_init(void)
{
    uint32_t core_id = xPortGetCoreID();
    ESP_INTR_DISABLE(ETS_CACHEERR_INUM);

    // We do not register a handler for the interrupt because it is interrupt
    // level 4 which is not serviceable from C. Instead, xtensa_vectors.S has
    // a call to the panic handler for
    // this interrupt.
    intr_matrix_set(core_id, ETS_CACHE_IA_INTR_SOURCE, ETS_CACHEERR_INUM);

    // Enable invalid cache access interrupt when the cache is disabled.
    // When the interrupt happens, we can not determine the CPU where the
    // invalid cache access has occurred. We enable the interrupt to catch
    // invalid access on both CPUs, but the interrupt is connected to the
    // CPU which happens to call this function.
    // For this reason, panic handler backtrace will not be correct if the
    // interrupt is connected to PRO CPU and invalid access happens on the APP
    // CPU.

    DPORT_SET_PERI_REG_MASK(EXTMEM_CACHE_DBG_INT_CLR_REG,
                            EXTMEM_MMU_ENTRY_FAULT_INT_CLR |
                            EXTMEM_DCACHE_REJECT_INT_CLR |
                            EXTMEM_DCACHE_WRITE_FLASH_INT_CLR |
                            EXTMEM_DC_PRELOAD_SIZE_FAULT_INT_CLR |
                            EXTMEM_DC_SYNC_SIZE_FAULT_INT_CLR |
                            EXTMEM_ICACHE_REJECT_INT_CLR |
                            EXTMEM_IC_PRELOAD_SIZE_FAULT_INT_CLR |
                            EXTMEM_IC_SYNC_SIZE_FAULT_INT_CLR);
    DPORT_SET_PERI_REG_MASK(EXTMEM_CACHE_DBG_INT_ENA_REG,
                            EXTMEM_MMU_ENTRY_FAULT_INT_ENA |
                            EXTMEM_DCACHE_REJECT_INT_ENA |
                            EXTMEM_DCACHE_WRITE_FLASH_INT_ENA |
                            EXTMEM_DC_PRELOAD_SIZE_FAULT_INT_ENA |
                            EXTMEM_DC_SYNC_SIZE_FAULT_INT_ENA |
                            EXTMEM_ICACHE_REJECT_INT_ENA |
                            EXTMEM_IC_PRELOAD_SIZE_FAULT_INT_ENA |
                            EXTMEM_IC_SYNC_SIZE_FAULT_INT_ENA |
                            EXTMEM_CACHE_DBG_EN);

    ESP_INTR_ENABLE(ETS_CACHEERR_INUM);
}

int IRAM_ATTR esp_cache_err_get_cpuid(void)
{
    return PRO_CPU_NUM;
}
