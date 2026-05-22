/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32h7xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ============================================================
 * HardFault diagnostic.
 *
 * HardFault_Handler (below) is a naked trampoline that selects MSP/PSP
 * based on EXC_RETURN bit 2, then tail-calls HardFault_Handler_C with:
 *   r0 = pointer to the 8-word hardware-stacked exception frame
 *   r1 = EXC_RETURN value that was in LR on entry
 *
 * The C handler latches the frame + fault status registers into globals
 * so they survive across the bkpt and are readable from Live Expressions.
 * ============================================================ */
volatile uint32_t g_fault_active     = 0;   /* set to 1 once a fault has been captured */
volatile uint32_t g_fault_cfsr       = 0;   /* SCB->CFSR  (MMFSR/BFSR/UFSR combined) */
volatile uint32_t g_fault_hfsr       = 0;   /* SCB->HFSR  */
volatile uint32_t g_fault_mmfar      = 0;   /* SCB->MMFAR (valid only if CFSR bit 7 set)  */
volatile uint32_t g_fault_bfar       = 0;   /* SCB->BFAR  (valid only if CFSR bit 15 set) */
volatile uint32_t g_fault_r0         = 0;
volatile uint32_t g_fault_r1         = 0;
volatile uint32_t g_fault_r2         = 0;
volatile uint32_t g_fault_r3         = 0;
volatile uint32_t g_fault_r12        = 0;
volatile uint32_t g_fault_lr         = 0;   /* return addr that called the faulting code */
volatile uint32_t g_fault_pc         = 0;   /* address of the faulting instruction */
volatile uint32_t g_fault_xpsr       = 0;
volatile uint32_t g_fault_exc_return = 0;   /* EXC_RETURN that was in LR on entry */

void HardFault_Handler_C(uint32_t *sp, uint32_t exc_return)
{
    /* All locals declared at top of block for C89-compatible parsers.
     * Allowed sp ranges for this board:
     *   DTCM (D1):  0x20000000..0x2001FFFF (128K)
     *   AXI  (D1):  0x24000000..0x2407FFFF (512K)
     *   SRAM (D2):  0x30000000..0x30047FFF (288K)
     *   SRAM (D3):  0x38000000..0x3800FFFF (64K)
     * The frame needs 32 bytes (basic) or 0x68 bytes (extended FP frame). */
    uint32_t a;
    int sp_ok;

    /* Capture SCB fault-status registers FIRST. These are SCS-space (always
     * mapped, never faults) so we are guaranteed to record them even if the
     * subsequent stacked-frame reads below blow up. */
    g_fault_cfsr       = *(volatile uint32_t *)0xE000ED28u;
    g_fault_hfsr       = *(volatile uint32_t *)0xE000ED2Cu;
    g_fault_mmfar      = *(volatile uint32_t *)0xE000ED34u;
    g_fault_bfar       = *(volatile uint32_t *)0xE000ED38u;
    g_fault_exc_return = exc_return;
    g_fault_active     = 1;   /* Mark captured EARLY: tells you we got here. */

    a = (uint32_t)sp;
    sp_ok =
        (a >= 0x20000000u && a + 0x68u <= 0x20020000u) ||
        (a >= 0x24000000u && a + 0x68u <= 0x24080000u) ||
        (a >= 0x30000000u && a + 0x68u <= 0x30048000u) ||
        (a >= 0x38000000u && a + 0x68u <= 0x38010000u);

    if (sp_ok) {
        g_fault_r0   = sp[0];
        g_fault_r1   = sp[1];
        g_fault_r2   = sp[2];
        g_fault_r3   = sp[3];
        g_fault_r12  = sp[4];
        g_fault_lr   = sp[5];
        g_fault_pc   = sp[6];
        g_fault_xpsr = sp[7];
    } else {
        /* Sentinel so you can tell "frame not read" from "frame was zero". */
        g_fault_r0   = 0xDEADBEEFu;
        g_fault_r1   = 0xDEADBEEFu;
        g_fault_r2   = 0xDEADBEEFu;
        g_fault_r3   = 0xDEADBEEFu;
        g_fault_r12  = 0xDEADBEEFu;
        g_fault_lr   = 0xDEADBEEFu;
        g_fault_pc   = (uint32_t)a;    /* the bad sp value itself */
        g_fault_xpsr = 0xDEADBEEFu;
    }

    __asm volatile ("dsb 0xF":::"memory");   /* commit writes before halting */
    __asm volatile ("bkpt #0");
    while (1) { }
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ETH_HandleTypeDef heth;
extern TIM_HandleTypeDef htim5;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  *        Naked trampoline: picks the correct stack pointer (MSP if
  *        EXC_RETURN bit 2 == 0, otherwise PSP) and tail-calls the
  *        C diagnostic handler defined in the USER CODE BEGIN 0 block.
  *        NOTE: must remain naked so the compiler does not push a frame
  *        that would obscure the stacked exception state.
  */
__attribute__((naked))
void HardFault_Handler(void)
{
  __asm volatile (
      " tst   lr, #4              \n"
      " ite   eq                  \n"
      " mrseq r0, msp             \n"
      " mrsne r0, psp             \n"
      " mov   r1, lr              \n"
      " b     HardFault_Handler_C \n"
  );
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32H7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM5 global interrupt.
  */
void TIM5_IRQHandler(void)
{
  /* USER CODE BEGIN TIM5_IRQn 0 */

  /* USER CODE END TIM5_IRQn 0 */
  HAL_TIM_IRQHandler(&htim5);
  /* USER CODE BEGIN TIM5_IRQn 1 */

  /* USER CODE END TIM5_IRQn 1 */
}

/**
  * @brief This function handles Ethernet global interrupt.
  */
void ETH_IRQHandler(void)
{
  /* USER CODE BEGIN ETH_IRQn 0 */

  /* USER CODE END ETH_IRQn 0 */
  HAL_ETH_IRQHandler(&heth);
  /* USER CODE BEGIN ETH_IRQn 1 */

  /* USER CODE END ETH_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
