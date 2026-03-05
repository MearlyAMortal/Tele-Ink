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

#define POLL_MS 100

static TwoWire *ikey_i2c = nullptr;
static uint8_t ikey_addr = 0x5F;
static TaskHandle_t key_task = NULL;
static SemaphoreHandle_t key_mutex = NULL;
static volatile bool key_task_run = false;
static int history_peek_idx = -1;

// Maps special keycodes to events
static bool handle_special_key(uint8_t &kc) {
    switch (kc) {
        case 0x9F: { // H Homescreen
            Display_Event_ShowHome();
            return true; 
        }
        case 0x94: { // I Idle
            Display_Event_ShowIdle();
            return true;
        }
        case 0xA8: { // C Command
            Display_Event_ShowCommand();
            return true;
        }
        case 0x80: { // ESC Wake/Sleep toggle
            if (screen_on) {
                Display_Event_Sleep();
            } else {
                Display_Event_Wake();
            }
            return true;
        }
        case 0x96: { // P Activate repaint
            Display_Event_Wake();
            return true;
        }
        default:
            printf("Keycode: 0x%02X unknown.\r\n", kc);
            return false;
    }
    return false;
}

// Reads a single byte from the keyboard over I2C, returns false if no key or error
// Cast the return as an int in case I2C returns negative for whatever reason
static bool i2c_read_key(uint8_t &out) {
    if (!ikey_i2c || !key_mutex) return false;
    if (xSemaphoreTake(key_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
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
    // Buffer for sequential mode building strings to send to command processor
    bool sequential_mode = false;
    static char line_buffer[CMD_BUFFER_SIZE];
    static size_t line_pos = 0;

    for (;;) {
        if (!key_task_run) {
            DEV_Delay_ms(POLL_MS*10);
            continue;
        }

        // Read Keyboard data, false if 0x00 (no key) IDLE
        keycode = 0;
        if (!i2c_read_key(keycode) ) {
            DEV_Delay_ms(POLL_MS);
            continue;
        }

        // Key pressed ATP
        SetLastActivityTick(); // Reset idle timer for display on any key press

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
            // Esc (replicates /exit)
            if (keycode == 0x1B) {
                if (!at_mode && !gnss_mode && !sms_read && !sms_send) {
                    last = keycode;
                    continue;
                }
                line_pos = 0;
                line_buffer[0] = '\0';
                at_mode = false;
                gnss_mode = false;
                // responding derived from reading so go to read mode
                if (sms_send && sms_read) {
                    sms_send = false;
                } else {
                    sms_send = false;
                    sms_read = false;
                }
                if (sms_read_all){
                    sms_count = 0;
                    sms_read_all = false;
                }
                // End all wifi avenues
                wifi_mode = false;
                if (wifi_scan) {
                    // Stop scan
                    wifi_scan = false;
                }
                if (wifi_connected) {
                    // Disconnect
                    wifi_connected = false;
                }
                last = keycode;
                continue;
            } 
            // Backspace/Delete
            if (keycode == 0x08 || keycode == 0x7F) {
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
            // Enter key sends command for processing
            if (keycode == 0x0D) {
                history_peek_idx = -1;
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

                    Command_Handle(); // Takes cmd_buffer mutex internally ;)

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

            // Arrow keys(only up and down for now)
            if (0xB5 == keycode || keycode == 0xB6){
                if (keycode == 0xB5) { //up
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100))) {
                        if (history_peek_idx < cmd_buffer.history_count - 1) {
                            history_peek_idx++;
                            int idx = cmd_buffer.history_count - 1 - history_peek_idx;
                            strcpy(line_buffer, cmd_buffer.history[idx]);
                            line_pos = strlen(line_buffer);
                            strcpy(cmd_buffer.input, line_buffer);
                            cmd_buffer.state = CMD_STATE_TYPING;
                        }
                        xSemaphoreGive(cmd_buffer.mutex);
                    }
                } else if (keycode == 0xB6) { //down
                    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100))) {
                        if (history_peek_idx > 0) {
                            history_peek_idx--;
                            int idx = cmd_buffer.history_count - 1 - history_peek_idx;
                            strcpy(line_buffer, cmd_buffer.history[idx]);
                            line_pos = strlen(line_buffer);
                            strcpy(cmd_buffer.input, line_buffer);
                        } else if (history_peek_idx == 0) {
                            history_peek_idx = -1;
                            line_buffer[0] = '\0';
                            line_pos = 0;
                            strcpy(cmd_buffer.input, line_buffer);
                        }
                        cmd_buffer.state = CMD_STATE_TYPING;
                        xSemaphoreGive(cmd_buffer.mutex);
                    }
                }
                last = keycode;
                continue;
            }

            // Adds pressed key to line buffer and updates cmd_buffer input for display
            if (keycode >= 0x20 && keycode <= 0x7E) { // Printable ASCII
                history_peek_idx = -1;
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
        // Update last keycode for debugging and polling delay to avoid spamming I2C
        last = keycode;
        DEV_Delay_ms(POLL_MS);
    }
}

// Returns true if keyboard is detected on I2C bus, false if not or error
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

// Start keyboard task with very high priority if key_task isnt running
static void Keyboard_StartTask(void){
    if (!key_task) {
        key_task_run = true;
        // Core 0, Priority 1
        xTaskCreatePinnedToCore(keyTask, "key", 4096, NULL, 1, &key_task, 0);
    }
}

// Initilize relavent DS and check if connected before starting task, returns true if task started successfully, false if not connected or error
bool Keyboard_Init(TwoWire *i2cInstance, uint8_t i2cAddress){
    if (!i2cInstance) return false;
    ikey_i2c = i2cInstance;
    ikey_addr = i2cAddress;
    if (!key_mutex) key_mutex = xSemaphoreCreateMutex();

    if (!Keyboard_IsConnected()) {
        printf("Keyboard not detected at I2C address 0x%02X.\r\n", ikey_addr);
        return false;
    }

    Keyboard_StartTask();
    return true;
}




