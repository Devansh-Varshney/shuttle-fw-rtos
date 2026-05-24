/**
 * @file uart_debug.h
 * @brief UART-based debug logging with DMA support and message queuing
 * @version 2.0
 * @date 2024
 * 
 * Features:
 * - DMA-based non-blocking transmission
 * - Double-buffered DMA to prevent corruption
 * - Message queue to prevent message loss
 * - Dropped message counter for diagnostics
 */

#ifndef UART_DEBUG_H
#define UART_DEBUG_H

#include "stm32h7xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// ============================================================================
// Configuration - Adjust these based on your setup
// ============================================================================

#ifndef DEBUG_UART
#define DEBUG_UART &huart7  // Change to your UART handle (e.g., &huart1, &huart3)
#endif

#ifndef DEBUG_BUFFER_SIZE
#define DEBUG_BUFFER_SIZE 256  // Maximum single message size
#endif

#ifndef MSG_QUEUE_SIZE
#define MSG_QUEUE_SIZE 64  // Number of messages that can be queued (increased from 16)
#endif

// ============================================================================
// External Variables (used by interrupt handlers)
// ============================================================================

extern DMA_HandleTypeDef hdma_uart7_tx;
extern volatile bool uart_debug_dma_active;

// ============================================================================
// Function Prototypes
// ============================================================================

/**
 * @brief Initialize UART debug system with DMA
 * @note Call this after HAL_Init() and UART initialization
 */
void uart_debug_init(void);

/**
 * @brief Print formatted debug message (printf-style)
 * @param format: Format string (printf-style)
 * @param ...: Variable arguments
 * @note Messages are queued. If queue is full, message is dropped.
 */
void uart_debug_print(const char *format, ...);

/**
 * @brief Print raw string without formatting
 * @param str: Null-terminated string to print
 */
void uart_debug_print_raw(const char *str);

/**
 * @brief Print binary data as hex dump
 * @param data: Pointer to data buffer
 * @param len: Length of data in bytes
 */
void uart_debug_print_hex(const uint8_t *data, uint16_t len);

/**
 * @brief Get current number of messages in queue
 * @return Number of queued messages waiting to be sent
 */
uint8_t uart_debug_get_queue_count(void);

/**
 * @brief Get total number of dropped messages
 * @return Count of messages dropped due to full queue
 */
uint32_t uart_debug_get_dropped_count(void);

/**
 * @brief Reset dropped message counter
 */
void uart_debug_reset_dropped_count(void);

/**
 * @brief Check if UART debug is currently transmitting
 * @return true if DMA transmission is active, false otherwise
 */
bool uart_debug_is_busy(void);

/**
 * @brief Manually process queue (call from main loop if needed)
 * @note Useful if DMA callback is not firing properly
 */
void uart_debug_process(void);

/**
 * @brief Print queue diagnostics (for debugging queue issues)
 * @note Uses blocking UART to avoid recursion
 */
void uart_debug_print_diagnostics(void);

// ============================================================================
// Convenience Macros for Different Log Levels
// ============================================================================

#define DEBUG_VERBOSE(msg, ...) uart_debug_print("[VERBOSE] " msg, ##__VA_ARGS__)
#define DEBUG_INFO(msg, ...)    uart_debug_print("[INFO] " msg, ##__VA_ARGS__)
#define DEBUG_WARN(msg, ...)    uart_debug_print("[WARN] " msg, ##__VA_ARGS__)
#define DEBUG_ERROR(msg, ...)   uart_debug_print("[ERROR] " msg, ##__VA_ARGS__)

#endif // UART_DEBUG_H
