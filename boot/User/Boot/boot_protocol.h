#ifndef __BOOT_PROTOCOL_H__
#define __BOOT_PROTOCOL_H__

#include <stdbool.h>

void boot_protocol_init(void);
void boot_protocol_task(void);
/* 判断UART IAP是否占用APP2或处于待跳转状态。 */
bool boot_protocol_is_busy(void);

#endif
