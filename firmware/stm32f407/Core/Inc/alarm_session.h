#ifndef __ALARM_SESSION_H
#define __ALARM_SESSION_H

#include <stdint.h>

/* 生成本次启动的内存内会话号，不写 Flash。 */
uint32_t AlarmSession_Generate(void);

#endif /* __ALARM_SESSION_H */
