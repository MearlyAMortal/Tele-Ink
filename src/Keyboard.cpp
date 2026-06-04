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

#define KEY_SCAN_BASE_POLL_MS 80
#define KEY_SCAN_BURST_POLL_MS 20
#define KEY_SCAN_BURST_WINDOW_MS 10000
#define KEY_RECONNECT_POLL_MS 500
#define KEY_QUEUE_LEN 32
#define KEY_SCAN_DRAIN_READS 3

#define KEY_I2C_RETRY_COUNT 2
#define KEY_I2C_RETRY_DELAY_US 500

static TwoWire *ikey_i2c = nullptr;
static uint8_t ikey_addr = 0x5F;
static TaskHandle_t key_task = NULL;
static TaskHandle_t key_scan_task = NULL;
static SemaphoreHandle_t i2c_mutex = NULL;
static QueueHandle_t key_queue = NULL;
static volatile bool key_connected = false;
static int history_peek_idx = -1;

// Maps special keycodes to events
static void handle_special_key(uint8_t &kc) {
    switch (kc) {
        case 0x9F: { // H Homescreen
            Display_Event_ShowHome();
            return; 
        }
        case 0x94: { // I Idle
            Display_Event_ShowIdle();
            return;
        }
        case 0xA8: { // C Command
            Display_Event_ShowCommand();
            return;
        }
        case 0x9C: { // D Dynamic window
            Display_Event_ShowDynamicWindow();
            return;
        }
        case 0x80: { // ESC Wake/Sleep toggle
            if (screen_on) {
                Display_Event_Sleep();
            } else {
                Display_Event_Wake();
            }
            return;
        }
        case 0x96: { // P Activate repaint
            Display_Event_Wake();
            return;
        }
        default:
            printf("Keycode: 0x%02X unknown.\r\n", kc);
            return;
    }
}

// Reads a single byte from the keyboard over I2C with retry logic, returns false if no key or error
static bool i2c_read_key(uint8_t &out) {
    if (!ikey_i2c || !i2c_mutex) return false;
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int attempt = 0; attempt < KEY_I2C_RETRY_COUNT; attempt++) {
            ikey_i2c->beginTransmission(ikey_addr);
            ikey_i2c->write(0x00);
            uint8_t tx = ikey_i2c->endTransmission(false);
            
            // Delay after endTransmission to let kb prepare data
            delayMicroseconds(KEY_I2C_RETRY_DELAY_US);
            
            uint8_t req = ikey_i2c->requestFrom((int)ikey_addr, 1);
            if ((tx == 0) && (req == 1)) {
                out = ikey_i2c->read();
                xSemaphoreGive(i2c_mutex);
                if (out != 0x00) {
                    return true;
                }
                return false;
            }
            if (attempt < KEY_I2C_RETRY_COUNT - 1) {
                delayMicroseconds(KEY_I2C_RETRY_DELAY_US * 2);
            }
        }
        xSemaphoreGive(i2c_mutex);
    }
    return false;
}

// Returns true if keyboard is detected on I2C bus, false if not or error
static bool Keyboard_IsConnected(void) {
    if (!ikey_i2c || !i2c_mutex) return false;
    bool connected = false;
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ikey_i2c->beginTransmission(ikey_addr);
        if (ikey_i2c->endTransmission() == 0) {
            connected = true;
            printf("Keyboard connected on I2C: 0x%02X\r\n", ikey_addr);
        }
        xSemaphoreGive(i2c_mutex);
    }
    return connected;
}

// No debounce needed, just read and handle keypresses from I2C 0x5F
static void keyScanTask(void *pv) {
    (void)pv;
    uint8_t keycode = 0;

    TickType_t last_wake = xTaskGetTickCount();
    TickType_t burst_until_tick = 0;

    for (;;) {
        if (!key_connected) {
            key_connected = Keyboard_IsConnected();
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(KEY_RECONNECT_POLL_MS));
            continue;
        }

        keycode = 0;
        if (i2c_read_key(keycode)) {
            burst_until_tick = xTaskGetTickCount() + pdMS_TO_TICKS(KEY_SCAN_BURST_WINDOW_MS);

            (void)xQueueSend(key_queue, &keycode, 0);

            // Drain a few immediate reads to catch clustered key events
            for (int i = 0; i < KEY_SCAN_DRAIN_READS; i++) {
                uint8_t next_key = 0;
                if (!i2c_read_key(next_key)) {
                    break;
                }
                (void)xQueueSend(key_queue, &next_key, 0);
            }
        }

        TickType_t poll_ticks = pdMS_TO_TICKS(KEY_SCAN_BASE_POLL_MS);
        if (xTaskGetTickCount() < burst_until_tick) {
            poll_ticks = pdMS_TO_TICKS(KEY_SCAN_BURST_POLL_MS);
        }
        vTaskDelayUntil(&last_wake, poll_ticks);
    }
}

// Consume keycodes from scanner queue and apply display/command behavior.
static void keyTask(void *pv) {
    (void)pv;
    uint8_t keycode = 0;
    bool sequential_mode = false;
    static char line_buffer[CMD_BUFFER_SIZE];
    static size_t line_pos = 0;

    for (;;) {
        if (!key_queue) {
            DEV_Delay_ms(KEY_RECONNECT_POLL_MS);
            continue;
        }

        // Wait for next key event from scan task.
        if (xQueueReceive(key_queue, &keycode, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        // Key press detected!
        SetLastActivityTick(); // Reset idle timer for display on any key press

        // Handle special key if mapped (exit sequential and return to base handling)
        if (keycode >= 0x80 && keycode <= 0xAF) {
            handle_special_key(keycode);
            continue;
        }

        // Handle normal keys in sequential mode (Must take mutex to update cmd_buffer)
        // Build local line buffer and update cmd_buffer input for command processing
        sequential_mode = (current_page == PAGE_COMMAND);

        if (sequential_mode) { 
            // Esc (replicates /exit) goes back one mode state if in a submode, otherwise goes back to base
            if (keycode == 0x1B) {
                if (!at_mode && !sms_read && !sms_send && !gnss_mode && !http_mode ) {
                    continue;
                }
                
                at_mode = false;
                gnss_mode = false;
                http_mode = false;
                // If exiting from response return to sms_read mode with original state
                if (sms_send && sms_read) {
                    sms_send = false;
                } else {
                    sms_send = false;
                    sms_read = false;
                    sms_read_all = false;
                    sms_count = 0;
                    memset(sms_ids, -1, sizeof(sms_ids));
                }
                continue;
            } 
            // Backspace/Delete
            if (keycode == 0x08 || keycode == 0x7F) {
                if (line_pos > 0) {
                    line_pos--;
                    line_buffer[line_pos] = '\0';
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        strcpy(cmd_buffer.input, line_buffer);
                        cmd_buffer.state = (line_pos > 0) ? CMD_STATE_TYPING : CMD_STATE_IDLE;
                        xSemaphoreGive(cmd_buffer.mutex);
                    }
                }
                continue;
            }
            // Enter key sends command for processing
            if (keycode == 0x0D) {
                history_peek_idx = -1;
                line_buffer[line_pos] = '\0';
                printf("Command entered: %s\r\n", line_buffer);
                if (line_pos != 0){
                    // Update history BEFORE processing command
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        if (cmd_buffer.history_count >= CMD_HISTORY_LINES) {
                            for (int i = 0; i < CMD_HISTORY_LINES - 1; i++) {
                                //strcpy(cmd_buffer.history[i], cmd_buffer.history[i + 1]);
                                memmove(cmd_buffer.history[i], cmd_buffer.history[i + 1], CMD_BUFFER_SIZE);
                            }
                            cmd_buffer.history_count = CMD_HISTORY_LINES - 1;
                        }
                        strcpy(cmd_buffer.history[cmd_buffer.history_count], line_buffer);
                        cmd_buffer.history_count++;

                        // input recall history for navigation using arrow keys
                        if (cmd_buffer.input_history_count >= CMD_INPUT_HISTORY_LINES) {
                            for (int i = 0; i < CMD_INPUT_HISTORY_LINES - 1; i++) {
                                //strcpy(cmd_buffer.input_history[i], cmd_buffer.input_history[i + 1]);
                                memmove(cmd_buffer.input_history[i], cmd_buffer.input_history[i + 1], CMD_BUFFER_SIZE);
                            }
                            cmd_buffer.input_history_count = CMD_INPUT_HISTORY_LINES - 1;
                        }
                        strcpy(cmd_buffer.input_history[cmd_buffer.input_history_count], line_buffer);
                        cmd_buffer.input_history_count++;
                        
                        // Place line_buffer into the command_buffer.input so command processor can handle
                        strcpy(cmd_buffer.input, line_buffer);
                        xSemaphoreGive(cmd_buffer.mutex);
                    } else {
                        printf("Error: Cant take CMD mutex to update history and input\r\n");
                    }

                    // Takes cmd_buffer mutex internally ;)
                    // Sets CMD_STATE to processing and done internally once finished
                    Command_Handle();


                    // Add output to history after command completes
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        // If the command is in done state and there is output
                        if (cmd_buffer.state == CMD_STATE_DONE && cmd_buffer.output[0] != '\0') {
                            if (cmd_buffer.history_count >= CMD_HISTORY_LINES) {
                                for (int i = 0; i < CMD_HISTORY_LINES - 1; i++) {
                                    strcpy(cmd_buffer.history[i], cmd_buffer.history[i + 1]);
                                }
                                cmd_buffer.history_count = CMD_HISTORY_LINES - 1;
                            }
                            strcpy(cmd_buffer.history[cmd_buffer.history_count], cmd_buffer.output);
                            cmd_buffer.history_count++;
                        } else {
                            printf("No output or command not done, not adding to history\r\n");
                        }
                        xSemaphoreGive(cmd_buffer.mutex);
                    }  else {
                        printf("Error: Cant take CMD mutex to update history and input\r\n");
                    }
                }
                
                // Reset line buffer
                line_pos = 0;
                line_buffer[0] = '\0';
                if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    strcpy(cmd_buffer.input, line_buffer);
                    cmd_buffer.state = CMD_STATE_IDLE;
                    xSemaphoreGive(cmd_buffer.mutex);
                } else {
                    printf("Error: Cant take CMD mutex to update history and input\r\n");
                }
                continue;
            }

            // Arrow keys(only up and down for now)
            if (0xB5 == keycode || keycode == 0xB6) {
                if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (keycode == 0xB5) { // up
                        if (history_peek_idx < cmd_buffer.input_history_count - 1) {
                            history_peek_idx++;
                            int idx = cmd_buffer.input_history_count - 1 - history_peek_idx;
                            strcpy(line_buffer, cmd_buffer.input_history[idx]);
                            line_pos = strlen(line_buffer);
                            strcpy(cmd_buffer.input, line_buffer);
                            cmd_buffer.state = (line_pos > 0) ? CMD_STATE_TYPING : CMD_STATE_IDLE;
                        }
                    } else { // down
                        if (history_peek_idx > 0) {
                            history_peek_idx--;
                            int idx = cmd_buffer.input_history_count - 1 - history_peek_idx;
                            strcpy(line_buffer, cmd_buffer.input_history[idx]);
                            line_pos = strlen(line_buffer);
                            strcpy(cmd_buffer.input, line_buffer);
                            cmd_buffer.state = (line_pos > 0) ? CMD_STATE_TYPING : CMD_STATE_IDLE;
                        } else if (history_peek_idx == 0) {
                            history_peek_idx = -1;
                            line_buffer[0] = '\0';
                            line_pos = 0;
                            strcpy(cmd_buffer.input, line_buffer);
                            cmd_buffer.state = CMD_STATE_IDLE;
                        }
                    }
                    xSemaphoreGive(cmd_buffer.mutex);
                } else {
                    printf("Error: Cant take CMD mutex to navigate history\r\n");
                }
                continue;
            }

            // Adds pressed key to line buffer and updates cmd_buffer input for display
            if (keycode >= 0x20 && keycode <= 0x7E) { // Printable ASCII
                history_peek_idx = -1;
                if (line_pos < CMD_BUFFER_SIZE - 1) {
                    line_buffer[line_pos++] = (char)keycode;
                    line_buffer[line_pos] = '\0';
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        strcpy(cmd_buffer.input, line_buffer);
                        xSemaphoreGive(cmd_buffer.mutex);
                    } else {
                        printf("Error: Cant take CMD mutex to update input\r\n");
                    }
                } else {
                    printf("Command buffer full!\r\n");
                }
            }

            // Update cmd_buffer state based on current cmd_buffer.input if we are typing
            if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                cmd_buffer.state = (line_pos > 0) ? CMD_STATE_TYPING : CMD_STATE_IDLE;
                xSemaphoreGive(cmd_buffer.mutex);
            } else {
                printf("Error: Cant take CMD mutex to update state\r\n");
            }
        } else if (current_page == PAGE_DYNAMIC_WINDOW) {
            // If in dynamic window mode this gives control to user if programmed to interact with anything that is displayed
        }
    }
}

// Start keyboard scanner and consumer tasks if they are not running.
static void Keyboard_StartTasks(void){
    if (!key_scan_task) {
        xTaskCreatePinnedToCore(keyScanTask, "key_scan", 4096, NULL, 4, &key_scan_task, 0);
    }
    if (!key_task) {
        xTaskCreatePinnedToCore(keyTask, "key", 4096, NULL, 3, &key_task, 0);
    }
}

// Initilize relavent DS and check if connected before starting task, returns true if task started successfully, false if not connected or error
bool Keyboard_Init(TwoWire *i2cInstance, uint8_t i2cAddress){
    if (!i2cInstance) return false;
    ikey_i2c = i2cInstance;
    ikey_addr = i2cAddress;
    if (!i2c_mutex) i2c_mutex = xSemaphoreCreateMutex();
    if (!key_queue) key_queue = xQueueCreate(KEY_QUEUE_LEN, sizeof(uint8_t));

    if (!i2c_mutex || !key_queue) {
        printf("Error: Keyboard init failed (mutex/queue).\r\n");
        return false;
    }

    if (Keyboard_IsConnected()) {
        key_connected = true;
    } else {
        printf("Error: Keyboard could not connect on init! Starting task anyway.\r\n");
    }

    Keyboard_StartTasks();
    return true;
}




