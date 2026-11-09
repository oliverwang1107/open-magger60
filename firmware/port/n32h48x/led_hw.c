/*
 * led_hw.c - LED output hardware, see led_hw.h and LED_NOTES.md. No waiting loops, no interrupts.
 */
#include "led_hw.h"

#include "led_regs.h"

static struct {
    const uint8_t *last_frame;                      /* host tests only look at it */
    bool     started;
    bool     killed;
    bool     busy;
    uint32_t t_start;
    uint32_t sent, done, timeouts;
} s_hw;

#if KM_LED_HW_LEVEL != LED_HW_DRY

/* PB8: alternate function 0, push-pull, pull-up, drive strength code 1, slew code 0 (LED_NOTES.md section 1). */
static void data_pin_configure(void)
{
    const unsigned pin = LED_DATA_PIN;
    const unsigned afi = pin / 4u, afsh = 8u * (pin % 4u);
    LED_GPIO_AF(LED_GPIOB, afi) = (LED_GPIO_AF(LED_GPIOB, afi) & ~(0x1Fu << afsh));       /* AF 0 */
    LED_GPIO_POTYPE(LED_GPIOB) &= ~(1u << pin);
    LED_GPIO_PMODE(LED_GPIOB) = (LED_GPIO_PMODE(LED_GPIOB) & ~(3u << (2u * pin))) | (LED_PMODE_AF << (2u * pin));
    LED_GPIO_PUPD(LED_GPIOB) = (LED_GPIO_PUPD(LED_GPIOB) & ~(3u << (2u * pin))) | (1u << (2u * pin));
    LED_GPIO_SR(LED_GPIOB) &= ~(1u << pin);
    LED_GPIO_DS(LED_GPIOB) = (LED_GPIO_DS(LED_GPIOB) & ~(3u << (2u * pin))) | (1u << (2u * pin));
}

static void data_pin_release(void)
{
    const unsigned pin = LED_DATA_PIN;
    LED_GPIO_PMODE(LED_GPIOB) &= ~(3u << (2u * pin));            /* back to the reset state: input */
    LED_GPIO_PUPD(LED_GPIOB) &= ~(3u << (2u * pin));
}

#if KM_LED_HW_LEVEL == LED_HW_FULL
/* PF5: output, OPEN DRAIN, no pull, driven low (low for as long as the lights are in use, LED_NOTES.md section 5). Level first, then mode. */
static void power_pin_on(void)
{
    const unsigned pin = LED_POWER_PIN;
    LED_GPIO_PBC(LED_GPIOF) = 1u << pin;
    LED_GPIO_POTYPE(LED_GPIOF) |= 1u << pin;
    LED_GPIO_PMODE(LED_GPIOF) = (LED_GPIO_PMODE(LED_GPIOF) & ~(3u << (2u * pin))) | (LED_PMODE_OUTPUT << (2u * pin));
    LED_GPIO_PUPD(LED_GPIOF) &= ~(3u << (2u * pin));
    LED_GPIO_SR(LED_GPIOF) &= ~(1u << pin);
    LED_GPIO_DS(LED_GPIOF) = (LED_GPIO_DS(LED_GPIOF) & ~(3u << (2u * pin))) | (1u << (2u * pin));
}

static void power_pin_off(void)
{
    LED_GPIO_PBSC(LED_GPIOF) = 1u << LED_POWER_PIN;              /* open drain released */
}
#endif

static void dma_stop(void)
{
    LED_DMA5_CHCFG &= ~LED_DMA5_CHCFG_EN;
}

/* The sequence: disable, clear, configure, enable (the channel number 5 is fixed; CHSEL selects SPI5 TX as the request). */
static void dma_arm(const uint8_t *frame, unsigned len)
{
    LED_DMA5_CHCFG &= ~LED_DMA5_CHCFG_EN;
    LED_DMA5_CHCFG = 0;
    LED_DMA5_TXNUM = 0;
    LED_DMA5_PADDR = 0;
    LED_DMA5_MADDR = 0;
    LED_DMA_INTCLR = LED_DMA5_INTCLR_ALL;
    LED_DMA5_CHCFG = (LED_DMA5_CHCFG & LED_DMA5_CHCFG_KEEP) | LED_DMA5_CHCFG_SET;
    LED_DMA5_TXNUM = len;
    LED_DMA5_PADDR = LED_SPI5_DAT_ADDR;
    LED_DMA5_MADDR = (uint32_t)(uintptr_t)frame;
    LED_DMA_BURSTEN = LED_DMA_BURSTEN_VAL;
    LED_DMA5_CHSEL = LED_DMA5_CHSEL_SPI5TX;
    LED_DMA5_CHCFG |= LED_DMA5_CHCFG_EN;
}

#endif /* KM_LED_HW_LEVEL != LED_HW_DRY */

bool led_hw_start(const uint8_t *dark, unsigned len, uint32_t now_cycles)
{
    if (s_hw.started || s_hw.killed) {
        return s_hw.started;
    }
    s_hw.started = true;
#if KM_LED_HW_LEVEL != LED_HW_DRY
    LED_RCC_APB2PCLKEN |= LED_APB2_SPI5 | LED_APB2_AFIO;
    LED_RCC_AHB1PCLKEN |= LED_AHB1_GPIOB | LED_AHB1_GPIOF;
    LED_RCC_AHBPCLKEN |= LED_AHB_DMA1;
#if KM_LED_HW_LEVEL == LED_HW_FULL
    power_pin_on();
#endif
    data_pin_configure();
    LED_SPI5_CTRL1 = (LED_SPI5_CTRL1 & LED_SPI5_CTRL1_KEEP) | LED_SPI5_CTRL1_SET;
    LED_SPI5_I2SCFG &= ~LED_SPI5_I2SCFG_CLR;
    LED_SPI5_CRCPOLY = (LED_SPI5_CRCPOLY & 0xFFFF0000u) | LED_SPI5_CRCPOLY_VAL;
    LED_SPI5_CTRL2 |= LED_SPI5_CTRL2_TXDMA;
    dma_arm(dark, len);
    LED_SPI5_CTRL2 |= LED_SPI5_CTRL2_EN;
#else
    (void)dark;
    (void)len;
#endif
    s_hw.last_frame = dark;
    s_hw.busy = true;
    s_hw.t_start = now_cycles;
    s_hw.sent++;
    return true;
}

void led_hw_kill(void)
{
    if (s_hw.killed) {
        return;
    }
    s_hw.killed = true;
    s_hw.busy = false;
    if (!s_hw.started) {
        return;                                     /* nothing was ever touched */
    }
    s_hw.started = false;
#if KM_LED_HW_LEVEL != LED_HW_DRY
    dma_stop();
    LED_SPI5_CTRL2 &= ~(LED_SPI5_CTRL2_EN | LED_SPI5_CTRL2_TXDMA);
    LED_DMA_INTCLR = LED_DMA5_INTCLR_ALL;
    LED_DMA_BURSTEN = 0;                            /* the value the ADC driver (and its restart) uses */
    data_pin_release();
#if KM_LED_HW_LEVEL == LED_HW_FULL
    power_pin_off();
#endif
#endif
}

bool led_hw_send(const uint8_t *frame, unsigned len, uint32_t now_cycles)
{
    if (!s_hw.started || s_hw.busy) {
        return false;
    }
#if KM_LED_HW_LEVEL != LED_HW_DRY
    dma_arm(frame, len);
#else
    (void)frame;
    (void)len;
#endif
    s_hw.last_frame = frame;
    s_hw.busy = true;
    s_hw.t_start = now_cycles;
    s_hw.sent++;
    return true;
}

led_hw_state_t led_hw_poll(uint32_t now_cycles)
{
    if (!s_hw.busy) {
        return LED_HW_IDLE;
    }
#if KM_LED_HW_LEVEL != LED_HW_DRY
    if ((LED_DMA5_CHCFG & LED_DMA5_CHCFG_EN) == 0u || LED_DMA5_TXNUM == 0u) {
        s_hw.busy = false;
        s_hw.done++;
        return LED_HW_IDLE;
    }
    if ((uint32_t)(now_cycles - s_hw.t_start) >= LED_HW_TIMEOUT_CYCLES) {
        dma_stop();
        LED_DMA_INTCLR = LED_DMA5_INTCLR_ALL;
        s_hw.busy = false;
        s_hw.timeouts++;
        return LED_HW_TIMEOUT;
    }
    return LED_HW_BUSY;
#else
    if ((uint32_t)(now_cycles - s_hw.t_start) >= 2u * 240000u) {     /* a pretend frame takes 2 ms */
        s_hw.busy = false;
        s_hw.done++;
        return LED_HW_IDLE;
    }
    return LED_HW_BUSY;
#endif
}

void led_hw_get_diag(led_hw_diag_t *d)
{
    d->sent = s_hw.sent;
    d->done = s_hw.done;
    d->timeouts = s_hw.timeouts;
    d->started = s_hw.started;
    d->killed = s_hw.killed;
    d->level = KM_LED_HW_LEVEL;
    d->spi_ctrl1 = d->spi_ctrl2 = d->dma_cfg = d->dma_txnum = d->rcc_cfg = d->rcc_cfg2 = d->rcc_apb2 = 0u;
#if KM_LED_HW_LEVEL != LED_HW_DRY
    if (s_hw.started) {                             /* a LED that is off (guard, kill) does not touch the hardware, not even to read */
        d->spi_ctrl1 = LED_SPI5_CTRL1;
        d->spi_ctrl2 = LED_SPI5_CTRL2;
        d->dma_cfg = LED_DMA5_CHCFG;
        d->dma_txnum = LED_DMA5_TXNUM;
        d->rcc_cfg = LED_RCC_CFG;
        d->rcc_cfg2 = LED_RCC_CFG2;
        d->rcc_apb2 = LED_RCC_APB2PCLKEN;
    }
#endif
}

#ifdef KM_LED_HOST_TEST
#include <string.h>

void led_hw_test_reset(void)
{
    memset(&s_hw, 0, sizeof s_hw);
}

const uint8_t *led_hw_test_last_frame(void)
{
    return s_hw.last_frame;
}
#endif
