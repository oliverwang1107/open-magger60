/*
 * sense_regs.h - registers and constants of the hall-sensor scan (ADC1-4, DMA1 ch1/ch3, GTIM1, GPIO mux, RCC bits).
 * Addresses and register names are those of the chip vendor's SVD (not in this repository). The bit MEANINGS are
 * mostly unconfirmed (the SVD has no enum values): SENSING_BRINGUP_NOTES.md says, for each group of values,
 * whether it was proven on the keyboard (S1-S3) or is only a guess (evidence levels V / M / I / U). The constants
 * are therefore named after what they are used for, not after a field name. Where the values were first learned is
 * stated in DISCLAIMER.md section 5.
 *
 * Under -DKM_SENSE_HOST_TEST the registers are a simulation provided by tests/test_sense.c.
 */
#ifndef SENSE_REGS_H
#define SENSE_REGS_H

#include <stdint.h>

#ifdef KM_SENSE_HOST_TEST
uint32_t *km_sense_sim_reg32(uintptr_t addr);
void km_sense_sim_feed(void);
#define SENSE_REG(addr)    (*km_sense_sim_reg32((uintptr_t)(addr)))
#define SENSE_IWDG_FEED()  km_sense_sim_feed()
#else
#include "n32h48x_regs.h"
#define SENSE_REG(addr)    KM_REG32(addr)
#define SENSE_IWDG_FEED()  km_iwdg_feed()
#endif

/* ---- RCC (0x40021000) ---- */
#define SENSE_RCC_BASE          0x40021000u
#define SENSE_RCC_CFG2          SENSE_REG(SENSE_RCC_BASE + 0x08u)
#define SENSE_RCC_AHBPCLKEN     SENSE_REG(SENSE_RCC_BASE + 0x18u)   /* DMA1EN = bit 7 */
#define SENSE_RCC_APB1PCLKEN    SENSE_REG(SENSE_RCC_BASE + 0x1Cu)   /* GTIM1EN = bit 10 */
#define SENSE_RCC_APB2PCLKEN    SENSE_REG(SENSE_RCC_BASE + 0x20u)   /* AFIO = bit 0 */
#define SENSE_RCC_AHBPRST       SENSE_REG(SENSE_RCC_BASE + 0x24u)   /* ADC1-4 reset = bits 18-21 (inferred) */
#define SENSE_RCC_AHB1PCLKEN    SENSE_REG(SENSE_RCC_BASE + 0x3Cu)   /* GPIOA-F = bits 0-5, ADC1-4 = bits 9-12 */

#define SENSE_AHB1_GPIO_ALL     0x3Fu
#define SENSE_AHB1_ADC(n)       (0x100u << (n))                     /* n = 1..4 -> 0x200, 0x400, 0x800, 0x1000 */
#define SENSE_AHB1_ADC_ALL      0x1E00u
#define SENSE_AHB_DMA1          0x80u
#define SENSE_APB1_GTIM1        0x400u
#define SENSE_APB2_AFIO         0x1u
#define SENSE_AHBPRST_ADC_ALL   0x3C0000u

/* ---- AFIO (0x40010800) ---- */
#define SENSE_AFIO_RMP_CFG      SENSE_REG(0x40010800u)
#define SENSE_AFIO_TOL5V_CFG(i) SENSE_REG(0x40010814u + 4u * (i))   /* i = 0: GPIOA|GPIOB<<16, 1: C|D<<16, 2: E|F<<16 */
#define SENSE_AFIO_SWJ_MASK     0xFFFE3FFFu
#define SENSE_AFIO_SWJ_VALUE    0x8000u         /* SWJ_CFG field = 2: JTAG off, SWD kept */
#define SENSE_UNK_4001004C      SENSE_REG(0x4001004Cu)  /* not in the SVD labels; |= 0x2000 before every ADC init (both sets do it; meaning U) */
#define SENSE_UNK_4001004C_BIT  0x2000u

/* ---- GPIO ---- */
#define SENSE_GPIOA             0x4002C000u
#define SENSE_GPIOB             0x4002C400u
#define SENSE_GPIOC             0x4002C800u
#define SENSE_GPIOD             0x4002CC00u
#define SENSE_GPIOE             0x4002D000u
#define SENSE_GPIOF             0x4002D400u
#define SENSE_GPIO_PMODE(p)     SENSE_REG((p) + 0x00u)
#define SENSE_GPIO_PUPD(p)      SENSE_REG((p) + 0x04u)
#define SENSE_GPIO_POTYPE(p)    SENSE_REG((p) + 0x08u)
#define SENSE_GPIO_AF(p, i)     SENSE_REG((p) + 0x0Cu + 4u * (i))   /* 5 bits per pin, 4 pins per register */
#define SENSE_GPIO_DS(p)        SENSE_REG((p) + 0x1Cu)
#define SENSE_GPIO_SR(p)        SENSE_REG((p) + 0x20u)
#define SENSE_GPIO_PBSC(p)      SENSE_REG((p) + 0x24u)
#define SENSE_GPIO_PBC(p)       SENSE_REG((p) + 0x28u)
#define SENSE_GPIO_POD(p)       SENSE_REG((p) + 0x30u)

#define SENSE_PMODE_OUTPUT      1u
#define SENSE_PMODE_ANALOG      3u

/* The 20 analog input pins (FIRMWARE_Magger60.md section 4). */
#define SENSE_ANALOG_A          0x00FFu
#define SENSE_ANALOG_B          0x0003u
#define SENSE_ANALOG_C          0x0033u
#define SENSE_ANALOG_D          0x0600u
#define SENSE_ANALOG_E          0x0300u
#define SENSE_ANALOG_F          0x0180u

/* Mux address lines and the output the bootloader also drives low while it reads the keys. */
#define SENSE_MUX_PORT          SENSE_GPIOB
#define SENSE_MUX_SHIFT         3u                  /* PB3 = phase bit 0, PB4 = phase bit 1 */
#define SENSE_MUX_MASK          0x18u
#define SENSE_PB5_MASK          0x20u               /* output low during the bootloader's key read and in our scan */

/* ---- ADC1-4 ---- */
#define SENSE_ADC_BASE(n)       (0x4002B000u + 0x400u * ((n) - 1u))   /* n = 1..4 */
#define SENSE_ADC_STS(b)        SENSE_REG((b) + 0x00u)   /* bit0 EOC, bit2 JENDC, bit6 RDY (used by both sets) */
#define SENSE_ADC_CTRL1(b)      SENSE_REG((b) + 0x04u)
#define SENSE_ADC_CTRL2(b)      SENSE_REG((b) + 0x08u)   /* bit0 ADON, bit2 CAL, bit28 SWSTRT */
#define SENSE_ADC_CTRL3(b)      SENSE_REG((b) + 0x0Cu)
#define SENSE_ADC_SAMPT(b, i)   SENSE_REG((b) + 0x10u + 4u * (i))   /* i = 0: ch0-7, 1: ch8-15, 2: ch16-, 4 bits per channel */
#define SENSE_ADC_CALFACT(b)    SENSE_REG((b) + 0x20u)
#define SENSE_ADC_RSEQ(b, i)    SENSE_REG((b) + 0x64u + 4u * (i))   /* i = 0: ranks 1-6, 1: 7-12, 2: 13-16 + LEN[28:25] */
#define SENSE_ADC_JSEQ(b)       SENSE_REG((b) + 0x70u)
#define SENSE_ADC_JDAT1(b)      SENSE_REG((b) + 0x74u)
#define SENSE_ADC_DAT(b)        SENSE_REG((b) + 0x84u)   /* DAT1[15:0], DAT2[31:16] (the pair's partner) */
#define SENSE_ADC_PCTRL(b)      SENSE_REG((b) + 0x90u)
#define SENSE_ADC_INTEN(b)      SENSE_REG((b) + 0x98u)
#define SENSE_ADC_DAT_ADDR(n)   (SENSE_ADC_BASE(n) + 0x84u)

#define SENSE_ADC_STS_EOC       0x01u
#define SENSE_ADC_STS_JENDC     0x04u
#define SENSE_ADC_STS_RDY       0x40u
#define SENSE_ADC_CTRL2_ADON    0x01u
#define SENSE_ADC_CTRL2_CAL     0x04u
#define SENSE_ADC_CTRL2_SWSTRT  0x10000000u
#define SENSE_ADC_CTRL3_CALDIF  0x04u
#define SENSE_ADC_CTRL3_CLKSEL  0x10u                    /* set in set B (PLL clock), cleared in set A (HCLK clock) */
#define SENSE_ADC_INTEN_JENDC   0x04u

/* ADC mode words. "poll" = the bootloader's key-read configuration (proven on the keyboard), "scan" = set B (proven in S3). */
#define SENSE_POLL_CTRL1        0x00000000u
#define SENSE_POLL_CTRL2        0x002A0000u
#define SENSE_POLL_SAMPT        12u                      /* sample time code the bootloader uses */
#define SENSE_SCAN_CTRL1        0x00000081u              /* 0x80 | 1 */
#define SENSE_SCAN_CTRL2        0x00090010u
#define SENSE_SCAN_SAMPT        5u                       /* sample time code of set B */
#define SENSE_CTRL1_KEEP        0xFFFFF07Eu
#define SENSE_CTRL2_KEEP        0xFEC0FFCDu
#define SENSE_CTRL3_MODE        0x3u                     /* RES = 3 (12 bit, inferred) */
#define SENSE_PCTRL_KEEP        0xFFFFFFC7u
#define SENSE_PCTRL_SET         0x28u
#define SENSE_RSEQ3_KEEP        0xE1FFFFFFu              /* LEN field = bits [28:25] = number of regular conversions - 1 */
#define SENSE_JSEQ_KEEP_LEN     0xE1FFFFFFu
#define SENSE_SCAN_EXTRSEL_CTRL2_KEEP 0xFFFF033Fu
#define SENSE_SCAN_EXTRSEL_CTRL2_SET  0x2440u            /* EXTRSEL = 9 at bits [13:10] plus bit 6 */
#define SENSE_SCAN_MASTER_CTRL2_KEEP  0xFF3FFFFFu        /* ADC1 and ADC3 only */
#define SENSE_SCAN_MASTER_CTRL2_SET   0x00800000u

/* RCC_CFG2 ADC clock fields */
#define SENSE_CFG2_POLL_LOW_NIBBLE  8u                   /* bootloader: CFG2[3:0] = 8, CFG2[8:4] = 0, ADC CTRL3 bit4 = 0 */
#define SENSE_CFG2_ADC_KEEP         0xFFFFFE0Fu          /* CFG2[8:4] */
#define SENSE_CFG2_NIB_KEEP         0xFFFFFFF0u          /* CFG2[3:0] */
#define SENSE_CFG2_SCAN_FIELD       0x150u               /* 0x50 | 0x100 into the CFG2[8:4] region */
#define SENSE_CFG2_ALT_BIT          0x00080000u          /* bit 19: set in set B, clear in set A */
#define SENSE_CFG2_UPPER_KEEP       0xFFF60FFFu
#define SENSE_CFG2_UPPER_POLL       0x00007000u          /* bootloader */
#define SENSE_CFG2_UPPER_SCAN       0x00087000u          /* 0x80000 | 0x7000 */

/* ---- DMA1 (0x40020000), channel n = 1..8, 0x14 bytes each ---- */
#define SENSE_DMA_BASE          0x40020000u
#define SENSE_DMA_CH(n)         (SENSE_DMA_BASE + 0x14u * ((n) - 1u))
#define SENSE_DMA_CHCFG(n)      SENSE_REG(SENSE_DMA_CH(n) + 0x00u)
#define SENSE_DMA_MADDR(n)      SENSE_REG(SENSE_DMA_CH(n) + 0x04u)
#define SENSE_DMA_PADDR(n)      SENSE_REG(SENSE_DMA_CH(n) + 0x08u)
#define SENSE_DMA_CHSEL(n)      SENSE_REG(SENSE_DMA_CH(n) + 0x0Cu)
#define SENSE_DMA_TXNUM(n)      SENSE_REG(SENSE_DMA_CH(n) + 0x10u)
#define SENSE_DMA_INTCLR        SENSE_REG(SENSE_DMA_BASE + 0xA0u)
#define SENSE_DMA_INTSTS        SENSE_REG(SENSE_DMA_BASE + 0xA4u)
#define SENSE_DMA_BURSTEN       SENSE_REG(SENSE_DMA_BASE + 0xA8u)

/* CHCFG: MINC | PSIZE=32 | MSIZE=32 | CIRC | priority 3 = 0x3AA0; +2 TC interrupt, +1 enable. */
#define SENSE_DMA_CHCFG_BASE    0x3AA0u
#define SENSE_DMA_CHCFG_KEEP    0xFFF8000Fu
#define SENSE_DMA_CHCFG_TCIE    0x2u
#define SENSE_DMA_CHCFG_EN      0x1u
#define SENSE_DMA_CH_ADC12      1u
#define SENSE_DMA_CH_ADC34      3u
#define SENSE_DMA_CHSEL_ADC12   0u                       /* CHSEL1 = 0 */
#define SENSE_DMA_CHSEL_ADC34   2u                       /* CHSEL3 = 2 */
#define SENSE_DMA_COUNT_ADC12   6u
#define SENSE_DMA_COUNT_ADC34   3u
#define SENSE_DMA_INTCLR_CH1    0x3u                     /* IRQ11 writes 3  */
#define SENSE_DMA_INTCLR_CH3    0x300u                   /* IRQ13 writes 0x300 */
#define SENSE_DMA_INTCLR_CH1_ALL 0xFu
#define SENSE_DMA_INTCLR_CH3_ALL 0xF00u
#define SENSE_DMA_TEIF_CH1      0x8u                     /* assumed STM32-style: nibble = GIF, TCIF, HTIF, TEIF */
#define SENSE_DMA_TEIF_CH3      0x800u

/* ---- GTIM1 (0x40002C00), the ADC trigger timer ---- */
#define SENSE_GTIM1_BASE        0x40002C00u
#define SENSE_GTIM1_CTRL1       SENSE_REG(SENSE_GTIM1_BASE + 0x00u)
#define SENSE_GTIM1_CTRL2       SENSE_REG(SENSE_GTIM1_BASE + 0x04u)
#define SENSE_GTIM1_EVTGEN      SENSE_REG(SENSE_GTIM1_BASE + 0x0Cu)
#define SENSE_GTIM1_CCMOD2      SENSE_REG(SENSE_GTIM1_BASE + 0x1Cu)
#define SENSE_GTIM1_CCEN        SENSE_REG(SENSE_GTIM1_BASE + 0x24u)
#define SENSE_GTIM1_CCDAT4      SENSE_REG(SENSE_GTIM1_BASE + 0x34u)
#define SENSE_GTIM1_PSC         SENSE_REG(SENSE_GTIM1_BASE + 0x40u)
#define SENSE_GTIM1_AR          SENSE_REG(SENSE_GTIM1_BASE + 0x44u)
#define SENSE_GTIM1_CNT         SENSE_REG(SENSE_GTIM1_BASE + 0x48u)
#define SENSE_GTIM1_INSEL       SENSE_REG(SENSE_GTIM1_BASE + 0x78u)
#define SENSE_GTIM_CEN          0x1u
#define SENSE_GTIM1_PSC_VALUE   0x17u
#define SENSE_GTIM1_AR_VALUE    0xFFFEu
#define SENSE_GTIM1_CCDAT4_VALUE 7u
#define SENSE_GTIM1_EVTGEN_VALUE 0x100u

/* ---- NVIC and DWT (Cortex-M4) ---- */
#define SENSE_NVIC_ISER(i)      SENSE_REG(0xE000E100u + 4u * (i))
#define SENSE_NVIC_ICER(i)      SENSE_REG(0xE000E180u + 4u * (i))
#define SENSE_NVIC_ICPR(i)      SENSE_REG(0xE000E280u + 4u * (i))   /* clear pending */
#define SENSE_NVIC_IPR(w)       SENSE_REG(0xE000E400u + 4u * (w))  /* priority bytes, word w holds IRQs 4w..4w+3 */
#define SENSE_IRQ_DMA1_CH1      11u
#define SENSE_IRQ_DMA1_CH3      13u
#define SENSE_IRQ_ADC1_2        18u
#define SENSE_IRQ_ADC3_4        47u
#define SENSE_DEMCR             SENSE_REG(0xE000EDFCu)
#define SENSE_DWT_CTRL          SENSE_REG(0xE0001000u)
#define SENSE_DWT_CYCCNT        SENSE_REG(0xE0001004u)

/* Progress marker: a RAM word far above the stack and bss (RAM is 0x20000000-0x20027FFF) that no startup code clears, so
 * its value survives a watchdog reset and shows how far the previous run got. */
#define SENSE_STAGE_ADDR        0x20026F08u
#define SENSE_STAGE_REG         SENSE_REG(SENSE_STAGE_ADDR)

/* CPU clock the delays assume (8 MHz HSE x 30 = 240 MHz, FIRMWARE_Magger60.md section 1). If the real clock is lower the delays are only longer. */
#define SENSE_CPU_HZ            240000000u
#define SENSE_CYCLES_PER_US     (SENSE_CPU_HZ / 1000000u)

#endif /* SENSE_REGS_H */
