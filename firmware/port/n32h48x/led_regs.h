/*
 * led_regs.h - the registers the LED output touches (SPI5, DMA1 channel 5, PB8, PF5, the clock enables). M5 task L5.
 * Names come from the chip vendor's SVD; the values are the ones that drive the LED chain on this keyboard
 * (hardware facts, see LED_NOTES.md section 2). Bit MEANINGS are mostly unknown (the SVD has no enum values), so the constants are
 * named after what they are used for.
 *
 * Under -DKM_LED_HOST_TEST the registers are a simulation provided by tests/test_led.c.
 */
#ifndef LED_REGS_H
#define LED_REGS_H

#include <stdint.h>

#ifdef KM_LED_HOST_TEST
uint32_t *km_led_sim_reg32(uintptr_t addr);
#define LED_REG(addr)  (*km_led_sim_reg32((uintptr_t)(addr)))
#else
#include "n32h48x_regs.h"
#define LED_REG(addr)  KM_REG32(addr)
#endif

/* ---- clocks (RCC 0x40021000), same registers and bit positions sense_regs.h uses ---- */
#define LED_RCC_AHBPCLKEN    LED_REG(0x40021018u)    /* DMA1 = bit 7 */
#define LED_RCC_APB2PCLKEN   LED_REG(0x40021020u)    /* AFIO = bit 0; bit 2 is the SPI5 clock enable (name unconfirmed) */
#define LED_RCC_AHB1PCLKEN   LED_REG(0x4002103Cu)    /* GPIOA..F = bits 0..5 */
#define LED_RCC_CFG          LED_REG(0x40021004u)    /* read only, for the diagnostics */
#define LED_RCC_CFG2         LED_REG(0x40021008u)    /* read only, for the diagnostics */
#define LED_AHB_DMA1         0x80u
#define LED_APB2_AFIO        0x1u
#define LED_APB2_SPI5        0x4u
#define LED_AHB1_GPIOB       0x2u
#define LED_AHB1_GPIOF       0x20u

/* ---- GPIO (same layout as sense_regs.h) ---- */
#define LED_GPIOB            0x4002C400u
#define LED_GPIOF            0x4002D400u
#define LED_GPIO_PMODE(p)    LED_REG((p) + 0x00u)
#define LED_GPIO_PUPD(p)     LED_REG((p) + 0x04u)
#define LED_GPIO_POTYPE(p)   LED_REG((p) + 0x08u)
#define LED_GPIO_AF(p, i)    LED_REG((p) + 0x0Cu + 4u * (i))   /* 5 bits per pin, 4 pins per register */
#define LED_GPIO_DS(p)       LED_REG((p) + 0x1Cu)
#define LED_GPIO_SR(p)       LED_REG((p) + 0x20u)
#define LED_GPIO_PBSC(p)     LED_REG((p) + 0x24u)              /* write 1 = set the pin */
#define LED_GPIO_PBC(p)      LED_REG((p) + 0x28u)              /* write 1 = clear the pin */

#define LED_PMODE_INPUT      0u
#define LED_PMODE_OUTPUT     1u
#define LED_PMODE_AF         2u
#define LED_DATA_PIN         8u                                /* PB8 = SPI5 MOSI */
#define LED_POWER_PIN        5u                                /* PF5 */

/* ---- SPI5 (0x40013000) ---- */
#define LED_SPI5_BASE        0x40013000u
#define LED_SPI5_CTRL1       LED_REG(LED_SPI5_BASE + 0x00u)
#define LED_SPI5_CTRL2       LED_REG(LED_SPI5_BASE + 0x04u)
#define LED_SPI5_STS         LED_REG(LED_SPI5_BASE + 0x08u)
#define LED_SPI5_DAT_ADDR    (LED_SPI5_BASE + 0x0Cu)
#define LED_SPI5_CRCPOLY     LED_REG(LED_SPI5_BASE + 0x18u)
#define LED_SPI5_I2SCFG      LED_REG(LED_SPI5_BASE + 0x1Cu)
#define LED_SPI5_CTRL1_KEEP  0xFFFFF9F7u /* CTRL1 = (CTRL1 & 0xF9F7) | 0xD844 as a halfword access; the upper half is left alone */
#define LED_SPI5_CTRL1_SET   0xD844u
#define LED_SPI5_CTRL2_TXDMA 0x4u
#define LED_SPI5_CTRL2_EN    0x1u
#define LED_SPI5_I2SCFG_CLR  0x2u         /* this bit of +0x1C is cleared */
#define LED_SPI5_CRCPOLY_VAL 7u           /* halfword write */

/* ---- DMA1 channel 5 (same layout as sense_regs.h: 0x14 bytes per channel) ---- */
#define LED_DMA_BASE         0x40020000u
#define LED_DMA5             (LED_DMA_BASE + 0x14u * 4u)
#define LED_DMA5_CHCFG       LED_REG(LED_DMA5 + 0x00u)
#define LED_DMA5_MADDR       LED_REG(LED_DMA5 + 0x04u)
#define LED_DMA5_PADDR       LED_REG(LED_DMA5 + 0x08u)
#define LED_DMA5_CHSEL       LED_REG(LED_DMA5 + 0x0Cu)
#define LED_DMA5_TXNUM       LED_REG(LED_DMA5 + 0x10u)
#define LED_DMA_INTCLR       LED_REG(LED_DMA_BASE + 0xA0u)
#define LED_DMA_BURSTEN      LED_REG(LED_DMA_BASE + 0xA8u)
#define LED_DMA5_INTCLR_ALL  0xF0000u
#define LED_DMA5_CHCFG_KEEP  0xFFF8000Fu
#define LED_DMA5_CHCFG_SET   0x3090u      /* memory -> peripheral (0x10), memory increment (0x80), priority 3 (0x3000); 8 bit, not circular */
#define LED_DMA5_CHCFG_EN    0x1u
#define LED_DMA5_CHSEL_SPI5TX 0x20u
#define LED_DMA_BURSTEN_VAL  1u           /* the LED driver writes 1 (the ADC driver writes 0), LED_NOTES.md section 2 */

#endif /* LED_REGS_H */
