#include "Keyboard.h"
#include <Wire.h>
#include "Display.h"
#include "Modem.h"
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


#define REG_KEY 0x00
#define POLL_MS 100
#define DEBOUNCE_READS 3

// FD
//static void Keyboard_SequentialMode(void);
static void handle_special_key(uint8_t &kc);
static bool i2c_read_key(uint8_t &out);
static void keyTask(void *pv);
static void Keyboard_Start(void);
static void Keyboard_Stop(void);

// Maps special keycodes to events
static void handle_special_key(uint8_t &kc) {
    printf("Handling special keycode: 0x%02X, ", kc);
    switch (kc) {
        case 0x9F: { // Homescreen
            Display_Event_ShowHome();
            break; 
        }
        case 0x94: { // Idle
            Display_Event_ShowIdle();
            break;
        }
        case 0xA8: { // Command
            //Display_Event_ShowCommand();
            printf("Not showing command.\r\n");
            break;
        }
        default:
            printf("Keycode: 0x%02X unknown.\r\n", kc);
            break;
    }
}

static bool i2c_read_key(uint8_t &out) {
    if (!ikey_i2c) return false;
    if (key_mutex) {
        if (xSemaphoreTake(key_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            return false;
        }
    }
    // Prepare kb
    ikey_i2c->beginTransmission(ikey_addr);
    // que one byte into tx buff from register ptr
    ikey_i2c->write(REG_KEY);
    // End write phase but dont stop and take tx wire error code
    uint8_t tx = ikey_i2c->endTransmission(false);
    uint8_t req = ikey_i2c->requestFrom((int)ikey_addr, 1);
    bool ok = (tx == 0) && (req == 1);
    if (ok) out = ikey_i2c->read();

    if (ok && out != 0x00){
        printf("I2C RX: addr=0x%02X tx=%d req=%d val=0x%02X\r\n", ikey_addr, tx, req, out);
    }
    if (key_mutex) xSemaphoreGive(key_mutex);
    return ok;
}

// No debounce, just read and handle special keys
static void keyTask(void *pv) {
    (void)pv;
    uint8_t last = 0;
    uint8_t stable = 0;
    uint8_t reads = 0;

    uint32_t timeout_count = 0;
    bool timeout = false;

    bool sequential_mode = false;
    uint8_t keycode = 0;
    char key_char = '\n';


    for (;;) {
        if (false){ // debug
            printf("keyTask alive loop\r\n");
            DEV_Delay_ms(5000);
            continue;
        }
        if (!key_task_run || !screen_on) { 
            printf("keyTask waiting: key_task_run=%d screen_on=%d\r\n", key_task_run, screen_on);
            DEV_Delay_ms(1000); 
            continue; 
        }

        keycode = 0;

        // Read Keyboard data
        if (!i2c_read_key(keycode) ) {
            DEV_Delay_ms(POLL_MS);
            continue;
        }
        // Idling detection
        if (keycode == 0x00){
            //printf("Idle\r\n");
            if (screen_on && !timeout && !idle_page && ++timeout_count >= 120) {
                printf("Sleep screen unless on idle_page\r\n");
                DisplayEvent e = { .type = DISP_EVT_SLEEP, .payload = NULL};
                Display_PostEvent(&e, 0);
                timeout = true;
            }

            DEV_Delay_ms(500);
            continue;
        }
        // Key pressed
        timeout_count = 0;
        timeout = false;

        // Check if FN special key (exit sequential and return to base handling)
        if (keycode != last && keycode >= 0x80 && keycode <= 0xAF) {
            last = keycode;
            handle_special_key(keycode);
            if (key_queue) xQueueReset(key_queue);
            sequential_mode = false;
            continue; // skip adding to queue
        }

       
        // Read multiple keys into queue for sequences
        // read keys until exit or enter then send to command processor wait for response
        // then sends a fast event to display task to show the current buffer (no delay)
        // then send another event to show command response
        sequential_mode = command_page;

        if (sequential_mode) { 
            if (keycode == 0x1B) { // Esc
                if (key_queue) xQueueReset(key_queue);
                sequential_mode = false;
                printf("Quit Command Mode\r\n");
                continue;
            } 
            if (keycode == 0x0D) { // Enter
                //printf("End of sequence: %s\r\n", command_send);
                command_send = NULL; // Clear command send buffer
                key_char = '\n';
                if (key_queue && (xQueueSend(key_queue, &key_char, pdMS_TO_TICKS(100))) == pdTRUE) {
                    printf("Command sequence ended with ENTER\r\n");
                }
                // Send to command processor
                //command_resp = Command_ProcessCommandBuffer();
                // Show command response on screen
                // ScreenTask should print data when it sees new command_resp
                continue;
            }

            // Adds key char to queue
            key_char = (char)keycode;
            printf("Key about to be queued: %c\r\n", key_char);
            if (key_queue && (xQueueSend(key_queue, &key_char, pdMS_TO_TICKS(100))) == pdTRUE) {
                printf("Keycode 0x%02X queued\r\n", keycode);
            } else {
                printf("Keycode 0x%02X NOT queued\r\n", keycode);
            } 
        }

        last = keycode;
        DEV_Delay_ms(POLL_MS);
    }
}

static void Keyboard_Start(void){
    if (key_task) return;
    key_task_run = true;
    // Core 0, Priority 1
    xTaskCreatePinnedToCore(keyTask, "key", 4096, NULL, 1, &key_task, 0);
}

bool Keyboard_Init(TwoWire *i2cInstance, uint8_t i2cAddress){
    printf("About to init keyboard\r\n");
    if (!i2cInstance) return false;
    ikey_i2c = i2cInstance;
    ikey_addr = i2cAddress ? i2cAddress : 0x5F; // M5Stack keyboard default

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
    if (key_queue) { vQueueDelete(key_queue); key_queue = NULL; }
    if (key_mutex) { vSemaphoreDelete(key_mutex); key_mutex = NULL; }
}

bool Keyboard_GetChar(char *out, TickType_t wait) {
    if (!out || !key_queue) return false;
    return xQueueReceive(key_queue, out, wait) == pdTRUE;
}

