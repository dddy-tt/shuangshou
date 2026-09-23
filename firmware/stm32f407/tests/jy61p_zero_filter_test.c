/*
 * Host regression for transient all-zero JY61P angle register snapshots.
 *
 * The production driver is included directly with a tiny HAL I2C mock.  This
 * exercises JY61P_Read_Data() at the same seam used by main.c instead of
 * testing a duplicate filter implementation.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../Core/Inc/jy61p.h"

/* Expected diagnostic bit; the regression intentionally compiles against the
 * pre-fix header so the behavioural failure can be observed first. */
#ifndef JY61P_ERR_ANGLE_ZERO
#define JY61P_ERR_ANGLE_ZERO 0x10U
#endif

/* Keep the target-only I2C/HAL headers out of this host build. */
#define __I2C_H

typedef struct {
    void *Instance;
} I2C_HandleTypeDef;

typedef int HAL_StatusTypeDef;

#define HAL_OK                    0
#define HAL_ERROR                 1
#define I2C_MEMADD_SIZE_8BIT      1U

I2C_HandleTypeDef hi2c1;
I2C_HandleTypeDef hi2c2;
I2C_HandleTypeDef hi2c3;
volatile uint32_t sys_tick_ms;

static int16_t mock_angle_raw[3];

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c,
                                   uint16_t dev_addr,
                                   uint16_t mem_addr,
                                   uint16_t mem_addr_size,
                                   uint8_t *data,
                                   uint16_t size,
                                   uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c);
void HAL_Delay(uint32_t delay_ms);
void MX_I2C1_Init(void);
void MX_I2C2_Init(void);

#include "../Core/Src/jy61p.c"

static int failures;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            printf("FAIL: %s\n", message);                            \
            failures++;                                                \
        }                                                              \
    } while (0)

static void put_s16(uint8_t *out, int16_t value)
{
    out[0] = (uint8_t)((uint16_t)value & 0xFFU);
    out[1] = (uint8_t)(((uint16_t)value >> 8) & 0xFFU);
}

static void put_vector(uint8_t *out, const int16_t values[3])
{
    put_s16(&out[0], values[0]);
    put_s16(&out[2], values[1]);
    put_s16(&out[4], values[2]);
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c,
                                   uint16_t dev_addr,
                                   uint16_t mem_addr,
                                   uint16_t mem_addr_size,
                                   uint8_t *data,
                                   uint16_t size,
                                   uint32_t timeout)
{
    static const int16_t acc_raw[3] = {0, 0, 2048};
    static const int16_t gyro_raw[3] = {0, 0, 0};

    (void)hi2c;
    (void)dev_addr;
    (void)mem_addr_size;
    (void)timeout;
    if (data == 0 || size != 6U) return HAL_ERROR;

    if (mem_addr == JY61P_REG_AX_L) {
        put_vector(data, acc_raw);
    } else if (mem_addr == JY61P_REG_GX_L) {
        put_vector(data, gyro_raw);
    } else if (mem_addr == JY61P_REG_ROLL_L) {
        put_vector(data, mock_angle_raw);
    } else {
        return HAL_ERROR;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_DeInit(I2C_HandleTypeDef *hi2c)
{
    (void)hi2c;
    return HAL_OK;
}

void HAL_Delay(uint32_t delay_ms)
{
    sys_tick_ms += delay_ms;
}

void MX_I2C1_Init(void) {}
void MX_I2C2_Init(void) {}

static uint8_t read_once(void)
{
    float acc[3];
    float gyro[3];
    float angle[3];

    sys_tick_ms += 5U;
    return JY61P_Read_Data(JY61P_CH_RIGHT, acc, gyro, angle);
}

int main(void)
{
    float saved_angle[3];
    uint8_t err;

    memset(&JY61P_Right, 0, sizeof(JY61P_Right));
    JY61P_Right.online = 1U;

    mock_angle_raw[0] = 1000;
    mock_angle_raw[1] = -2000;
    mock_angle_raw[2] = 3000;
    err = read_once();
    CHECK(err == 0U, "normal angle snapshot must be accepted");
    CHECK(JY61P_Right.angle_valid != 0U, "normal angle snapshot must be valid");
    memcpy(saved_angle, JY61P_Right.angle, sizeof(saved_angle));

    mock_angle_raw[0] = 0;
    mock_angle_raw[1] = 0;
    mock_angle_raw[2] = 0;
    err = read_once();
    CHECK((err & JY61P_ERR_ANGLE_ZERO) != 0U,
          "one all-zero angle snapshot must be marked transient-invalid");
    CHECK(memcmp(saved_angle, JY61P_Right.angle, sizeof(saved_angle)) == 0,
          "one all-zero angle snapshot must keep the last good pose");

    err = read_once();
    CHECK((err & JY61P_ERR_ANGLE_ZERO) != 0U,
          "second consecutive all-zero snapshot must still be filtered");
    CHECK(memcmp(saved_angle, JY61P_Right.angle, sizeof(saved_angle)) == 0,
          "second consecutive all-zero snapshot must keep the last good pose");

    err = read_once();
    CHECK((err & JY61P_ERR_ANGLE_ZERO) != 0U,
          "repeated all-zero snapshots must remain invalid");
    CHECK(memcmp(saved_angle, JY61P_Right.angle, sizeof(saved_angle)) == 0,
          "repeated all-zero snapshots must never overwrite the last good pose");
    CHECK(JY61P_Right.online != 0U,
          "all-zero angle data still proves the I2C sensor is responding");

    mock_angle_raw[0] = 1200;
    mock_angle_raw[1] = -1800;
    mock_angle_raw[2] = 2800;
    err = read_once();
    CHECK(err == 0U, "non-zero pose after zero debounce must be accepted");
    CHECK(JY61P_Right.angle[0] != 0.0f &&
          JY61P_Right.angle[1] != 0.0f &&
          JY61P_Right.angle[2] != 0.0f,
          "normal pose must replace the preserved last good pose");

    if (failures != 0) return 1;
    printf("jy61p_zero_filter_test: all checks passed\n");
    return 0;
}
