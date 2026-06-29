/*
 * Active mapping note (v3.1):
 *   SoftI2C_1  PB6(SCL) / PB7(SDA)  -> Right-hand JY61P
 *   SoftI2C_2  PB8(SCL) / PB9(SDA)  -> Left-hand JY61P
 *   SoftI2C_3  PC6(SCL) / PC7(SDA)  -> MAX30102
 *   SI2C_MPU_RIGHT / SI2C_MPU_LEFT are legacy enum names only.
 *   They no longer represent current MPU6050 wiring.
 */
/**
  ******************************************************************************
  * @file           : soft_i2c.c
  * @brief          : Three software I2C buses with timeout fuse and bus recovery
  * @author         : Project maintenance v3.1
  * @date           : 2026-06-29
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "soft_i2c.h"
#include "gpio.h"

/* 閳光偓閳光偓 閺冭泛绨稉搴ょТ閺冭泛鐣€规矮绠?閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓 */

/**
 * @brief GPIO NOP 瀵よ埖妞傜粵澶岄獓
 * @note  delay(3) 缁撅妇鐡戞禍?10 娑?NOP 瀵邦亞骞?(168MHz 娑撳瀹?60ns 鑴?10 = 600ns 閸╁搫娼?
 *        閸旂姳绗?GPIO BSRR 鐎靛嫬鐡ㄩ崳銊ュ晸閸忋儱绱戦柨鈧? 鐎圭偤妾崡濠傛噯閺堢喎婀?3~5娓璼 閼煎啫娲?
 */
#define SI2C_DELAY_100K  3U    /* 100kHz 閺嶅洤鍣Ο鈥崇础 */
#define SI2C_DELAY_400K  0U    /* 400kHz 韫囶偊鈧喐膩瀵?(娴犲懎鐦庣€涙ê娅掗崘娆忓弳瀵偓闁库偓) */

/**
 * @brief 绾剝绉撮弮鍓佸晬閺傤厺绗傞梽?閳?閹烘帞鍤庡顔芥焽濮濆鏀ｉ梼鍙夊Б
 * @note  濮ｅ繗鐤嗗顏嗗箚缁?0.3娓璼 (1 NOP + SDA_READ 鐠?IDR 鐎靛嫬鐡ㄩ崳?,
 *        2000 濞?閳?600娓璼 鐡掑懏妞傜粣妤€褰? 鏉╂粌銇囨禍搴㈩劀鐢?I2C 鎼存梻鐡熷鎯扮箿 (~10娓璼),
 *        閸欏牐鍐绘径鐔虹叚娴犮儵浼╅崗宥囬兇缂佺喕鐨熸惔锕€娅掗幇鐔虹叀閸掓澘宕辨い瑁も偓? *        閼汇儴袝閸欐垶顒濈搾鍛, 99% 閺勵垱甯撶痪?閻掑﹦鍋ｉ悧鈺冩倞閺傤叀顥囬幋鏍︾矤鐠佹儳顦缁樻簚閵? */
#define SI2C_TO_CYCLES   2000UL

/**
 * @brief 閹崵鍤庨幁銏狀槻: 鏉╃偟鐢婚崣鎴︹偓浣烘畱閺冨爼鎸撻懘澶婂暱閺? * @note  閺嶈宓?I2C 鐟欏嫯瀵?v6.0 鎼?.1.16, 瑜?SDA 鐞氼偂绮犵拋鎯ь槵閹板繐顦婚幏澶夌秵閺?
 *        娑撴槒顔曟径鍥х安閸?SCL 娑撳﹤褰傞柅浣规付婢?9 娑擃亪顤傛径鏍ㄦ闁界喎鎳嗛張鐔朵簰闁插﹥鏂侀幀鑽ゅ殠閵? */
#define SI2C_RECOVERY_CLKS  9U

/* 閳光偓閳光偓 GPIO 娴ｅ秴褰块弻銉﹀鐞?(閺囧じ鍞?GCC __builtin_ctz, 閸忕厧顔?Keil ARMCC) 閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓 */
static uint8_t pin_to_bit(uint16_t pin)
{
    if      (pin == GPIO_PIN_0)  return 0;
    else if (pin == GPIO_PIN_1)  return 1;
    else if (pin == GPIO_PIN_2)  return 2;
    else if (pin == GPIO_PIN_3)  return 3;
    else if (pin == GPIO_PIN_4)  return 4;
    else if (pin == GPIO_PIN_5)  return 5;
    else if (pin == GPIO_PIN_6)  return 6;
    else if (pin == GPIO_PIN_7)  return 7;
    else if (pin == GPIO_PIN_8)  return 8;
    else if (pin == GPIO_PIN_9)  return 9;
    else if (pin == GPIO_PIN_10) return 10;
    else if (pin == GPIO_PIN_11) return 11;
    else if (pin == GPIO_PIN_12) return 12;
    else if (pin == GPIO_PIN_13) return 13;
    else if (pin == GPIO_PIN_14) return 14;
    else if (pin == GPIO_PIN_15) return 15;
    return 0;
}

/* 閳光偓閳光偓 瀵洝鍓奸幒褍鍩楃€?(BSRR 閸樼喎鐡欓幙宥勭稊, 娑撳秴褰茬悮顐¤厬閺傤厽澧﹂弬? 閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓 */

/**
 * @brief SCL 閹恒劍灏虫潏鎾冲毉: 妤傛鏁搁獮? * @note  BSRR 娴?16 娴ｅ秴鍟?1 缂冾喕缍?ODR, 閺勵垰宕熼崨銊︽埂閸樼喎鐡欓幙宥勭稊,
 *        娑撳秳绱版禍褏鏁撶拠?閺€?閸愭瑧鐝垫禍澶堚偓? */
#define SCL_H(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SCL_PORT->BSRR = SI2C1_SCL_PIN; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SCL_PORT->BSRR = SI2C2_SCL_PIN; \
    else                               SI2C3_SCL_PORT->BSRR = SI2C3_SCL_PIN; \
} while(0)

/**
 * @brief SCL 閹恒劍灏虫潏鎾冲毉: 娴ｅ海鏁搁獮? * @note  BSRR 妤?16 娴ｅ秴鍟?1 婢跺秳缍?ODR (缁涘鐜禍?BRR 鐎靛嫬鐡ㄩ崳?,
 *        閸氬本鐗遍弰顖氬礋閸涖劍婀￠崢鐔风摍閹垮秳缍旈妴? */
#define SCL_L(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SCL_PORT->BSRR = (uint32_t)SI2C1_SCL_PIN << 16U; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SCL_PORT->BSRR = (uint32_t)SI2C2_SCL_PIN << 16U; \
    else                               SI2C3_SCL_PORT->BSRR = (uint32_t)SI2C3_SCL_PIN << 16U; \
} while(0)

#define SDA_H(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SDA_PORT->BSRR = SI2C1_SDA_PIN; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SDA_PORT->BSRR = SI2C2_SDA_PIN; \
    else                               SI2C3_SDA_PORT->BSRR = SI2C3_SDA_PIN; \
} while(0)

#define SDA_L(dev)  do { \
    if      ((dev) == SI2C_MPU_RIGHT) SI2C1_SDA_PORT->BSRR = (uint32_t)SI2C1_SDA_PIN << 16U; \
    else if ((dev) == SI2C_MPU_LEFT)  SI2C2_SDA_PORT->BSRR = (uint32_t)SI2C2_SDA_PIN << 16U; \
    else                               SI2C3_SDA_PORT->BSRR = (uint32_t)SI2C3_SDA_PIN << 16U; \
} while(0)

/* 閳光偓閳光偓 瀵洝鍓奸悩鑸碘偓浣筋嚢閸欐牕鐣?閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓 */

/**
 * @brief 鐠?SDA 瀵洝鍓艰ぐ鎾冲閻㈤潧閽?(鏉堟挸鍙嗛悩鑸碘偓?
 * @note  娴ｈ法鏁?GPIOx->IDR 鐎靛嫬鐡ㄩ崳? 鐠囪澧犳稉宥夋付閸掑洦宕?MODER 娑撻缚绶崗?
 *        閸ョ姳璐熼幒銊﹀俺鏉堟挸鍤Ο鈥崇础娑?IDR 娴犲秶鍔ч崣宥嗘Ё瀵洝鍓肩€圭偤妾悽闈涢挬閵? *        娴ｅ棗婀鈧蹇涘櫞閺€鎯ф倵闂団偓鐟?SDA_SetIn() 绾喕绻氭稉宥夆攳閸斻劍鈧崵鍤庨妴? */
#define SDA_READ(dev) ( \
    (dev) == SI2C_MPU_RIGHT ? ((SI2C1_SDA_PORT->IDR & SI2C1_SDA_PIN) ? 1U : 0U) : \
    (dev) == SI2C_MPU_LEFT  ? ((SI2C2_SDA_PORT->IDR & SI2C2_SDA_PIN) ? 1U : 0U) : \
                               ((SI2C3_SDA_PORT->IDR & SI2C3_SDA_PIN) ? 1U : 0U) )

/**
 * @brief 鐠?SCL 瀵洝鍓艰ぐ鎾冲閻㈤潧閽?
 * @note  閻劋绨幀鑽ゅ殠閹垹顦查弮鍓佹畱閺冨爼鎸撻幏澶夊嚑鐡掑懏妞傚Λ鈧ù?
 *        娴犮儱寮烽崥顖氬З閸撳秶鈥樼拋?SCL 瀹歌尪顫﹂柌濠冩杹閵? */
#define SCL_READ(dev) ( \
    (dev) == SI2C_MPU_RIGHT ? ((SI2C1_SCL_PORT->IDR & SI2C1_SCL_PIN) ? 1U : 0U) : \
    (dev) == SI2C_MPU_LEFT  ? ((SI2C2_SCL_PORT->IDR & SI2C2_SCL_PIN) ? 1U : 0U) : \
                               ((SI2C3_SCL_PORT->IDR & SI2C3_SCL_PIN) ? 1U : 0U) )

/* 閳光偓閳光偓 GPIO 閺傜懓鎮滈崚鍥ㄥ床 (瀵偓濠曞繑膩閹风喓娈戦崗鎶芥暛) 閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓 */

/**
 * @brief 鐏?SDA 瀵洝鍓肩拋鍙ヨ礋閹恒劍灏虫潏鎾冲毉濡€崇础
 * @note  MODER[2y+1:2y] = 01 閳?闁氨鏁ゆ潏鎾冲毉濡€崇础
 *        閸掑洦宕查崜宥団€樻穱?ODR 瀹告彃鍟撻崗銉ф窗閺嶅洨鏁搁獮?(閻?SDA_H/SDA_L 鐎瑰繋绻氱拠?,
 *        闁灝鍘ら惉顒勬？鏉堟挸鍤柨娆掝嚖閻㈤潧閽╅妴? */
static void SDA_SetOut(SI2C_Dev_t dev)
{
    GPIO_TypeDef *port;
    uint16_t pin;
    if (dev == SI2C_MPU_RIGHT)      { port = SI2C1_SDA_PORT; pin = SI2C1_SDA_PIN; }
    else if (dev == SI2C_MPU_LEFT)  { port = SI2C2_SDA_PORT; pin = SI2C2_SDA_PIN; }
    else                             { port = SI2C3_SDA_PORT; pin = SI2C3_SDA_PIN; }
    uint8_t bit = pin_to_bit(pin);
    /* 濞撳懘娅?MODER 鐎电懓绨叉担?閳?閸?01 (闁氨鏁ゆ潏鎾冲毉) */
    port->MODER &= ~(3UL << (2UL * bit));
    port->MODER |=  (1UL << (2UL * bit));
}

/**
 * @brief 鐏?SDA 瀵洝鍓肩拋鍙ヨ礋鏉堟挸鍙嗗Ο鈥崇础 (濞搭喚鈹? 娓氭繆绂嗘径鏍劥娑撳﹥濯?
 * @note  MODER[2y+1:2y] = 00 閳?鏉堟挸鍙嗗Ο鈥崇础
 *        閻劋绨柌濠冩杹 SDA 閹崵鍤? 缁涘绶熸禒搴ゎ啎婢跺洤绨茬粵?(ACK=0 閹峰缍?.
 */
static void SDA_SetIn(SI2C_Dev_t dev)
{
    GPIO_TypeDef *port;
    uint16_t pin;
    if (dev == SI2C_MPU_RIGHT)      { port = SI2C1_SDA_PORT; pin = SI2C1_SDA_PIN; }
    else if (dev == SI2C_MPU_LEFT)  { port = SI2C2_SDA_PORT; pin = SI2C2_SDA_PIN; }
    else                             { port = SI2C3_SDA_PORT; pin = SI2C3_SDA_PIN; }
    uint8_t bit = pin_to_bit(pin);
    port->MODER &= ~(3UL << (2UL * bit));
}

/* 閳光偓閳光偓 瀵よ埖妞?閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓閳光偓 */

/**
 * @brief 鏉烆垯娆㈠鑸垫 (缁鐭戞惔?NOP 瀵邦亞骞?
 * @param n 瀵よ埖妞傜粵澶岄獓: 0閳?.9娓璼, 3閳?.6娓璼 (168MHz 鐎圭偞绁?
 * @note  娴ｈ法鏁?volatile 娣囶噣銈板顏嗗箚閸欐﹢鍣? 闂冪粯顒涚紓鏍槯閸ｃ劋绱崠鏍ㄧХ闂勩們鈧? *        168MHz 娑撳鐦℃稉?NOP 缁?6ns, 閸旂姳绗傚顏嗗箚閸掑棙鏁鈧柨鈧痪?30ns/鏉烆喓鈧? */
static void si2c_delay(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++) {
        __NOP();
    }
}

/* 閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡? *  閺嶇绺剧搾鍛閻旀梹鏌囬崙鑺ユ殶 閳?鐟欙絽鍠?閹烘帞鍤庡顔芥焽 閳?MCU 濮橀晲绠欏濠氭敚"闂傤噣顣?
 * 閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡?*/

/**
 * @brief  缁涘绶?SDA 閸欐ü璐熸妯兼暩楠?(娴犲氦顔曟径鍥櫞閺€鐐偓鑽ゅ殠), 鐢妇鈥栫搾鍛閻旀梹鏌?
 * @param  dev  鐠佹儳顦紓鏍у娇
 * @retval 0    閹存劕濮?(SDA 閸︺劏绉撮弮璺哄閸欐﹢鐝?
 * @retval 1    鐡掑懏妞?(閹烘帞鍤庨弬顓☆棁閹存牔绮犵拋鎯ь槵瀵倸鐖堕幏澶嬵劥 SDA)
 * @note   鐠嬪啰鏁ら崷鐑樻珯: 缁涘绶熸禒搴ゎ啎婢跺洭鍣撮弨?ACK閵嗕胶鐡戝鍛偓鑽ゅ殠缁屾椽妫?
 *         濮ｅ繗鐤嗗顏嗗箚: SDA_READ (鐠?IDR 鐎靛嫬鐡ㄩ崳?~2 閸涖劍婀? + 閸掑棙鏁?(~3 閸涖劍婀?
 *         2000 鏉?閳?600娓璼 @ 168MHz, 鐟曞棛娲婇張鈧幈銏㈡畱 I2C 娴犲氦顔曟径鍥ф惙鎼存柣鈧? */
static uint8_t si2c_wait_sda_high(SI2C_Dev_t dev)
{
    uint32_t to = SI2C_TO_CYCLES;
    while (!SDA_READ(dev) && --to) {
        __NOP();
    }
    return (to == 0U) ? 1U : 0U;  /* 0=閹存劕濮? 1=鐡掑懏妞傞悢鏃€鏌?*/
}

/**
 * @brief  缁涘绶?SCL 閸欐ü璐熸妯兼暩楠?(閺冨爼鎸撻幏澶夊嚑鐡掑懏妞傚Λ鈧ù?, 鐢妇鈥栫搾鍛閻旀梹鏌?
 * @param  dev  鐠佹儳顦紓鏍у娇
 * @retval 0    閹存劕濮?
 * @retval 1    鐡掑懏妞?(SCL 鐞氼偂绮犵拋鎯ь槵闂€鎸庢埂閹峰缍?閳?閺冨爼鎸撻幏澶夊嚑鐡掑懘妾?
 * @note   MPU6050 閸?MAX30102 閸у洣绗夐弨顖涘瘮閺冨爼鎸撻幏澶夊嚑, 濮濄倕鍤遍弫棰佸瘜鐟曚胶鏁ゆ禍? *         閹崵鍤庨幁銏狀槻閺冭埖顥呭ù瀣€栨禒鑸垫櫊闂呮嚎鈧? */
static uint8_t si2c_wait_scl_high(SI2C_Dev_t dev)
{
    uint32_t to = SI2C_TO_CYCLES;
    while (!SCL_READ(dev) && --to) {
        __NOP();
    }
    return (to == 0U) ? 1U : 0U;
}

/**
 * @brief  閸欐垿鈧?SCL 娑撳﹦娈戞稉鈧稉顏呮闁界喕鍓﹂崘?(閻劋绨幀鑽ゅ殠閹垹顦?
 * @param  dev          鐠佹儳顦紓鏍у娇
 * @param  speed_delay  瀵よ埖妞傜粵澶岄獓
 * @note   閸?SDA 鐞氼偅鍓版径鏍ㄥ娴ｅ孩妞? 娑撴槒顔曟径鍥ф躬 SCL 娑撳﹦鐐曟潪?9 濞嗏€冲讲鏉╊偂濞?
 *         娴犲氦顔曟径鍥╁Ц閹焦婧€婢跺秳缍呴獮鍫曞櫞閺€?SDA閵? */
static void si2c_send_clock_pulse(SI2C_Dev_t dev, uint8_t speed_delay)
{
    SCL_L(dev);
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    /* 閳?妤犲矁鐦?SCL 绾喖鐤勯崣姗€鐝?閳?閼汇儲甯撶痪鎸庢焽閸?SCL 閺冪姵纭剁悮顐＄瑐閹?*/
    if (si2c_wait_scl_high(dev)) {
        /* SCL 鐡掑懏妞傞張顏勫綁妤?閳?閹烘帞鍤庨悧鈺冩倞閺傤叀顥? 閺冪娀娓剁紒褏鐢婚崣鎴︹偓浣藉墻閸?*/
        return;
    }
}

/* 閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡? *  閹崵鍤庨幁銏狀槻鎼村繐鍨?閳?I2C 鐟欏嫯瀵?v6.0 鎼?.1.16
 * 閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡?*/

/**
 * @brief  I2C 閹崵鍤庣憴锝夋敚鎼村繐鍨?
 * @param  dev  鐠佹儳顦紓鏍у娇
 * @note   瑜版挻顥呭ù瀣煂 SDA 鐞氼偅鍓版径鏍ㄥ瘮缂侇厽濯烘担?(BUS_BUSY) 閺?
 *         1. 閸欐垿鈧?9 娑?SCL 閺冨爼鎸撻懘澶婂暱, 妞瑰崬濮╂禒搴ゎ啎婢跺洨濮搁幀浣规簚闁插﹥鏂?SDA
 *         2. 閼汇儰绮涢張顏堝櫞閺€?閳?鐡掑懏妞傞悢鏃€鏌? 鏉╂柨娲?SI2C_TIMEOUT
 *         3. 閸欐垿鈧?STOP 閺夆€叉娴ｆ寧鈧崵鍤庨崶鐐插煂 IDLE
 *         閺堫剙鍤遍弫鏉挎躬娑撹鎯婇悳?50ms 缁狙傛崲閸斺€茶厬鐠嬪啰鏁? 娑撳秹妯嗘繅鐐茬杽閺冭埖鈧嗩洣濮瑰倿鐝惃?5ms 娴犺濮熼妴? */
static SI2C_Status_t si2c_bus_recovery(SI2C_Dev_t dev)
{
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    /* 1. 绾喕绻?SCL 閸掓繂顫愭稉娲彯 (閼汇儱鍑＄悮顐ｅ娴ｅ骸鍨崗鍫ュ櫞閺€? */
    SCL_H(dev);
    si2c_delay(speed);

    /* 2. 鏉╃偟鐢婚崣鎴︹偓浣规付婢?9 娑擃亝妞傞柦鐔诲墻閸?*/
    for (uint8_t i = 0; i < SI2C_RECOVERY_CLKS; i++) {
        si2c_send_clock_pulse(dev, speed);
        /* 濮ｅ繋閲滈懘澶婂暱閸氬孩顥呴弻?SDA 閺勵垰鎯佸鏌ュ櫞閺€?*/
        if (SDA_READ(dev)) {
            break;  /* SDA 瀹告煡鍣撮弨? 閹绘劕澧犻柅鈧崙?*/
        }
    }

    /* 3. 閼?9 娑擃亝妞傞柦鐔锋倵 SDA 娴犲秶鍔ф担?閳?閻椻晝鎮婇弬顓犲殠绾喛顓? 閺€鎯х磾閹垹顦?*/
    if (!SDA_READ(dev)) {
        return SI2C_TIMEOUT;
    }

    /* 4. 閸欐垿鈧?STOP 閺夆€叉: SDA 娴犲簼缍嗛崚浼寸彯閻ㄥ嫯鐑﹂崣?(閸?SCL 妤傛鏁搁獮铏埂闂? */
    SDA_SetOut(dev);
    SDA_L(dev);
    si2c_delay(speed);
    SCL_H(dev);
    si2c_delay(speed);
    SDA_H(dev);
    si2c_delay(speed);

    return SI2C_OK;
}

/* 閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡? *  I2C 閸╃儤婀伴弮璺虹碍閸樼喕顕?
 * 閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡?*/

/**
 * @brief  閸氼垰濮╅弶鈥叉: SDA 閳?閸?SCL=H 閺堢喖妫?
 * @param  dev          鐠佹儳顦紓鏍у娇
 * @param  speed_delay  闁喎瀹崇€电懓绨查惃鍕閺冨墎鐡戠痪? * @note   I2C 鐟欏嫯瀵? 鐠у嘲顫愰弶鈥叉 = 閸?SCL 娑撴椽鐝悽闈涢挬閺? SDA 娴犲酣鐝崚棰佺秵閻ㄥ嫯鐑﹂崣妯糕偓? *         鐠嬪啰鏁ら懓鍛箑妞よ崵鈥樻穱婵団偓鑽ゅ殠缁屾椽妫?(SDA=H, SCL=H) 閸氬骸鍟€鐠嬪啰鏁ら妴? */
static void si2c_start(SI2C_Dev_t dev, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    SDA_H(dev);
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SDA_L(dev);          /* 閳?鏉╂瑩鍣锋禍褏鏁撶挧宄邦潗閺夆€叉 */
    si2c_delay(speed_delay);
    SCL_L(dev);
    si2c_delay(speed_delay);
}

/**
 * @brief  閸嬫粍顒涢弶鈥叉: SDA 閳?閸?SCL=H 閺堢喖妫?
 * @note   閸嬫粍顒涢弶鈥叉 = 閸?SCL 娑撴椽鐝悽闈涢挬閺? SDA 娴犲簼缍嗛崚浼寸彯閻ㄥ嫯鐑﹂崣妯糕偓? */
static void si2c_stop(SI2C_Dev_t dev, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    SDA_L(dev);
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SDA_H(dev);          /* 閳?鏉╂瑩鍣锋禍褏鏁撻崑婊勵剾閺夆€叉 */
    si2c_delay(speed_delay);
}

/**
 * @brief  閸愭瑤绔存稉顏勭摟閼哄倸鍩岄幀鑽ゅ殠, 鐢?ACK 鐡掑懏妞傞悢鏃€鏌?
 * @param  dev          鐠佹儳顦紓鏍у娇
 * @param  byte         瀵板懎褰傞柅浣哥摟閼? * @param  speed_delay  闁喎瀹冲鑸垫缁涘楠?
 * @retval 0            娴犲氦顔曟径?ACK (SDA 鐞氼偅濯烘担?
 * @retval 1            娴犲氦顔曟径?NACK 閹存牞绉撮弮?(閹烘帞鍤庨弬顓☆棁)
 * @note   v2.3 閺€纭呯箻: 缁?9 娑?SCL 閸涖劍婀￠崘鍛瑝閸愬秶娲哥拠璁崇濞?
 *         閼板本妲稿顏嗗箚缁涘绶?SDA 閸欐ü缍?(ACK) 楠炶埖鏌︽禒銉ㄧТ閺冨墎鍟嶉弬顓溾偓? *         閼汇儰绮犵拋鎯ь槵濮濓絽鐖舵惔鏃傜摕, SDA 娴兼艾婀?SCL 娑撳﹤宕屽▽鍨倵 ~1娓璼 閸愬懓顫﹂幏澶夌秵閵? */
static uint8_t si2c_write_byte(SI2C_Dev_t dev, uint8_t byte, uint8_t speed_delay)
{
    SDA_SetOut(dev);
    for (uint8_t i = 0; i < 8; i++) {
        if (byte & 0x80U) {
            SDA_H(dev);
        } else {
            SDA_L(dev);
        }
        si2c_delay(speed_delay);
        SCL_H(dev);
        si2c_delay(speed_delay);
        SCL_L(dev);
        byte <<= 1;
    }

    /* 閳光偓閳光偓 缁?9 娑擃亝妞傞柦? 闁插﹥鏂?SDA 閳?缁涘绶熸禒搴ゎ啎婢跺洦濯烘担?(ACK) 閳光偓閳光偓 */
    SDA_SetIn(dev);            /* SDA 閸掑洣璐熸潏鎾冲弳, 闁插﹥鏂侀幀鑽ゅ殠 */
    si2c_delay(speed_delay);
    SCL_H(dev);                /* 娴溠呮晸缁?9 娑?SCL 娑撳﹤宕屽▽?*/

    /* 閳?閸忔娊鏁梼鎻掑敖: 鐢箒绉撮弮鍓佸晬閺傤厾娈?ACK 缁涘绶?閳?*/
    uint8_t nack = si2c_wait_sda_high(dev);
    /* 婵″倹鐏夌搾鍛 (SDA 婵绮撴稉杞扮秵 = 娴犲氦顔曟径?ACK), nack=0 閳?濮濓絽鐖?
     * 婵″倹鐏?SDA 閸欐﹢鐝?(NACK 閹存牗鏌囩痪?, nack=1 閳?瀵倸鐖?*/

    si2c_delay(speed_delay);
    SCL_L(dev);
    SDA_SetOut(dev);
    return nack;  /* 0=ACK (濮濓絽鐖?, 1=NACK/鐡掑懏妞?*/
}

/**
 * @brief  娴犲孩鈧崵鍤庣拠璁崇娑擃亜鐡ч懞? 鐢?SDA 闁插﹥鏂佺搾鍛閻旀梹鏌?
 * @param  dev          鐠佹儳顦紓鏍у娇
 * @param  send_ack     1=閸欐垿鈧?ACK (闁氨鐓℃禒搴ゎ啎婢跺洨鎴风紒顓炲絺闁?, 0=閸欐垿鈧?NACK (閺堚偓閸氬骸鐡ч懞?
 * @param  speed_delay  闁喎瀹冲鑸垫缁涘楠?
 * @return 鐠囪褰囬崚鎵畱 8 娴ｅ秵鏆熼幑? * @note   v2.3 閺€纭呯箻: 濮ｅ繋閲?SCL 妤傛鏁搁獮铏埂闂? 缁涘绶?SDA 缁嬪啿鐣鹃崥搴″晙鐠囪褰?
 *         闂冨弶顒涢崶鐘冲笓缁惧灝鐦庨悽鐔烘暩鐎圭懓顕遍懛鏉戞躬 SDA 鐠哄啿褰夋稉顓⑩偓鏃囶嚖鐠囨眹鈧? */
static uint8_t si2c_read_byte(SI2C_Dev_t dev, uint8_t send_ack, uint8_t speed_delay)
{
    uint8_t byte = 0;
    SDA_SetIn(dev);            /* 闁插﹥鏂?SDA, 鐠佲晙绮犵拋鎯ь槵妞瑰崬濮╅弫鐗堝祦缁?*/
    for (uint8_t i = 0; i < 8; i++) {
        SDA_H(dev);            /* 閸?ODR=1 娴ｅ棔绗夋す鍗炲З (瀹告彃鍨忔潏鎾冲弳), 绾喕绻氬ù顔锯敄 */
        si2c_delay(speed_delay);
        SCL_H(dev);            /* SCL 娑撳﹤宕屽▽? 娴犲氦顔曟径鍥偓浣稿毉閺佺増宓?*/
        /* 缁涘绶?SDA 缁嬪啿鐣?(鎼存柨顕幒鎺斿殠鐎靛嫮鏁撻悽闈涱啇鐎佃壈鍤ч惃鍕瑐閸楀洦閮ㄥ鎯扮箿) */
        si2c_delay(speed_delay);
        byte <<= 1;
        if (SDA_READ(dev)) {
            byte |= 1U;
        }
        si2c_delay(speed_delay);
        SCL_L(dev);
    }

    /* 閳光偓閳光偓 缁?9 娑擃亝妞傞柦? 閸欐垿鈧?ACK/NACK 閳光偓閳光偓 */
    SDA_SetOut(dev);
    if (send_ack) {
        SDA_L(dev);  /* ACK  = 閹峰缍?閳?闁氨鐓℃禒搴ゎ啎婢跺洨鎴风紒顓炲絺闁椒绗呮稉鈧€涙濡?*/
    } else {
        SDA_H(dev);  /* NACK = 閹峰鐝?閳?闁氨鐓℃禒搴ゎ啎婢跺洩绻栭弰顖涙付閸氬簼绔存稉顏勭摟閼?*/
    }
    si2c_delay(speed_delay);
    SCL_H(dev);
    si2c_delay(speed_delay);
    SCL_L(dev);
    SDA_H(dev);
    return byte;
}

/* 閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡? *  閸忣剙鍙?API
 * 閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡鎰ㄦ櫜閳烘劏鏅查埡?*/

/**
 * @brief  鏉?I2C 閸掓繂顫愰崠?閳?閹崵鍤庣粚娲＝绾喛顓?+ 娑撳鐭?STOP 鎼村繐鍨?
 * @note   閸?MX_GPIO_Init() 娑斿鎮楃拫鍐暏閵? *         鐎佃鐦＄捄顖涒偓鑽ゅ殠閸欐垿鈧?STOP 閺夆€叉, 绾喕绻氭禒搴ゎ啎婢跺洨濮搁幀浣规簚婢跺秳缍呴崚?IDLE閵? *         v2.3: 婢х偛濮為崥顖氬З閸撳秵鈧崵鍤庤箛娆愵梾濞? 閼?SDA 鐞氼偅濯烘担搴″灟閹笛嗩攽閹崵鍤庨幁銏狀槻閵? */
void SoftI2C_Init(void)
{
    for (uint8_t d = 0; d < 3; d++) {
        SI2C_Dev_t dev = (SI2C_Dev_t)d;
        /* 濡偓濞?SDA 閺勵垰鎯佺悮顐ｅ壈婢舵牗濯烘担?(娴犲氦顔曟径鍥ㄦ弓闁插﹥鏂? */
        SDA_SetIn(dev);
        if (!SDA_READ(dev)) {
            /* SDA 鐞氼偅濯哄?閳?閹笛嗩攽閹崵鍤庨幁銏狀槻 */
            si2c_bus_recovery(dev);
        }
        /* 閺冪姾顔戦弰顖氭儊閹垹顦? 鐞涖儰绔存稉?STOP 绾喕绻氶幀鑽ゅ殠 IDLE */
        si2c_stop(dev, SI2C_DELAY_100K);
    }
}

/**
 * @brief  鐠囪褰囬崡鏇氶嚋鐎靛嫬鐡ㄩ崳銊ョ摟閼? * @param  dev       鐠佹儳顦紓鏍у娇
 * @param  dev_addr  閸ｃ劋娆㈤崷鏉挎絻 (8-bit, bit0=0 鐞涖劎銇氶崘?
 * @param  reg_addr  鐎靛嫬鐡ㄩ崳銊ユ勾閸р偓
 * @param  data      [閸戝搫寮琞 鐠囪褰囬崚鎵畱閺佺増宓佺€涙濡?
 * @retval SI2C_OK       閹存劕濮?
 * @retval SI2C_TIMEOUT  閹崵鍤庣搾鍛 (閹烘帞鍤庨弬顓☆棁閹存牔绮犵拋鎯ь槵閺冪姴鎼锋惔?
 * @retval SI2C_NACK     娴犲氦顔曟径?NACK (閸ｃ劋娆㈤崷鏉挎絻闁挎瑨顕ら幋鏍ф珤娴犺埖婀亸杈╁崕)
 *
 * @閺冭泛绨?(閺嶅洤鍣?I2C 缂佸嫬鎮庣拠璁崇皑閸?:
 *   START 閳?閸愭瑥娅掓禒璺烘勾閸р偓(W) 閳?閸愭瑥鐦庣€涙ê娅掗崷鏉挎絻 閳?Repeated START 閳?鐠囪娅掓禒璺烘勾閸р偓(R) 閳?鐠?鐎涙濡?NACK) 閳?STOP
 */
SI2C_Status_t SoftI2C_ReadByte(SI2C_Dev_t dev, uint8_t dev_addr,
                               uint8_t reg_addr, uint8_t *data)
{
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    /* 閳光偓閳光偓 闂冭埖顔?1: 閸愭瑥娅掓禒璺烘勾閸р偓 (W) + 鐎靛嫬鐡ㄩ崳銊ユ勾閸р偓 閳光偓閳光偓 */
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFEU, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;  /* 閸ｃ劋娆㈤弮鐘茬安缁? 閸欘垵鍏橀崷鏉挎絻闁挎瑨顕ら幋鏍ф珤娴犺埖婀稉濠勬暩 */
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;  /* 鐎靛嫬鐡ㄩ崳銊ユ勾閸р偓 NACK */
    }

    /* 閳光偓閳光偓 闂冭埖顔?2: Repeated START + 鐠囪娅掓禒璺烘勾閸р偓 (R) + 鐠?1 鐎涙濡?+ NACK 閳光偓閳光偓 */
    si2c_start(dev, speed);  /* Repeated Start: 娑撳秴鍘涢崣?STOP */
    if (si2c_write_byte(dev, dev_addr | 0x01U, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    *data = si2c_read_byte(dev, 0U, speed);  /* send_ack=0 閳?NACK 缂佸牊顒?*/
    si2c_stop(dev, speed);
    return SI2C_OK;
}

/**
 * @brief  閹靛綊鍣虹拠璇插絿鏉╃偟鐢荤€靛嫬鐡ㄩ崳? * @param  dev       鐠佹儳顦紓鏍у娇
 * @param  dev_addr  閸ｃ劋娆㈤崷鏉挎絻 (8-bit)
 * @param  reg_addr  鐠у嘲顫愮€靛嫬鐡ㄩ崳銊ユ勾閸р偓 (婢堆囧劥閸?I2C 閸ｃ劋娆㈤弨顖涘瘮閼奉亜濮╅柅鎺戭杻)
 * @param  buf       [閸戝搫寮琞 閺佺増宓佺紓鎾冲暱閸? * @param  len       鐠囪褰囩€涙濡弫? * @retval SI2C_OK / SI2C_NACK / SI2C_TIMEOUT
 *
 * @note   MPU6050 閻ㄥ嫬濮為柅鐔峰/闂勨偓閾昏桨鍗庣€靛嫬鐡ㄩ崳銊ユ勾閸р偓鏉╃偟鐢婚柅鎺戭杻,
 *         娑撯偓濞喡ゎ嚢閸?14 鐎涙濡В鏃堚偓鎰嚋鐎靛嫬鐡ㄩ崳銊嚢閸欐牕鎻?~14 閸婂秲鈧? */
SI2C_Status_t SoftI2C_ReadBuf(SI2C_Dev_t dev, uint8_t dev_addr,
                              uint8_t reg_addr, uint8_t *buf, uint8_t len)
{
    if (len == 0U) return SI2C_OK;
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    /* 閸愭瑩妯佸▓?*/
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFEU, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }

    /* Repeated Start 閳?鐠囧妯佸▓?*/
    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr | 0x01U, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }

    for (uint8_t i = 0; i < len; i++) {
        /* 閺堚偓閸氬簼绔存稉顏勭摟閼哄倸褰?NACK (0), 閸撳秹娼伴崣?ACK (1) */
        buf[i] = si2c_read_byte(dev, (i < (len - 1U)) ? 1U : 0U, speed);
    }
    si2c_stop(dev, speed);
    return SI2C_OK;
}

/**
 * @brief  閸愭瑥宕熸稉顏勭槑鐎涙ê娅掔€涙濡?
 * @param  dev       鐠佹儳顦紓鏍у娇
 * @param  dev_addr  閸ｃ劋娆㈤崷鏉挎絻 (8-bit)
 * @param  reg_addr  鐎靛嫬鐡ㄩ崳銊ユ勾閸р偓
 * @param  data      瀵板懎鍟撻崗銉ф畱閸楁洖鐡ч懞鍌涙殶閹? * @retval SI2C_OK / SI2C_NACK / SI2C_TIMEOUT
 */
SI2C_Status_t SoftI2C_WriteByte(SI2C_Dev_t dev, uint8_t dev_addr,
                                uint8_t reg_addr, uint8_t data)
{
    uint8_t speed = (dev == SI2C_MAX30102) ? SI2C_DELAY_400K : SI2C_DELAY_100K;

    si2c_start(dev, speed);
    if (si2c_write_byte(dev, dev_addr & 0xFEU, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, reg_addr, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    if (si2c_write_byte(dev, data, speed)) {
        si2c_stop(dev, speed);
        return SI2C_NACK;
    }
    si2c_stop(dev, speed);
    return SI2C_OK;
}
