/*
 * DHT.c
 *
 * Driver for DHT11 / DHT22 temperature & humidity sensors.
 * Target: STM32F4xx via HAL + DWT cycle-counter for µs delays.
 *
 * Fixes applied vs. original:
 *   1. DHT22 raw values now divided by 10.0f → correct decimal readings
 *   2. DHT22 negative temperature handled via sign bit (bit 15)
 *   3. Checksum comparison masked to 8 bits (& 0xFF)
 *   4. DHT_Read() variable 'i' initialized to 0
 *   5. Presence checked before reading data bytes
 *   6. TYPE_DHT11 / TYPE_DHT22 comment corrected
 *   7. DWT_Delay_Init() called once at startup inside DHT_GetData guard
 */

/* ── User config ────────────────────────────────────────────────────────── */

#include "stm32f4xx_hal.h"

//#define TYPE_DHT11      /* uncomment for DHT11 */
#define TYPE_DHT22        /* uncomment for DHT22 */

#define DHT_PORT  GPIOA
#define DHT_PIN   GPIO_PIN_8

/* ── No changes needed below this line ──────────────────────────────────── */

#include "DHT.h"

/* --------------------------------------------------------------------------
 * DWT microsecond delay
 * -------------------------------------------------------------------------- */

static uint8_t dwt_initialized = 0;

static uint32_t DWT_Delay_Init(void)
{
    /* Disable then re-enable TRC */
    CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;
    CoreDebug->DEMCR |=  CoreDebug_DEMCR_TRCENA_Msk;

    /* Disable then re-enable cycle counter */
    DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk;
    DWT->CTRL |=  DWT_CTRL_CYCCNTENA_Msk;

    /* Reset counter */
    DWT->CYCCNT = 0;

    __ASM volatile ("NOP");
    __ASM volatile ("NOP");
    __ASM volatile ("NOP");

    return (DWT->CYCCNT) ? 0 : 1; /* 0 = success, 1 = not started */
}

static inline void delay_us(volatile uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    us *= (HAL_RCC_GetHCLKFreq() / 1000000UL);
    while ((DWT->CYCCNT - start) < us);
}

/* --------------------------------------------------------------------------
 * GPIO helpers
 * -------------------------------------------------------------------------- */

static void Set_Pin_Output(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = GPIO_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOx, &GPIO_InitStruct);
}

static void Set_Pin_Input(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin  = GPIO_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;   /* pull-up keeps line stable */
    HAL_GPIO_Init(GPIOx, &GPIO_InitStruct);
}

/* --------------------------------------------------------------------------
 * DHT protocol — start pulse
 * -------------------------------------------------------------------------- */

static void DHT_Start(void)
{
    Set_Pin_Output(DHT_PORT, DHT_PIN);
    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_RESET); /* pull low */

#if defined(TYPE_DHT11)
    delay_us(18000);   /* DHT11: hold low ≥ 18 ms */
#elif defined(TYPE_DHT22)
    delay_us(1200);    /* DHT22: hold low ≥ 1 ms  */
#endif

    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_SET);   /* release high */
    delay_us(30);                                          /* host release  */
    Set_Pin_Input(DHT_PORT, DHT_PIN);
}

/* --------------------------------------------------------------------------
 * DHT protocol — check sensor response pulse
 * Returns: 1 = sensor present and responding
 *          0 = no response (sensor absent or wiring fault)
 * -------------------------------------------------------------------------- */

static uint8_t DHT_Check_Response(void)
{
    uint8_t response = 0;

    delay_us(40);

    if (!HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN))  /* line pulled low by sensor */
    {
        delay_us(80);
        if (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN)) /* line pulled high by sensor */
            response = 1;
    }

    /* Wait for sensor to release the line before data transmission */
    uint32_t timeout = 10000;
    while (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) && --timeout);

    return response;
}

/* --------------------------------------------------------------------------
 * DHT protocol — read one byte (MSB first)
 * -------------------------------------------------------------------------- */

static uint8_t DHT_Read(void)
{
    uint8_t data = 0;   /* FIX: initialized to 0 (was uninitialized) */

    for (int j = 7; j >= 0; j--)
    {
        /* Wait for pin to go high (start of bit) */
        uint32_t timeout = 10000;
        while (!HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) && --timeout);

        delay_us(40); /* sample after 40 µs — mid-bit */

        if (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN))
            data |= (1 << j);  /* high after 40 µs → bit is 1 */
        /* else: bit is 0, already set by initialization */

        /* Wait for pin to go low (end of bit) */
        timeout = 10000;
        while (HAL_GPIO_ReadPin(DHT_PORT, DHT_PIN) && --timeout);
    }

    return data;
}

/* --------------------------------------------------------------------------
 * Public API — read temperature and humidity into DHT_DataTypedef
 * -------------------------------------------------------------------------- */

void DHT_GetData(DHT_DataTypedef *DHT_Data)
{
    /* Initialize DWT once */
    if (!dwt_initialized)
    {
        DWT_Delay_Init();
        dwt_initialized = 1;
    }

    DHT_Start();

    /* FIX: bail out if sensor does not respond */
    if (!DHT_Check_Response())
        return;

    uint8_t Rh_byte1   = DHT_Read();
    uint8_t Rh_byte2   = DHT_Read();
    uint8_t Temp_byte1 = DHT_Read();
    uint8_t Temp_byte2 = DHT_Read();
    uint8_t checksum   = DHT_Read();

    /* FIX: mask sum to 8 bits before comparing — overflow is expected */
    uint8_t sum = (uint8_t)((Rh_byte1 + Rh_byte2 + Temp_byte1 + Temp_byte2) & 0xFF);

    if (checksum != sum)
        return; /* checksum mismatch — discard frame, keep last good values */

#if defined(TYPE_DHT11)
    /* DHT11: integer-only, no scaling needed */
    DHT_Data->Temperature = (float)Temp_byte1;
    DHT_Data->Humidity    = (float)Rh_byte1;

#elif defined(TYPE_DHT22)
    /* DHT22: 16-bit word, LSB = 0.1 unit, bit 15 = sign */
    uint16_t raw_hum  = ((uint16_t)Rh_byte1   << 8) | Rh_byte2;
    uint16_t raw_temp = ((uint16_t)Temp_byte1 << 8) | Temp_byte2;

    DHT_Data->Humidity = (float)raw_hum / 10.0f; /* FIX: divide by 10 */

    /* FIX: handle negative temperature (bit 15 = sign flag) */
    if (raw_temp & 0x8000)
        DHT_Data->Temperature = -((float)(raw_temp & 0x7FFF) / 10.0f);
    else
        DHT_Data->Temperature =  ((float)raw_temp / 10.0f);
#endif
}
