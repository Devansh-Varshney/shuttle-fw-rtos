/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "cmsis_os.h"
#include "lwip.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "lwip/sockets.h"
#include "mqtt_task.h"
#include "json.h"
#include "diag.h"
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */
//osThreadId_t serverHandle;
//const osThreadAttr_t serverTaskAttr = {
//		  .name = "tcpServer",
//		  .stack_size = 768 * 4, // bytes (words × 4)
//		  .priority = osPriorityAboveNormal };
//
//osThreadId_t clientHandle;
//const osThreadAttr_t clientTaskAttr = {
//		  .name = "tcpClient",
//		  .stack_size = 768 * 4,
//		  .priority = osPriorityAboveNormal };
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
void StartDefaultTask(void *argument);

/* USER CODE BEGIN PFP */
void tcp_server_task(void *argument);
void tcp_client_task(void *argument);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Handler for the MQTT subscription on "shuttle/wcs".
 * Runs on lwIP's tcpip_thread - must be FAST, no blocking, no slow I/O.
 * Just enqueues the raw bytes to the JSON worker task for parsing. */
static void on_cmd(const char *t, const void *p, size_t n, void *ctx)
{
    (void)t; (void)ctx;
    json_handler_enqueue(p, n);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  static const mqtt_config_t mqtt_cfg = {
      .broker_ip    = "192.168.0.37",
      .broker_port  = 1883,
      .client_id    = "shuttle_1",
      .keep_alive_s = 60,
      .username     = NULL,
      .password     = NULL,
  };
  mqtt_init(&mqtt_cfg);
  json_handler_init();
  diag_init();

  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 50;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOG, USR2_LED_Pin|USR1_LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : USR2_LED_Pin USR1_LED_Pin */
  GPIO_InitStruct.Pin = USR2_LED_Pin|USR1_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
//void tcp_server_task(void *argument)
//{
//
//    int sock, client;
//    struct sockaddr_in server_addr, client_addr;
//    char rx_buffer[128];
//    socklen_t addr_len;
//    int len;
//
//    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
//    if(sock < 0)
//        vTaskDelete(NULL);
//
//    server_addr.sin_family = AF_INET;
//    server_addr.sin_port = htons(5000);
//    server_addr.sin_addr.s_addr = INADDR_ANY;
//
//    bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
//    listen(sock, 1);
//
//    while(1)
//    {
//        addr_len = sizeof(client_addr);
//
//        client = accept(sock, (struct sockaddr *)&client_addr, &addr_len);
//
//        if(client >= 0)
//        {
//            while((len = recv(client, rx_buffer, sizeof(rx_buffer) - 1, 0)) > 0)
//            {
//                rx_buffer[len] = '\0';   // Null terminate
//
//                /* ---- COMMAND PARSING ---- */
//
//                if(strstr(rx_buffer, "LED ON"))
//                {
//                    HAL_GPIO_WritePin(USR1_LED_GPIO_Port, USR1_LED_Pin, GPIO_PIN_RESET);
//
//                    char reply[] = "\nLED turned ON\r\n";
//                    send(client, reply, strlen(reply), 0);
//                }
//                else if(strstr(rx_buffer, "LED OFF"))
//                {
//                    HAL_GPIO_WritePin(USR1_LED_GPIO_Port, USR1_LED_Pin, GPIO_PIN_SET);
//
//                    char reply[] = "\nLED turned OFF\r\n";
//                    send(client, reply, strlen(reply), 0);
//                }
//                else
//                {
//                    char reply[] = "\nUnknown Command\r\n";
//                    send(client, reply, strlen(reply), 0);
//                }
//            }
//
//            closesocket(client);
//        }
//    }
//
//
//
////    int sock, client;
////    struct sockaddr_in server_addr, client_addr;
////    char rx_buffer[128];
////    socklen_t addr_len;
////    int len;
////
////    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
////
////    if(sock < 0)
////        vTaskDelete(NULL);
////
////    server_addr.sin_family = AF_INET;
////    server_addr.sin_port = htons(5000);
////    server_addr.sin_addr.s_addr = INADDR_ANY;
////
////    bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
////    listen(sock, 1);
////
////    while(1)
////    {
////        client = accept(sock, (struct sockaddr *)&client_addr, &addr_len);
////
////        if(client >= 0)
////        {
////            HAL_GPIO_WritePin(USR1_LED_GPIO_Port, USR1_LED_Pin, GPIO_PIN_RESET);
////
////            while((len = recv(client, rx_buffer, sizeof(rx_buffer), 0)) > 0)
////            {
////                send(client, rx_buffer, len, 0);
////            }
////
////            HAL_GPIO_WritePin(USR1_LED_GPIO_Port, USR1_LED_Pin, GPIO_PIN_SET);
////            closesocket(client);
////        }
////}
//
//}
//void tcp_client_task(void *argument)
//{
//    int sock;
//    struct sockaddr_in server_addr;
//    char msg[] = "Hello from STM32H7\r\n";
//
//    while(1)
//    {
//        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
//
//        if(sock < 0)
//        {
//            osDelay(2000);
//            continue;
//        }
//
//        server_addr.sin_family = AF_INET;
//        server_addr.sin_port = htons(6000);
//
//        /* CHANGE to your PC/server IP */
//        server_addr.sin_addr.s_addr = inet_addr("192.168.0.124");
//
//        if(connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0)
//        {
//            /* Connected → LED ON (active LOW) */
//        	HAL_GPIO_WritePin(USR2_LED_GPIO_Port, USR2_LED_Pin, GPIO_PIN_RESET);
//
//            send(sock, msg, strlen(msg), 0);
//
//            closesocket(sock);
//            vTaskDelay(pdMS_TO_TICKS(200));
//            /* Disconnect → LED OFF */
//            HAL_GPIO_WritePin(USR2_LED_GPIO_Port, USR2_LED_Pin, GPIO_PIN_SET);
//        }
//        else
//        {
//            /* Connection failed -> LED OFF.
//             * BUG FIX: the socket was allocated by socket() above. If we
//             * don't closesocket() here, the underlying TCP PCB leaks.
//             * Every 3 s of failed connect leaks one MEMP_NUM_TCP_PCB slot,
//             * which eventually causes MQTT (and any other TCP user) to get
//             * ERR_MEM on reconnect (status=201). */
//        	HAL_GPIO_WritePin(USR2_LED_GPIO_Port, USR2_LED_Pin, GPIO_PIN_SET);
//        	closesocket(sock);
//        }
//
//        osDelay(3000);
//    }
//}
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for LWIP */
	osDelay(pdMS_TO_TICKS(1000));
  MX_LWIP_Init();
  /* USER CODE BEGIN 5 */
//  serverHandle = osThreadNew(tcp_server_task, NULL, &serverTaskAttr);
//
// if(serverHandle == NULL)
// {
//     Error_Handler();
// }
//
// clientHandle = osThreadNew(tcp_client_task, NULL, &clientTaskAttr);
//
// if(clientHandle == NULL)
// {
//     Error_Handler();
// }

 /* Register the test subscription. Done once, on a task (so the mutex
  * inside mqtt_subscribe is safe to use). The registration is kept in
  * our subscription table; the mqtt_task will replay it to the broker
  * automatically on every connect, including after a cable cycle. */
 mqtt_app_subscribe("shuttle/shuttle1/wcs", 1, on_cmd, NULL);

  /* Infinite loop */
  for(;;)
  {
    osDelay(5000);   // let MQTT connect first
mqtt_app_publish_string("shuttle/shuttle1/telemetry", "This is shuttle 1!!!");
  }
  /* USER CODE END 5 */
}

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x30020000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_128KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x30040000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_512B;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM5 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM5)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
