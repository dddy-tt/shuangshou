#include "alarm_session.h"

#include "main.h"

static uint32_t alarm_session_mix(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7FEB352DU;
    value ^= value >> 15;
    value *= 0x846CA68BU;
    value ^= value >> 16;
    return value;
}

uint32_t AlarmSession_Generate(void)
{
    uint32_t value = 0U;
    uint32_t wait;

    /* STM32F407 自带 RNG；使用有界轮询，启动失败也不会卡住系统。 */
#if defined(RNG)
    __HAL_RCC_RNG_CLK_ENABLE();
    SET_BIT(RNG->CR, RNG_CR_RNGEN);
    CLEAR_BIT(RNG->SR, RNG_SR_CEIS | RNG_SR_SEIS);
    for (wait = 0U; wait < 256U; wait++) {
        uint32_t status = READ_REG(RNG->SR);
        if ((status & (RNG_SR_CECS | RNG_SR_SECS)) != 0U) {
            break;
        }
        if ((status & RNG_SR_DRDY) != 0U) {
            value = READ_REG(RNG->DR);
            break;
        }
    }
    CLEAR_BIT(RNG->CR, RNG_CR_RNGEN);
#else
    wait = 0U;
#endif

    /* 回退混合唯一 ID、启动时钟扰动和栈地址；不产生 Flash 写入。 */
    value ^= HAL_GetUIDw0();
    value ^= HAL_GetUIDw1();
    value ^= HAL_GetUIDw2();
    value ^= HAL_GetTick();
    value ^= SysTick->VAL;
    value ^= (uint32_t)(uintptr_t)&value;
    value ^= wait;
    value = alarm_session_mix(value);

    /* 协议中 0 保留为“无会话号”，因此强制避开 0。 */
    if (value == 0U) value = 0xA5C39E17UL;
    return value;
}
