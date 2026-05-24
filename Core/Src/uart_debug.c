/**
 * @file uart_debug.c
 * @brief UART-based debug logging implementation with DMA support and message queuing
 * @version 2.0
 * 
 * This implementation uses:
 * - Double-buffered DMA to prevent data corruption
 * - Message queue to prevent message loss on consecutive calls
 * - Non-blocking operation to avoid stalling main application
 */

#include "uart_debug.h"
#include "usart.h"
// ============================================================================
// DMA Configuration
// ============================================================================

#define DMA_TX_BUFFER_SIZE 1024

// Double-buffer DMA buffers (ping-pong) - aligned for DMA and cache
#if defined(__ICCARM__)
#pragma data_alignment=32
#elif defined(__CC_ARM) || defined(__GNUC__)
__attribute__((aligned(32)))
#endif
static char dma_tx_buffer[2][DMA_TX_BUFFER_SIZE];

static uint8_t dma_buffer_index = 0;  // Current buffer in use (0 or 1)
volatile bool uart_debug_dma_active = false;

// ============================================================================
// Message Queue Implementation
// ============================================================================

typedef struct {
    char data[DMA_TX_BUFFER_SIZE];
    uint16_t length;
} uart_message_t;

static uart_message_t msg_queue[MSG_QUEUE_SIZE];
static volatile uint8_t queue_head = 0;  // Read position
static volatile uint8_t queue_tail = 0;  // Write position
static volatile uint8_t queue_count = 0; // Number of items in queue
static volatile uint32_t dropped_msg_count = 0;  // Diagnostic counter

// ============================================================================
// DMA Handle and Forward Declarations
// ============================================================================

DMA_HandleTypeDef hdma_uart7_tx;

static void uart_debug_send_next_message(void);
static void uart7_dma_init(void);
void uart7_dma_tx_complete(DMA_HandleTypeDef *hdma);

// ============================================================================
// DMA Initialization
// ============================================================================

/**
 * @brief Initialize UART7 DMA (Stream 1 for TX)
 */
static void uart7_dma_init(void)
{
    // Configure DMA Handle
    hdma_uart7_tx.Instance = DMA1_Stream1;
    hdma_uart7_tx.Init.Request = DMA_REQUEST_UART7_TX;
    hdma_uart7_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_uart7_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_uart7_tx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_uart7_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_uart7_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_uart7_tx.Init.Mode = DMA_NORMAL;
    hdma_uart7_tx.Init.Priority = DMA_PRIORITY_MEDIUM;
    hdma_uart7_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    
    // Initialize DMA
    if (HAL_DMA_Init(&hdma_uart7_tx) != HAL_OK)
    {
        // DMA initialization error - infinite loop for debugging
        while(1);
    }
    
    // Link DMA to UART7
    __HAL_LINKDMA(&huart7, hdmatx, hdma_uart7_tx);
    
    // Register DMA TX complete callback
    HAL_DMA_RegisterCallback(&hdma_uart7_tx, HAL_DMA_XFER_CPLT_CB_ID, uart7_dma_tx_complete);
}

// ============================================================================
// DMA Callback
// ============================================================================

/**
 * @brief DMA TX complete callback
 * @note This is called from interrupt context
 */
void uart7_dma_tx_complete(DMA_HandleTypeDef *hdma)
{
    uart_debug_dma_active = false;
    
    // Automatically send next message if queue has items
    uart_debug_send_next_message();
}

// ============================================================================
// Queue Management
// ============================================================================

/**
 * @brief Send the next message from the queue
 * @note This function should be called with interrupts disabled or from ISR
 */
static void uart_debug_send_next_message(void)
{
    // Check if queue is empty or DMA is busy
    if (queue_count == 0 || uart_debug_dma_active)
    {
        return;
    }
    
    // Get the next buffer to use (alternate between 0 and 1)
    uint8_t next_buffer_idx = (dma_buffer_index == 0) ? 1 : 0;
    char *tx_buffer = dma_tx_buffer[next_buffer_idx];
    
    // Copy message from queue to DMA buffer
    memcpy(tx_buffer, msg_queue[queue_head].data, msg_queue[queue_head].length);
    uint16_t msg_len = msg_queue[queue_head].length;
    
    // Update queue indices
    queue_head = (queue_head + 1) % MSG_QUEUE_SIZE;
    queue_count--;
    
    // Update buffer index
    dma_buffer_index = next_buffer_idx;
    
    // Set DMA active flag
    uart_debug_dma_active = true;
    
    // Clean D-Cache before DMA transfer (required for STM32H7)
    SCB_CleanDCache_by_Addr((uint32_t*)tx_buffer, DMA_TX_BUFFER_SIZE);
    
    // Start DMA transmission
    HAL_UART_Transmit_DMA(DEBUG_UART, (uint8_t*)tx_buffer, msg_len);
}

// ============================================================================
// Public API Functions
// ============================================================================

/**
 * @brief Initialize UART debug system
 * @note Call this after HAL_Init() and MX_USARTx_UART_Init()
 */
void uart_debug_init(void)
{
    // Initialize queue
    queue_head = 0;
    queue_tail = 0;
    queue_count = 0;
    dropped_msg_count = 0;
    uart_debug_dma_active = false;
    
    // Clear DMA buffers
    memset(dma_tx_buffer[0], 0, DMA_TX_BUFFER_SIZE);
    memset(dma_tx_buffer[1], 0, DMA_TX_BUFFER_SIZE);
    
    // Initialize DMA for UART7
    uart7_dma_init();
    
    // Send startup banner via blocking transmit
    // const char *banner = 
    //     "\r\n"
    //     "========================================\r\n"
    //     "  STM32H753 UART Debug Logger v2.0\r\n"
    //     "  DMA + Message Queue Active\r\n"
    //     "  Baud: 921600 | Queue Size: ""\r\n"
    //     "========================================\r\n\r\n";
    
    // HAL_UART_Transmit(DEBUG_UART, (uint8_t*)banner, strlen(banner), 1000);
}

/**
 * @brief Print formatted debug message via UART DMA (with queuing)
 * @param format: printf-style format string
 * @param ...: Variable arguments
 */
void uart_debug_print(const char *format, ...)
{
    char temp_buffer[DEBUG_BUFFER_SIZE];
    va_list args;
    
    va_start(args, format);
    int len = vsnprintf(temp_buffer, DEBUG_BUFFER_SIZE - 1, format, args);
    va_end(args);
    
    // Check if formatting was successful
    if (len <= 0 || len >= DEBUG_BUFFER_SIZE - 1)
    {
        return;
    }
    
    // Critical section: modify queue
    uint32_t primask_bit = __get_PRIMASK();
    __disable_irq();
    
    // Check if queue is full
    if (queue_count >= MSG_QUEUE_SIZE)
    {
        dropped_msg_count++;
        __set_PRIMASK(primask_bit);
        return;
    }
    
    // Add message to queue
    uint16_t total_len = len + 2;  // +2 for \r\n
    if (total_len <= DMA_TX_BUFFER_SIZE)
    {
        memcpy(msg_queue[queue_tail].data, temp_buffer, len);
        msg_queue[queue_tail].data[len] = '\r';
        msg_queue[queue_tail].data[len + 1] = '\n';
        msg_queue[queue_tail].length = total_len;
        
        // Update queue tail
        queue_tail = (queue_tail + 1) % MSG_QUEUE_SIZE;
        queue_count++;
        
        // If DMA is idle, start transmission immediately
        if (!uart_debug_dma_active)
        {
            uart_debug_send_next_message();
        }
    }
    
    __set_PRIMASK(primask_bit);
}

/**
 * @brief Print raw string without formatting
 * @param str: String to print
 */
void uart_debug_print_raw(const char *str)
{
    if (str == NULL)
    {
        return;
    }
    
    uint16_t len = strlen(str);
    
    // Check length
    if (len == 0 || len > DMA_TX_BUFFER_SIZE)
    {
        return;
    }
    
    // Critical section: modify queue
    uint32_t primask_bit = __get_PRIMASK();
    __disable_irq();
    
    // Check if queue is full
    if (queue_count >= MSG_QUEUE_SIZE)
    {
        dropped_msg_count++;
        __set_PRIMASK(primask_bit);
        return;
    }
    
    // Add to queue
    memcpy(msg_queue[queue_tail].data, str, len);
    msg_queue[queue_tail].length = len;
    
    queue_tail = (queue_tail + 1) % MSG_QUEUE_SIZE;
    queue_count++;
    
    // If DMA is idle, start transmission
    if (!uart_debug_dma_active)
    {
        uart_debug_send_next_message();
    }
    
    __set_PRIMASK(primask_bit);
}

/**
 * @brief Print binary data as hex dump
 * @param data: Pointer to data
 * @param len: Length of data
 */
void uart_debug_print_hex(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0)
    {
        return;
    }
    
    uart_debug_print("Hex Dump (%d bytes):", len);
    
    for (uint16_t i = 0; i < len; i++)
    {
        uart_debug_print("%02X ", data[i]);
        
        if ((i + 1) % 16 == 0)
        {
            uart_debug_print("");  // Empty string will just add \r\n
        }
    }
    
    if (len % 16 != 0)
    {
        uart_debug_print("");  // Final line break
    }
}

/**
 * @brief Get current number of messages in queue
 * @return Number of queued messages
 */
uint8_t uart_debug_get_queue_count(void)
{
    return queue_count;
}

/**
 * @brief Get total number of dropped messages
 * @return Count of dropped messages
 */
uint32_t uart_debug_get_dropped_count(void)
{
    return dropped_msg_count;
}

/**
 * @brief Reset dropped message counter
 */
void uart_debug_reset_dropped_count(void)
{
    uint32_t primask_bit = __get_PRIMASK();
    __disable_irq();
    dropped_msg_count = 0;
    __set_PRIMASK(primask_bit);
}

/**
 * @brief Check if UART debug is currently transmitting
 * @return true if DMA is active, false otherwise
 */
bool uart_debug_is_busy(void)
{
    return uart_debug_dma_active;
}

/**
 * @brief Manually process the queue (call from main loop)
 * @note Use this if DMA callbacks aren't working or for polling mode
 */
void uart_debug_process(void)
{
    if (!uart_debug_dma_active && queue_count > 0)
    {
        uint32_t primask_bit = __get_PRIMASK();
        __disable_irq();
        uart_debug_send_next_message();
        __set_PRIMASK(primask_bit);
    }
}

/**
 * @brief Print queue diagnostics using blocking UART (for debugging)
 * @note This bypasses the queue to show queue status
 */
void uart_debug_print_diagnostics(void)
{
    char diag_buf[256];
    int len = snprintf(diag_buf, sizeof(diag_buf),
                       "\r\n=== UART DEBUG DIAGNOSTICS ===\r\n"
                       "Queue Count: %d/%d\r\n"
                       "Dropped: %lu\r\n"
                       "DMA Active: %s\r\n"
                       "Head: %d, Tail: %d\r\n"
                       "===============================\r\n",
                       queue_count, MSG_QUEUE_SIZE,
                       dropped_msg_count,
                       uart_debug_dma_active ? "YES" : "NO",
                       queue_head, queue_tail);
    
    // Send via blocking UART (bypasses queue)
    HAL_UART_Transmit(DEBUG_UART, (uint8_t*)diag_buf, len, 1000);
}

// ============================================================================
// Printf Redirection (Optional)
// ============================================================================

/**
 * @brief Redirect printf to UART (for legacy code compatibility)
 */
#ifdef __GNUC__
int _write(int file, char *ptr, int len)
{
    HAL_UART_Transmit(DEBUG_UART, (uint8_t*)ptr, len, HAL_MAX_DELAY);
    return len;
}
#endif
