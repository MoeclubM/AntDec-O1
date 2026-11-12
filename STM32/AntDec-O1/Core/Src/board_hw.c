#include "board_hw.h"
#include "xmos_link.h"

static uint16_t adc_read_single(uint32_t channel, uint32_t sample_time)
{
  ADC_ChannelConfTypeDef cfg = {0};
  cfg.Channel = channel;
  cfg.Rank = ADC_REGULAR_RANK_1;
  cfg.SamplingTime = sample_time;
  if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) {
    return 0U;
  }
  uint16_t val = 0U;
  for (uint8_t pass = 0U; pass < 2U; pass++) {
    if (HAL_ADC_Start(&hadc1) != HAL_OK) {
      return 0U;
    }
    if (HAL_ADC_PollForConversion(&hadc1, 5U) == HAL_OK) {
      val = (uint16_t)HAL_ADC_GetValue(&hadc1);
    }
  }
  return val;
}

void Board_ReadAnalogRails(uint16_t *vdda_mv, uint16_t *pg1_mv, uint16_t *pg2_mv)
{
  if ((ADC1->CR2 & ADC_CR2_TSVREFE) == 0U) {
    SET_BIT(ADC1->CR2, ADC_CR2_TSVREFE);
    Board_DelayUs(25U);
  }
  uint16_t vref_raw = adc_read_single(ADC_CHANNEL_VREFINT, ADC_SAMPLETIME_239CYCLES_5);
  uint16_t ch0_raw  = adc_read_single(ADC_CHANNEL_0, ADC_SAMPLETIME_71CYCLES_5);
  uint16_t ch1_raw  = adc_read_single(ADC_CHANNEL_1, ADC_SAMPLETIME_71CYCLES_5);

  uint32_t vdda = 3300UL;
  if (vref_raw >= 1000U && vref_raw <= 2200U) {
    vdda = (1200UL * 4095UL) / (uint32_t)vref_raw;
  }

  if (vdda_mv != NULL) {
    *vdda_mv = (uint16_t)vdda;
  }
  if (pg1_mv != NULL) {
    *pg1_mv = (uint16_t)((ch0_raw * vdda) / 4095UL);
  }
  if (pg2_mv != NULL) {
    *pg2_mv = (uint16_t)((ch1_raw * vdda) / 4095UL);
  }
}

uint16_t Board_Micros16(void)
{
  return (uint16_t)(TIM2->CNT);
}

void Board_DelayUs(uint16_t us)
{
  uint16_t start = (uint16_t)(TIM2->CNT);
  while ((uint16_t)((uint16_t)(TIM2->CNT) - start) < us) {
  }
}

void Board_I2C1_BusRecover(void)
{
  (void)HAL_I2C_DeInit(&hi2c1);

  GPIO_InitTypeDef gpio = {0};
  HAL_GPIO_WritePin(DAC_I2C_PORT, DAC_I2C_SCL_PIN | DAC_I2C_SDA_PIN, GPIO_PIN_SET);
  gpio.Pin = DAC_I2C_SCL_PIN | DAC_I2C_SDA_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_OD;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DAC_I2C_PORT, &gpio);
  Board_DelayUs(10U);

  for (uint8_t i = 0U; i < 9U; i++) {
    if (HAL_GPIO_ReadPin(DAC_I2C_PORT, DAC_I2C_SDA_PIN) == GPIO_PIN_SET) {
      break;
    }
    HAL_GPIO_WritePin(DAC_I2C_PORT, DAC_I2C_SCL_PIN, GPIO_PIN_RESET);
    Board_DelayUs(6U);
    HAL_GPIO_WritePin(DAC_I2C_PORT, DAC_I2C_SCL_PIN, GPIO_PIN_SET);
    Board_DelayUs(6U);
  }

  HAL_GPIO_WritePin(DAC_I2C_PORT, DAC_I2C_SDA_PIN, GPIO_PIN_RESET);
  Board_DelayUs(6U);
  HAL_GPIO_WritePin(DAC_I2C_PORT, DAC_I2C_SCL_PIN, GPIO_PIN_SET);
  Board_DelayUs(6U);
  HAL_GPIO_WritePin(DAC_I2C_PORT, DAC_I2C_SDA_PIN, GPIO_PIN_SET);
  Board_DelayUs(6U);

  MX_I2C1_Init();
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == XMOS_RX_PIN) {
    XmosLink_OnRxEdge();
  }
}
