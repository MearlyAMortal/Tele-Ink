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
static SemaphoreHandle_t modem_resp_sem = NULL;
//static volatile bool modem_expect_response = false;
//static char modem_resp_buf[512];

static bool modem_ready = false;
static bool modem_serial_begun = false;
static bool modem_idle = false;


// FD
static void Modem_StartTask(void);
static bool Modem_WriteRaw(const uint8_t *data, size_t len, uint32_t timeout_ms);
static bool Modem_AT(void);
static void Modem_HandleURC(const char *line);
static bool Modem_GetGNSSRaw(char *resp, size_t resp_len, uint32_t timeout_ms);
static bool Modem_GetGNSSutc(char *resp, size_t resp_en, uint32_t timeout_ms);
static bool Modem_CheckNetwork(void);
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


static bool Modem_WriteRaw(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!modemSerial || !modem_ready) return false;
    // Try to take the mutex so we don't collide with modemTask sending an AT cmd.
    if (modem_mutex) {
        if (xSemaphoreTake(modem_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    }
    size_t written = modemSerial->write(data, len);
    modemSerial->flush();
    if (modem_mutex) xSemaphoreGive(modem_mutex);
    return written == len;
}

// Sends AT command to modem and waits for response. (safe)
bool Modem_SendAT(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms) {
    if (!modemSerial || !cmd || !resp || resp_len == 0) return false;
    ModemCmd *r = (ModemCmd*)malloc(sizeof(ModemCmd)); // Heap
    if (!r) return false;
    printf("Modem_SendAT malloc OK\r\n");
    strncpy(r->cmd, cmd, sizeof(r->cmd)-1); 
    r->cmd[sizeof(r->cmd)-1]=0;
    r->done_sem = xSemaphoreCreateBinary();
    r->timeout_ms = timeout_ms;
    r->resp[0] = '\0';
    r->waitForOK = true;
    r->start_tick = 0;

    // enqueue request (wait briefly)
    if (modem_cmd_queue == NULL || xQueueSend(modem_cmd_queue, &r, pdMS_TO_TICKS(2000)) != pdTRUE) {
        vSemaphoreDelete(r->done_sem);
        free(r);
        printf("Modem_SendAT free OK\r\n");
        return false;
    }

    // wait for response (task will give the semaphore)
    bool ok = false;
    if (xSemaphoreTake(r->done_sem, pdMS_TO_TICKS(timeout_ms + 500)) == pdTRUE) {
        // copy to caller buffer
        strncpy(resp, r->resp, resp_len-1);
        resp[resp_len-1] = '\0';
        ok = true;
        // if the modemTask reported a timeout marker, treat as failure
        if (strstr(resp, "OK") != NULL) {
            ok = true;
        }   
    }

    vSemaphoreDelete(r->done_sem);
    free(r);
    printf("Modem_SendAT free OK\r\n");
    return ok;
}

static bool Modem_AT(void){
    if (!modem_serial_begun) return false;
    char resp[64];
    if (!Modem_SendAT("AT", resp, sizeof(resp), 1000)) {
        printf("AT response: NO\r\n");
        return false;
    }

    if (strstr(resp, "OK")) {
        printf("AT response: OK\r\n");
        return true;
    }
    return false;
}



static void Modem_HandleURC(const char *line) {
    if (strstr(line, "+CREG:")) {

        DisplayEvent e = { .type = DISP_EVT_MODEM_NET, .payload = NULL };
        Display_PostEvent(&e, 0);
    }
}


// Request GNSS on, get raw +CGNSINF (SIMCOM-style) into resp
static bool Modem_GetGNSSRaw(char *resp, size_t resp_len, uint32_t timeout_ms) {
    if (!modem_ready) return false;
    char tmp[128];
    // enable GNSS
    Modem_SendAT("AT+CGNSPWR=1", tmp, sizeof(tmp), 1500);
    vTaskDelay(pdMS_TO_TICKS(800));
    // read GNSS info
    if (!Modem_SendAT("AT+CGNSINF", resp, resp_len, timeout_ms)) return false;
    return true;
}

// Parse UTC timestamp from a +CGNSINF line into out (YYYMMDDHHMMSS.sss)
static bool Modem_GetGNSSutc(char *resp, size_t resp_len, uint32_t timeout_ms) {
    const size_t TMP_SZ = 1024;
    char *tmp = (char*)malloc(TMP_SZ);
    if (!tmp) return false;
    printf("Modem_GetGNSSutc malloc OK\r\n");
    bool ok = false;
    if (!Modem_GetGNSSRaw(tmp, TMP_SZ, timeout_ms)) {
        free(tmp);
        printf("Modem_GetGNSSutc free OK\r\n");
        return false;
    }

    char *p = strstr(tmp, "+CGNSINF:");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            char *save = NULL;
            char *tok = strtok_r(p, ",", &save);
            int fld = 0;
            while (tok) {
                fld++;
                if (fld == 3) {
                    strncpy(resp, tok, resp_len-1);
                    resp[resp_len-1] = '\0';
                    ok = true;
                    break;
                }
                tok = strtok_r(NULL, ",", &save);
            }
        }
    }
    free(tmp);
    printf("Modem_GetGNSSutc free OK\r\n");
    return ok;
}

static bool Modem_CheckNetwork(void) {
    if (!modem_ready) {
        printf("Modem_CheckNetwork: modem is off, skipping\r\n");
        return false;
    }
    char resp[512];
    if (!Modem_SendAT("AT+CREG?", resp, sizeof(resp), 3000)) {
        printf("AT send Timeout or no valid response\r\n");
        return false;
    }
    if (strstr(resp, "+CREG: OK")) {
        Modem_HandleURC(resp);
        return true;
    }
    printf("Modem Network: No %s\r\n", resp);
    return false;
}



// Modem background task to handle command queue and URCs
static void modemTask(void *pv) {
    (void)pv;
    char line[256];
    size_t idx = 0;
    ModemCmd *queued = NULL;

    // Init modem
    
    printf("Waiting 20s for modem coldstart\r\n");
    DEV_Delay_ms(20000);
    /*
    printf("Restarting modem...\r\n");
    Modem_TogglePWK(2200);
    DEV_Delay_ms(25000);
    Modem_TogglePWK(1100);
    DEV_Delay_ms(36000);
    printf("Modem ready to begin serial.\r\n");
    */

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
            //printf("Modem init RX: %s\r\n", resp);
            if (strcmp(resp, "OK") == 0) {
                modem_ready = true;
                break;
            }
        }
        if (modem_ready) break;
        printf("Modem not responding to AT, retrying...\r\n");
        DEV_Delay_ms(1000);
    }

    if (modem_ready) {
        printf("Modem is ready!\r\n");
        DisplayEvent e = { .type = DISP_EVT_MODEM_READY, .payload = NULL};
        Display_PostEvent(&e, 0);
    } else {
        printf("Modem failed to respond to AT commands.\r\n");
    }

    for (;;) {
        // if modem not ready, wait and loop
        if (!modemSerial || !modem_ready) {
            DEV_Delay_ms(2500);
            continue;
        }

        // 1) Accept new command if none currently pending
        if (current_cmd == NULL && modem_cmd_queue) {
            if (xQueueReceive(modem_cmd_queue, &queued, 0) == pdTRUE) {
                printf("Command recived!\r\n");
                // start the command
                current_cmd = queued;
                printf("Dequeued AT: %s\r\n", current_cmd->cmd);
                current_cmd->start_tick = xTaskGetTickCount();
                // clear resp buffer
                current_cmd->resp[0] = '\0';
                // send the command
                if (modem_mutex && xSemaphoreTake(modem_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    modemSerial->print(current_cmd->cmd);
                    modemSerial->print("\r\n");
                    modemSerial->flush();
                    xSemaphoreGive(modem_mutex);
                } else {
                    // Failed to get mutex - abort command
                    strcpy(current_cmd->resp, "ERROR: modem_mutex timeout");
                    xSemaphoreGive(current_cmd->done_sem);
                    current_cmd = NULL;
                }
            }
        }

        // 2) Read available bytes and accumulate lines
        while (modemSerial->available()) {
            int c = modemSerial->read();
            printf("modemSerial->available()=%d read=%d\r\n", modemSerial->available(), c);
            if (c < 0) break;
            if (idx < sizeof(line) - 1) line[idx++] = (char)c;
            // line termination on LF
            if (c == '\n') {
                while (idx > 0 && (line[idx-1] == '\r' || line[idx-1] == '\n')) idx--;
                line[idx] = '\0';

                // debug log
                printf("Modem RX: %s\r\n", line);

                if (current_cmd) {
                    // append this line to the accumulated response (safe)
                    strncat(current_cmd->resp, line, sizeof(current_cmd->resp) - strlen(current_cmd->resp) - 2);
                    strncat(current_cmd->resp, "\n", sizeof(current_cmd->resp) - strlen(current_cmd->resp) - 1);

                    // termination detection for multiline AT responses
                    if (current_cmd->waitForOK) {
                        if (strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0 ||
                            strstr(line, "+CME ERROR") || strstr(line, "+CMS ERROR")) {
                            xSemaphoreGive(current_cmd->done_sem); // Return to sendAT
                            current_cmd = NULL;
                        }
                        // special-case prompt '>' (for SMS); caller must handle separately
                        else if (strcmp(line, ">") == 0) {
                            // deliver prompt as response so caller can detect and send message body
                            printf("Waiting for SMS response.\r\n");
                            xSemaphoreGive(current_cmd->done_sem);
                            // keep current_cmd NULL — caller must follow up with a raw write
                            current_cmd = NULL;
                        }
                    } else {
                        // single-line mode: return first line
                        xSemaphoreGive(current_cmd->done_sem);
                        current_cmd = NULL;
                    }
                } else {
                    // unsolicited URC handling (non-blocking)
                    Modem_HandleURC(line);
                }

                idx = 0;
            }
            DEV_Delay_ms(10);
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
    if (!modem_resp_sem) modem_resp_sem = xSemaphoreCreateBinary();
    // Queue
    if (!modem_cmd_queue) modem_cmd_queue = xQueueCreate(4, sizeof(ModemCmd*));

    if (modemSerial) {
        Modem_StartTask();

        // DEBUG
        //Modem_CheckNetwork();
    }
    return false;
}
