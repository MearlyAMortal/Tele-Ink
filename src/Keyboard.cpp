#include "Keyboard.h"
#include <Wire.h>
#include "Display.h"
#include "Modem.h"
#include "Command.h"
//FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"


static TwoWire *ikey_i2c = nullptr;
static uint8_t ikey_addr = 0x5F;
static TaskHandle_t key_task = NULL;
static QueueHandle_t key_queue = NULL;
static SemaphoreHandle_t key_mutex = NULL;
static volatile bool key_task_run = false;

#define POLL_MS 100

// FD
static bool handle_special_key(uint8_t &kc);
static bool i2c_read_key(uint8_t &out);
static void keyTask(void *pv);
static void Keyboard_Start(void);
static void Keyboard_Stop(void);
static bool Keyboard_IsConnected(void);


// Maps special keycodes to events
static bool handle_special_key(uint8_t &kc) {
    switch (kc) {
        case 0x9F: { // Homescreen
            Display_Event_ShowHome();
            return true; 
        }
        case 0x94: { // Idle
            Display_Event_ShowIdle();
            return true;
        }
        case 0xA8: { // Command
            Display_Event_ShowCommand();
            return true;
        }
        case 0x80: { // Wake/Sleep toggle
            if (screen_on) {
                Display_Event_Sleep();
            } else {
                Display_Event_Wake();
            }
            return true;
        }
        default:
            printf("Keycode: 0x%02X unknown.\r\n", kc);
            return false;
    }
    return false;
}

static bool i2c_read_key(uint8_t &out) {
    if (!ikey_i2c || !key_mutex) return false;
    if (xSemaphoreTake(key_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Prepare kb
        ikey_i2c->beginTransmission(ikey_addr);
        // que one byte into tx buff from register ptr
        ikey_i2c->write(0x00);
        // End write phase but dont stop, and take tx wire error code
        uint8_t tx = ikey_i2c->endTransmission(false);
        uint8_t req = ikey_i2c->requestFrom((int)ikey_addr, 1);
        bool ok = (tx == 0) && (req == 1);
        if (ok) out = ikey_i2c->read();

        xSemaphoreGive(key_mutex);
        if (ok && out != 0x00){
            return true;
        } 
    }
    return false;
}

// No debounce, just read and handle special keys
static void keyTask(void *pv) {
    (void)pv;
    uint8_t keycode = 0;
    uint8_t last = 0;
    uint32_t timeout_count = 0;
    bool timeout = false;
    // Buffer for sequential mode building strings to send to command processor
    bool sequential_mode = false;
    static char line_buffer[CMD_BUFFER_SIZE];
    static size_t line_pos = 0;

    for (;;) {
        if (!key_task_run) {
            DEV_Delay_ms(POLL_MS*1000);
            continue;
        }

        keycode = 0;

        // Read Keyboard data, false if 0x00 (no key)
        if (!i2c_read_key(keycode) ) {
            // Idle timeout handling
            DEV_Delay_ms(POLL_MS);
            continue;
        }

        // Key pressed
        timeout_count = 0;
        timeout = false;
        // Handle special key if mapped (exit sequential and return to base handling)
        if (keycode >= 0x80 && keycode <= 0xAF) {
            last = keycode;
            if (!handle_special_key(keycode)) continue; // Not mapped
            line_pos = 0;
            line_buffer[0] = '\0';
            sequential_mode = false;
            continue;
        }

        // Handle normal keys in sequential mode (Must take mutex to update cmd_buffer)
        // Build local line buffer and update cmd_buffer input for command processing
        sequential_mode = (current_page == PAGE_COMMAND);

        if (sequential_mode) { 
            if (keycode == 0x1B) { // Esc
                line_pos = 0;
                line_buffer[0] = '\0';
                sequential_mode = false;
                last = keycode;
                Display_ClearCommandHistory();
                Display_Event_ShowHome(); // Default to home
                continue;
            } 
            if (keycode == 0x08 || keycode == 0x7F) { // Backspace/Delete
                if (line_pos > 0) {
                    line_pos--;
                    line_buffer[line_pos] = '\0';
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100))) {
                        strcpy(cmd_buffer.input, line_buffer);
                        cmd_buffer.state = CMD_STATE_TYPING;
                        xSemaphoreGive(cmd_buffer.mutex);
                    }
                }
                last = keycode;
                continue;
            }
            if (keycode == 0x0D) { // Enter
                line_buffer[line_pos] = '\0';
                if (line_pos != 0){
                    // Update history BEFORE processing command
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100))) {
                        // Is history full?
                        if (cmd_buffer.history_count >= CMD_HISTORY_LINES) {
                            // Shift history up
                            for (int i = 0; i < CMD_HISTORY_LINES - 1; i++) {
                                strcpy(cmd_buffer.history[i], cmd_buffer.history[i + 1]);
                            }
                            cmd_buffer.history_count = CMD_HISTORY_LINES - 1;
                        }
                        strcpy(cmd_buffer.history[cmd_buffer.history_count], line_buffer);
                        cmd_buffer.history_count++;
                        
                        strcpy(cmd_buffer.input, line_buffer);
                        xSemaphoreGive(cmd_buffer.mutex);
                    }

                    Command_Handle(); // Takes mutex internally

                    // Add output to history after command completes
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100))) {
                        if (cmd_buffer.output[0] != '\0') {
                            if (cmd_buffer.history_count >= CMD_HISTORY_LINES) {
                                for (int i = 0; i < CMD_HISTORY_LINES - 1; i++) {
                                    strcpy(cmd_buffer.history[i], cmd_buffer.history[i + 1]);
                                }
                                cmd_buffer.history_count = CMD_HISTORY_LINES - 1;
                            }
                            strcpy(cmd_buffer.history[cmd_buffer.history_count], cmd_buffer.output);
                            cmd_buffer.history_count++;
                        }
                        xSemaphoreGive(cmd_buffer.mutex);
                    }
                }
                
                // Reset line buffer
                line_pos = 0;
                line_buffer[0] = '\0';
                if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100))) {
                    strcpy(cmd_buffer.input, line_buffer);
                    cmd_buffer.state = CMD_STATE_TYPING;
                    xSemaphoreGive(cmd_buffer.mutex);
                }
                last = keycode;
                continue;
            }

            // Adds key char to queue
            if (keycode >= 0x20 && keycode <= 0x7E) { // Printable ASCII
                if (line_pos < CMD_BUFFER_SIZE - 1) {
                    line_buffer[line_pos++] = (char)keycode;
                    line_buffer[line_pos] = '\0';
                    
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100))) {
                        strcpy(cmd_buffer.input, line_buffer);
                        cmd_buffer.state = CMD_STATE_TYPING;
                        xSemaphoreGive(cmd_buffer.mutex);
                    }

                } else {
                    printf("Command buffer full!\r\n");
                }
            }
        }

        last = keycode;
        DEV_Delay_ms(POLL_MS);
    }
}

static bool Keyboard_IsConnected(void) {
    if (!ikey_i2c || !key_mutex) return false;
    bool connected = false;
    if (xSemaphoreTake(key_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ikey_i2c->beginTransmission(ikey_addr);
        if (ikey_i2c->endTransmission() == 0) {
            connected = true;
        }
        xSemaphoreGive(key_mutex);
    }
    return connected;
}

static void Keyboard_Start(void){
    if (key_task) return;
    if (Keyboard_IsConnected()) {
        key_task_run = true;
        // Core 0, Priority 1
        xTaskCreatePinnedToCore(keyTask, "key", 4096, NULL, 1, &key_task, 0);
    } else {
        printf("Keyboard not connected!\r\n");
    }
}

bool Keyboard_Init(TwoWire *i2cInstance, uint8_t i2cAddress){
    if (!i2cInstance) return false;
    ikey_i2c = i2cInstance;
    ikey_addr = i2cAddress;
    if (!key_mutex) key_mutex = xSemaphoreCreateMutex();
    if (!key_queue) key_queue = xQueueCreate(256, sizeof(char));
    Keyboard_Start();
    return true;
}

static void Keyboard_Stop(void) {
    key_task_run = false;
    if (key_task) {
        vTaskDelete(key_task);
        key_task = NULL;
    }
    if (key_queue) vQueueDelete(key_queue); key_queue = NULL;
    if (key_mutex) vSemaphoreDelete(key_mutex); key_mutex = NULL;
}

bool Keyboard_GetChar(char *out, TickType_t wait) {
    if (!out || !key_queue) return false;
    return xQueueReceive(key_queue, out, wait) == pdTRUE;
}

