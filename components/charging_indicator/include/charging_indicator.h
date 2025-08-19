#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void charging_indicator_init(void);
// dt_ms: 호출 간격(ms). 권장 1000ms.
void charging_indicator_update(uint32_t dt_ms);

#ifdef __cplusplus
}
#endif
