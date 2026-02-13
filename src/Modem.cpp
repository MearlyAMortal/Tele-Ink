#include "Modem.h"
#include "Arduino.h"
#include <string.h>
#include <stdlib.h>
#include "Display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Pins
static HardwareSerial *modemSerial = nullptr;
static uint8_t modemPowerPin = -1;
static uint8_t modem_rx_pin = -1;
static uint8_t modem_tx_pin = -1;

// Modem state
static SemaphoreHandle_t modem_mutex = NULL;
static TaskHandle_t modem_task_handle = NULL;
static QueueHandle_t modem_cmd_queue = NULL;
static ModemCmd *current_cmd = NULL;

// new response sync objects
//static SemaphoreHandle_t modem_resp_sem = NULL;

//static bool modem_ready = false;
static bool modem_serial_begun = false;
static bool modem_idle = false;


// FD
static void Modem_StartTask(void);
static bool Modem_WriteRaw(const uint8_t *data, size_t len, uint32_t timeout_ms);
static bool Modem_WaitOnlyOK(uint32_t timeout_ms);
static bool Modem_HandleURC(const char *line);
static void modemTask(void *pv);


void Modem_TogglePWK(uint32_t duration_ms) {
    if (modemPowerPin == -1) {
        printf("Modem: No power pin provided.\r\n");
        return;
    }
    printf("Modem: Toggle power for %lu ms.\r\n", duration_ms);
    digitalWrite(modemPowerPin, HIGH);
    DEV_Delay_ms(duration_ms);
    digitalWrite(modemPowerPin, LOW);
}

void Modem_Restart(void) {
    printf("Restarting modem...\r\n");
    modem_ready = false;
    Modem_TogglePWK(2200);
    DEV_Delay_ms(25000);
    Modem_TogglePWK(1100);
    DEV_Delay_ms(20000);
    printf("Modem ready to begin serial.\r\n");
    modem_ready = true;
}

static bool Modem_WriteRaw(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!modemSerial || !modem_ready) return false;
    if (!data || len == 0) return true;

    if (modem_mutex) {
        if (xSemaphoreTake(modem_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    } 
    size_t written = modemSerial->write(data, len);
    modemSerial->flush();
    if (modem_mutex) xSemaphoreGive(modem_mutex);
    return written == len;
}

/*
static bool Modem_WaitOnlyOK(uint32_t timeout_ms) {
    ModemCmd *w = (ModemCmd*)malloc(sizeof(ModemCmd));
    if (!w) return false;
    memset(w, 0, sizeof(*w));

    w->cmd[0] = '\0';
    w->resp[0] = '\0';
    w->timeout_ms = timeout_ms;
    w->waitForOK = true;
    w->noTx = true;
    w->done_sem = xSemaphoreCreateBinary();
    if (!w->done_sem) { free(w); return false; }

    if (xQueueSend(modem_cmd_queue, &w, pdMS_TO_TICKS(2000)) != pdTRUE) {
        vSemaphoreDelete(w->done_sem);
        free(w);
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(w->done_sem, pdMS_TO_TICKS(timeout_ms + 500)) == pdTRUE) {
        ok = (strstr(w->resp, "OK") != NULL) 
        && (strstr(w->resp, "ERROR") == NULL) 
        && (strstr(w->resp, "+CME ERROR") == NULL) 
        &&(strstr(w->resp, "+CMS ERROR") == NULL);
    }

    vSemaphoreDelete(w->done_sem);
    free(w);
    return ok;
}
*/
// Queue a wait-only command (returns the handle)
static ModemCmd* Modem_QueueWaitOnly(uint32_t timeout_ms) {
    ModemCmd *w = (ModemCmd*)malloc(sizeof(ModemCmd));
    if (!w) return NULL;
    memset(w, 0, sizeof(*w));
    w->cmd[0] = '\0';
    w->resp[0] = '\0';
    w->timeout_ms = timeout_ms;
    w->waitForOK = true;
    w->noTx = true;
    w->done_sem = xSemaphoreCreateBinary();
    if (!w->done_sem) { free(w); return NULL; }
    if (xQueueSend(modem_cmd_queue, &w, pdMS_TO_TICKS(500)) != pdTRUE) {
        vSemaphoreDelete(w->done_sem);
        free(w);
        return NULL;
    }
    return w;
}

// Wait for the queued command to finish and return result
static bool Modem_WaitAndFree(ModemCmd *w, uint32_t timeout_ms) {
    if (!w) return false;
    bool ok = false;
    if (xSemaphoreTake(w->done_sem, pdMS_TO_TICKS(timeout_ms + 500)) == pdTRUE) {
        ok = (strstr(w->resp, "OK") != NULL) &&
             (strstr(w->resp, "ERROR") == NULL) &&
             (strstr(w->resp, "+CME ERROR") == NULL) &&
             (strstr(w->resp, "+CMS ERROR") == NULL);
    }
    vSemaphoreDelete(w->done_sem);
    free(w);
    return ok;
}


bool Modem_SendSMS(const char *number, const char *message, uint32_t timeout_ms) {
    if (!number || !number[0] || !message) return false;
    //printf("SMS: starting to %s\r\n", number);
    char tmp[512] = {0};
    /*
    if (!Modem_SendAT("AT+CMGF=1", tmp, sizeof(tmp), timeout_ms)) {
        printf("SMS: CMGF failed\r\n");
        return false;
    }
        */
    //printf("SMS: CMGF ok\r\n");
    char cmgs[96];
    snprintf(cmgs, sizeof(cmgs), "AT+CMGS=\"%s\"", number);
    Modem_SendAT(cmgs, tmp, sizeof(tmp), timeout_ms);
    //printf("SMS: CMGS resp: %s\r\n", tmp);
    if (strchr(tmp, '>') == NULL) {
        printf("SMS: no > prompt\r\n");
        return false;
    }
    //printf("SMS: got > prompt\r\n");

    ModemCmd *w = Modem_QueueWaitOnly(timeout_ms);
    if (!w) return false;

    DEV_Delay_ms(100);

    if (!Modem_WriteRaw((uint8_t*)message, strlen(message), timeout_ms)) {
        printf("SMS: write body failed\r\n");
        return false;
    }
    //printf("SMS: wrote body\r\n");

    const uint8_t ctrlz = 0x1A;
    if (!Modem_WriteRaw(&ctrlz, 1, timeout_ms)) {
        printf("SMS: write ctrlz failed\r\n");
        return false;
    }
    //printf("SMS: wrote ctrlz, waiting for OK\r\n");
    bool ok = Modem_WaitAndFree(w, timeout_ms);
    //printf("SMS: final result = %s\r\n", ok ? "OK" : "FAIL");
    return ok;
}


// Sends AT command to modem and waits for response. (safe)
bool Modem_SendAT(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms) {
    if (!modemSerial || !cmd || !resp || resp_len == 0) return false;
    ModemCmd *r = (ModemCmd*)malloc(sizeof(ModemCmd)); // Heap
    if (!r) return false;
    //printf("Modem_SendAT malloc OK\r\n");
    strncpy(r->cmd, cmd, sizeof(r->cmd)-1); 
    r->cmd[sizeof(r->cmd)-1]=0;
    r->done_sem = xSemaphoreCreateBinary();
    r->timeout_ms = timeout_ms;
    r->resp[0] = '\0';
    r->waitForOK = true;
    r->noTx = false;
    r->start_tick = 0;

    // enqueue request (wait briefly)
    if (modem_cmd_queue == NULL || xQueueSend(modem_cmd_queue, &r, pdMS_TO_TICKS(2000)) != pdTRUE) {
        vSemaphoreDelete(r->done_sem);
        free(r);
        return false;
    }

    // wait for response (task will give the semaphore)
    bool ok = false;
    if (xSemaphoreTake(r->done_sem, pdMS_TO_TICKS(timeout_ms + 500)) == pdTRUE) {
        // copy to caller buffer
        strncpy(resp, r->resp, resp_len-1);
        resp[resp_len-1] = '\0';
        //ok = true; 
         // Check for actual OK (or > for SMS prompt)
        ok = (strstr(resp, "OK") != NULL || strchr(resp, '>') != NULL) &&
             (strstr(resp, "ERROR") == NULL) &&
             (strstr(resp, "+CME ERROR") == NULL) &&
             (strstr(resp, "+CMS ERROR") == NULL);  
    }

    vSemaphoreDelete(r->done_sem);
    free(r);
    //printf("Modem_SendAT free OK\r\n");
    return ok;
}


static bool Modem_HandleURC(const char *line) {
    if (strcmp(line, "+CREG: 0,5") == 0 || strcmp(line, "+CREG: 0,1") == 0) {
        printf("Modem network connected!\r\n");
        DisplayEvent e = { .type = DISP_EVT_MODEM_NET, .payload = NULL};
        Display_PostEvent(&e, 0);
        DEV_Delay_ms(250); // Wait for display task to process net event and update modem_net before we
        return true;
    } 
    else if (strncmp(line, "+CMT:", 5) == 0) {
        printf("SMS recieved!\r\n");
    }
    else {
        printf("Modem URC unhandled: %s\r\n", line);
    }
    return false;
}


// Modem background task to handle command queue and URCs
static void modemTask(void *pv) {
    (void)pv;
    char line[256];
    size_t idx = 0;

    printf("Waiting 20s for modem coldstart\r\n");
    DEV_Delay_ms(20000);
    

    modemSerial->begin(115200, SERIAL_8N1, modem_rx_pin, modem_tx_pin);
    modemSerial->flush();
    DEV_Delay_ms(100);
    modem_serial_begun = true;

    for (int i = 0; i < 3; ++i) {
        modemSerial->print("AT\r\n");
        DEV_Delay_ms(500);

        while (modemSerial->available()) {
            char resp[32];
            size_t len = modemSerial->readBytesUntil('\n', resp, sizeof(resp)-1);
            resp[len] = '\0';
            while (len > 0 && isspace(resp[len - 1])) {
                resp[--len] = '\0';
            }
            if (strcmp(resp, "OK") == 0) {
                printf("Modem is ready!\r\n");
                DisplayEvent e = { .type = DISP_EVT_MODEM_READY, .payload = NULL};
                Display_PostEvent(&e, 0);
                DEV_Delay_ms(250); // Wait for display task to process ready event and update modem_ready before we
            }
            if (modem_ready) break;
        }
        if (modem_ready) break;
        //printf("Modem not responding to AT, retrying...\r\n");
        DEV_Delay_ms(1000);
    }

    char tmp[256];
    Modem_SendAT("AT+CREG?", tmp, sizeof(tmp), 5000);
    
    // Init complete, enter main loop to handle commands and URCs
    for (;;) {
        // if modem not ready, wait and loop
        if (!modemSerial || !modem_ready) {
            DEV_Delay_ms(2500);
            continue;
        }

        // 1) Accept new command if none currently pending
        if (current_cmd == NULL && modem_cmd_queue) {
            ModemCmd *queued = NULL;
            if (xQueueReceive(modem_cmd_queue, &queued, 0) == pdTRUE) {
                printf("Command recived!\r\n");
                current_cmd = queued;
                current_cmd->start_tick = xTaskGetTickCount();
                current_cmd->resp[0] = '\0';
                idx = 0;
                // send the command
                if (!current_cmd->noTx){
                    if (modem_mutex && xSemaphoreTake(modem_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        modemSerial->print(current_cmd->cmd);
                        modemSerial->print("\r\n");
                        modemSerial->flush();
                        xSemaphoreGive(modem_mutex);
                    } else {
                        strncpy(current_cmd->resp, "ERROR: modem_mutex timeout", sizeof(current_cmd->resp) - 1);
                        current_cmd->resp[sizeof(current_cmd->resp) - 1] = '\0';
                        xSemaphoreGive(current_cmd->done_sem);
                        current_cmd = NULL;
                    }
                }
            }
        }

        // 2) Read available bytes and accumulate lines
        while (modemSerial->available()) {
            int c = modemSerial->read();
            //printf("read=%d\r\n", c);
            if (c < 0) break;

            // If we're waiting for OK and we see '>', treat it like "response ready" for AT+CMGS.
            if (current_cmd && current_cmd->waitForOK && c == '>') {
                // Only treat as SMS prompt if the command sms
                if (strncmp(current_cmd->cmd, "AT+CMGS", 7) == 0) {
                    strncat(current_cmd->resp, ">\n", sizeof(current_cmd->resp) - strlen(current_cmd->resp) - 1);
                    xSemaphoreGive(current_cmd->done_sem);
                    current_cmd = NULL;
                    idx = 0;
                    continue;
                }
            }

            if (idx < sizeof(line) - 1) line[idx++] = (char)c;
            
            if (c == '\n') {
                // strip CR/LF
                while (idx > 0 && (line[idx-1] == '\r' || line[idx-1] == '\n')) idx--;
                line[idx] = '\0';

                // debug log
                if (line[0] != '\0') printf("Modem RX: %s\r\n", line);

                //printf("Modem RX: %s\r\n", line);
                if (idx > 0) {
                    if (current_cmd) {
                        // append to response transcript
                        strncat(current_cmd->resp, line, sizeof(current_cmd->resp) - strlen(current_cmd->resp) - 2);
                        strncat(current_cmd->resp, "\n", sizeof(current_cmd->resp) - strlen(current_cmd->resp) - 1);

                        if (current_cmd->waitForOK) {
                            if (strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0 ||
                                strstr(line, "+CME ERROR") || strstr(line, "+CMS ERROR")) {
                                xSemaphoreGive(current_cmd->done_sem);
                                current_cmd = NULL;
                            }
                        } else {
                            xSemaphoreGive(current_cmd->done_sem);
                            current_cmd = NULL;
                        }
                    } else {
                        // unsolicited result code / URC
                        Modem_HandleURC(line);
                    }
                }

                idx = 0;
            }
        }
        // check for command timeout
        if (current_cmd) {
            TickType_t now = xTaskGetTickCount();
            if ((now - current_cmd->start_tick) > pdMS_TO_TICKS(current_cmd->timeout_ms)) {
                strncpy(current_cmd->resp, "TIMEOUT", sizeof(current_cmd->resp)-1);
                current_cmd->resp[sizeof(current_cmd->resp)-1] = '\0';
                xSemaphoreGive(current_cmd->done_sem);
                current_cmd = NULL;
            }
        }

        // Idle
        if (!current_cmd && !modemSerial->available()){
            DEV_Delay_ms(200);
        } else {
            DEV_Delay_ms(10);
        }
    }
}

static void Modem_StartTask(void) {
    // Core 1, Priority 2
    xTaskCreatePinnedToCore(modemTask, "modem", 8192, NULL, 2, &modem_task_handle, 1);
}

// Create Sem/Queue and start modemTask
bool Modem_Init(HardwareSerial *serial, int rxPin, int txPin, int powerPin) {
    modemSerial = serial;
    modemPowerPin = powerPin;
    modem_rx_pin = rxPin;
    modem_tx_pin = txPin;

    if (modemPowerPin != -1) {
        pinMode(modemPowerPin, OUTPUT);
        digitalWrite(modemPowerPin, LOW);
    }

    // Safety
    if (!modem_mutex) modem_mutex = xSemaphoreCreateMutex();
    //if (!modem_resp_sem) modem_resp_sem = xSemaphoreCreateBinary();
    // Queue
    if (!modem_cmd_queue) modem_cmd_queue = xQueueCreate(4, sizeof(ModemCmd*));

    if (modemSerial) Modem_StartTask();
    
    return true;
}
