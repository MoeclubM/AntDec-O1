#ifndef BOARD_HW_H
#define BOARD_HW_H

#include "stm32f1xx_hal.h"
#include "adc.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"
#include <stdbool.h>
#include <stdint.h>

/* Power sequencing & control outputs */
#define DAC_CHIP_EN_PORT        GPIOC
#define DAC_CHIP_EN_PIN         GPIO_PIN_0   /* U501.8  -> ES9039Q2M CHIP_EN */
#define DAC_AVDD_EN_PORT        GPIOC
#define DAC_AVDD_EN_PIN         GPIO_PIN_1   /* U501.9  -> U4 TPS7A2033 (DAC_AVDD_3V3 + DVDD 1.2V) */
#define DAC_VCCA_EN_PORT        GPIOC
#define DAC_VCCA_EN_PIN         GPIO_PIN_2   /* U501.10 -> U3 TPS7A2033 (DAC_VCCA_3V3) */
#define DAC_AVCC_EN_PORT        GPIOC
#define DAC_AVCC_EN_PIN         GPIO_PIN_3   /* U501.11 -> U5 ES9312Q (AVCC_DAC1/2) */
#define AMP_EN_REQ_PORT         GPIOC
#define AMP_EN_REQ_PIN          GPIO_PIN_4   /* U501.24 -> U14/U15 OPA1622 EN */
#define MCU_RUN_OK_PORT         GPIOC
#define MCU_RUN_OK_PIN          GPIO_PIN_5   /* U501.25 -> Baseboard MCU_RUN_OK */
#define AUDIO_PATH_EN_PORT      GPIOB
#define AUDIO_PATH_EN_PIN       GPIO_PIN_0   /* U501.26 -> Baseboard AUDIO_PATH_ENABLE */

/* Power & system sense inputs */
#define PWR_OK_PORT             GPIOC
#define PWR_OK_PIN              GPIO_PIN_15  /* U501.4  <- LM27762 PGOOD (Active LOW: 0 = +-4.5V OK) */
#define XU_PWR_PRESENT_N_PORT   GPIOC
#define XU_PWR_PRESENT_N_PIN    GPIO_PIN_9   /* U501.40 <- XU_POWER_PRESENT_N */
#define STM_VBUS_SENSE_PORT     GPIOA
#define STM_VBUS_SENSE_PIN      GPIO_PIN_9   /* U501.42 <- STM_USB_VBUS_SENSE */

/* ST7789 SPI1 display control pins (PA5=SCK, PA7=MOSI) */
#define TFT_DC_PORT             GPIOA
#define TFT_DC_PIN              GPIO_PIN_2   /* U501.16 -> J_TFT.6 */
#define TFT_RST_PORT            GPIOA
#define TFT_RST_PIN             GPIO_PIN_3   /* U501.17 -> J_TFT.5 */
#define TFT_CS_PORT             GPIOA
#define TFT_CS_PIN              GPIO_PIN_4   /* U501.20 -> J_TFT.7 */

/* User tactile keys (Active LOW, 10k pull-up + 10nF) */
#define KEY_PREV_PORT           GPIOC
#define KEY_PREV_PIN            GPIO_PIN_13  /* U501.2  <- KEY3 */
#define KEY_PLAY_PORT           GPIOB
#define KEY_PLAY_PIN            GPIO_PIN_1   /* U501.27 <- KEY4 */
#define KEY_NEXT_PORT           GPIOC
#define KEY_NEXT_PIN            GPIO_PIN_14  /* U501.3  <- KEY5 */

/* XMOS inter-board link pins */
#define XMOS_RX_PORT            GPIOC
#define XMOS_RX_PIN             GPIO_PIN_6   /* U501.37 <- X0D37 (STM_XU_MUTE_STATUS) */
#define XMOS_TX_PORT            GPIOC
#define XMOS_TX_PIN             GPIO_PIN_7   /* U501.38 <-> X0D38 (STM_XU_DSD_ON_STATUS, 4.7k pull-up on XMOS) */

/* DAC I2C1 pins */
#define DAC_I2C_PORT            GPIOB
#define DAC_I2C_SCL_PIN         GPIO_PIN_6   /* U501.58 -> DAC_I2C_SCL */
#define DAC_I2C_SDA_PIN         GPIO_PIN_7   /* U501.59 -> DAC_I2C_SDA */

void Board_I2C1_BusRecover(void);
void Board_ReadAnalogRails(uint16_t *vdda_mv, uint16_t *pg1_mv, uint16_t *pg2_mv);
uint16_t Board_Micros16(void);
void Board_DelayUs(uint16_t us);

#endif /* BOARD_HW_H */
