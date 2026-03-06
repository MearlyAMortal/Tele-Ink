#include "Modem.h"
#include "Arduino.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
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
static TaskHandle_t modem_background_task_handle = NULL;
static QueueHandle_t modem_cmd_queue = NULL;
static ModemCmd *current_cmd = NULL;
static bool modem_serial_begun = false;

// Removes anything not ASCII and makes it a space
void ReplaceControlChars(char* s) {
    if (!s) return;
    for (char *c = s; *c; c++) {
        if (*c < 0x20 || *c >= 0x7F) {
            *c = ' ';
        }
    }
}

// Public function for toggling modem PWK high for duration_ms to trigger power on/off or reset depending on duration
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

// Public function for easy restarting the modem automatically with the correct timing (long press then short press)
void Modem_Restart(void) {
    if (!modem_ready) {
        printf("Modem_Restart: Modem not ready, cannot restart.\r\n");
        return;
    }
    printf("Restarting modem...\r\n");
    Modem_TogglePWK(2200);
    DEV_Delay_ms(25000);
    Modem_TogglePWK(1100);
    DEV_Delay_ms(20000);
    printf("Modem restarted.\r\n");
}

// Writes raw data to modem while mutex is taken, returns true if all bytes written successfully before timeout, false if error or timeout
static bool Modem_WriteRaw(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!modemSerial || !modem_ready) return false;
    if (!data || len == 0) return true;
    if (!modem_mutex) return false;

    if (xSemaphoreTake(modem_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        size_t written = modemSerial->write(data, len);
        modemSerial->flush();
        if (modem_mutex) xSemaphoreGive(modem_mutex);
        return written == len;
    }
    return false;
}

// Wait for the queued command to finish and return result before continuing in the main task
// Uses malloc and free which can cause fragmentation but should be fine since commands are not frequent
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

// Wait until response is recived then signal to sender and free the wait struct. Returns true if OK received, false if error or timeout
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

// Sends AT command to modem and waits for response. (safe)
bool Modem_SendAT(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms) {
    if (!modemSerial || !cmd || !resp || resp_len == 0) return false;
    ModemCmd *r = (ModemCmd*)malloc(sizeof(ModemCmd)); // Heap
    if (!r) return false;
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
        // Check for actual OK (or > for SMS prompt)
        ok = (strstr(resp, "OK") != NULL || strchr(resp, '>') != NULL) &&
             (strstr(resp, "ERROR") == NULL) &&
             (strstr(resp, "+CME ERROR") == NULL) &&
             (strstr(resp, "+CMS ERROR") == NULL);  
    }

    vSemaphoreDelete(r->done_sem);
    free(r);
    return ok;
}

// Returns true if modem mode was set or already in that mode, false if error
bool Modem_SetCheckMode(uint8_t mode) {
    if (mode != 0 && mode != 1) return false;
    if (mode == modem_mode) return true;

    char tmp[32] = {0};
    if (!Modem_SendAT("AT+CMGF=1", tmp, sizeof(tmp), 2000)) {
        printf("SetCheckMode: CMGF failed\r\n");
        return false;
    }
    modem_mode = mode;
    return true;
}

// Send message to number, get prompt, write message and ctrl+z, wait for response. Returns true if OK received, false if error or timeout.
bool Modem_SendSMS(const char *number, const char *message, uint32_t timeout_ms) {
    if (!number || !number[0] || !message) return false;

    if (!Modem_SetCheckMode(1)) return false;
    char cmgs[96] = {0};
    char resp[32] = {0};
    snprintf(cmgs, sizeof(cmgs), "AT+CMGS=\"%s\"", number);
    Modem_SendAT(cmgs, resp, sizeof(resp), timeout_ms);
    if (strchr(resp, '>') == NULL) {
        printf("SMS: no > prompt\r\n");
        return false;
    }

    ModemCmd *w = Modem_QueueWaitOnly(timeout_ms);
    if (!w) return false;
    DEV_Delay_ms(100);
    // Write message body
    if (!Modem_WriteRaw((uint8_t*)message, strlen(message), timeout_ms)) {
        printf("SMS: write body failed\r\n");
        return false;
    }
    // Write ctrl+z to send
    const uint8_t ctrlz = 0x1A;
    if (!Modem_WriteRaw(&ctrlz, 1, timeout_ms)) {
        printf("SMS: write ctrlz failed\r\n");
        return false;
    }
    // Wait for response and free wait struct
    bool ok = Modem_WaitAndFree(w, timeout_ms);
    return ok;
}

static bool Modem_HandleURC(const char *line) {
    if (strcmp(line, "OK") == 0) {
        return true;
    }
    else if (strncmp(line, "+CMTI:", 6) == 0) {
        printf("SMS recieved! Index: %s\r\n", line + 12);
        DisplayEvent e = { .type = DISP_EVT_SMS_RECEIVED, .payload = NULL};
        Display_PostEvent(&e, 0);
        DEV_Delay_ms(250);
        return true;
    } 
    else {
        printf("Modem URC unhandled: %s\r\n", line);
    }
    return false;
}

// Returns true if state changed. update internal based on phyisical state (mostly helpful for communication between displayTask)
bool Modem_CheckStatus(void) {
    // modemTask must have been deinitialized or serial = nullptr or something catostrophic happened if modemSerial is null at this point
    if (!modemSerial && modem_serial_begun) {
        modem_serial_begun = false;
        DisplayEvent e = { .type = DISP_EVT_MODEM_LOST, .payload = NULL};
        Display_PostEvent(&e, 0);
        DEV_Delay_ms(250);
        return true;
    }
    // Take mutex for entire check to prevent modemTask from doing too much fr tho
    // Give before each return but after the delay to ensure display task has time to process event and update modem state before we potentially check again or do something else with the modem
    if (modem_mutex && xSemaphoreTake(modem_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        printf("Modem_CheckStatus: failed to take modem_mutex (timeout)\r\n");
        return false;
    }
    // Begin serial if not already begun (handles first power on or restart case, but also modemTask deinit case where we set modemSerial to null but didnt update state yet)
    if (!modem_serial_begun) {
        modemSerial->begin(115200, SERIAL_8N1, modem_rx_pin, modem_tx_pin);
        modemSerial->flush();
        DEV_Delay_ms(100);
        modem_serial_begun = true;
    }
    // Send AT manually to check if modem is ready
    modemSerial->print("AT\r\n");
    unsigned long start = millis();
    char resp[64] = {0};
    int idx = 0;
    bool at = false;
    bool creg = false;
    // Read response with timeout
    while (millis() - start < 1000 && idx < sizeof(resp) - 1) {
        if (modemSerial->available()) {
            char c = modemSerial->read();
            resp[idx++] = c;
            resp[idx] = '\0';
            if (strstr(resp, "OK")) {
                at = true;
                break;
            }
        }
    }
    // No response modem lost dont continue
    if (!at) {
        xSemaphoreGive(modem_mutex);
        // Did we lose the modem or is it just not ready?
        if (modem_ready || modem_net) {
            DisplayEvent e = { .type = DISP_EVT_MODEM_LOST, .payload = NULL};
            Display_PostEvent(&e, 0);
            DEV_Delay_ms(250);
            return true;
        } 
        return false;
    }
    // At least ready at this point
    // Manually check registration status if not marked as lost or init (AT is sufficient to keep modem_net true).
    if (!modem_net) {
        resp[0] = '\0';
        modemSerial->print("AT+CREG?\r\n");
        start = millis();
        idx = 0;
        while (millis() - start < 1000 && idx < sizeof(resp) - 1) {
            if (modemSerial->available()) {
                char c = modemSerial->read();
                resp[idx++] = c;
                resp[idx] = '\0';
                if (strstr(resp, "0,5") || strstr(resp, "0,1")) {
                    creg = true;
                    break;
                }
            }
        }
        // Registered to home or roaming network
        if (creg) {
            xSemaphoreGive(modem_mutex);
            DisplayEvent e = { .type = DISP_EVT_MODEM_NET, .payload = NULL};
            Display_PostEvent(&e, 0);
            DEV_Delay_ms(250);
            return true;
        }
        // Not registered to network, but modem is ready at least
        else if (!modem_ready && at){
            xSemaphoreGive(modem_mutex);
            DisplayEvent e = { .type = DISP_EVT_MODEM_READY, .payload = NULL};
            Display_PostEvent(&e, 0);
            DEV_Delay_ms(250);
            return true;
        }

    }
    xSemaphoreGive(modem_mutex);
    return false;
}

// Converts NMEA-style latitude/longitude to decimal degrees
static double nmea_to_decimal(const char *val, char dir) {
    double deg, min;
    int deg_width = (dir == 'N' || dir == 'S') ? 2 : 3;
    char deg_str[4] = {0};
    strncpy(deg_str, val, deg_width);
    deg = atof(deg_str);
    min = atof(val + deg_width);
    double dec = deg + min / 60.0;
    if (dir == 'S' || dir == 'W') dec = -dec;
    return dec;
}

// Check for 7 commas in a row (8 empty fields)
bool is_empty_gnss(const char *input) {
    while (*input == ' ' || *input == '\t') input++;
    int comma_count = 0;
    for (const char *p = input; *p; ++p) {
        if (*p == ',') comma_count++;
        else if (*p != '\r' && *p != '\n' && *p != ' ' && *p != '\t') return false;
    }
    return comma_count >= 7;
}

// Converts GNSS AT output to a one-line summary, takes mutex internally, updates global gnss_data, sends display event if we on home page for update
void GNSS_ToOneLinerAndUpdate(const char *input, char *output, size_t out_size) {
    char lat[16], ns, lon[16], ew, date[8], time[10], alt[8], spd[8];
    // Parse the fields
    int n = sscanf(input, "%15[^,],%c,%15[^,],%c,%6[^,],%9[^,],%7[^,],%7[^,]",
        lat, &ns, lon, &ew, date, time, alt, spd);
    if (is_empty_gnss(input) || n < 8) {
        snprintf(output, out_size, "Error: Failed to fix or parse");
        return;
    }
    double lat_dd = nmea_to_decimal(lat, ns);
    double lon_dd = nmea_to_decimal(lon, ew);
    // Convert two-character strings into integers for day, month, year, hour, min, sec
    int year = 2000 + ((date[4] - '0') * 10 + (date[5] - '0'));
    int month = ((date[2] - '0') * 10 + (date[3] - '0')) - 1; // tm_mon is 0-based
    int day = ((date[0] - '0') * 10 + (date[1] - '0'));
    int hour = ((time[0] - '0') * 10 + (time[1] - '0'));
    int min = ((time[2] - '0') * 10 + (time[3] - '0'));
    int sec = ((time[4] - '0') * 10 + (time[5] - '0'));
    // Format local time struct
    struct tm tm_struct = {0};
    tm_struct.tm_year = year - 1900; // tm_year start 1900
    tm_struct.tm_mon = month;
    tm_struct.tm_mday = day;
    tm_struct.tm_hour = hour;
    tm_struct.tm_min = min;
    tm_struct.tm_sec = sec;
    // Calculate extimated UTC offset using longitude (15 degrees per hour)
    int offset = (int)(lon_dd / 15);
    tm_struct.tm_hour += offset;
    // Normalize time struct (handles day/month/year rollover)
    time_t t = mktime(&tm_struct);
    // Set strings for output and global state 
    char local_date_fmt[16], local_time_fmt[16];
    strftime(local_date_fmt, sizeof(local_date_fmt), "%Y-%m-%d", localtime(&t));
    strftime(local_time_fmt, sizeof(local_time_fmt), "%H:%M:%S", localtime(&t));
    // Grab UTC from original input
    char date_fmt[16], time_fmt[16];
    snprintf(date_fmt, sizeof(date_fmt), "20%c%c-%c%c-%c%c", date[4], date[5], date[2], date[3], date[0], date[1]);
    snprintf(time_fmt, sizeof(time_fmt), "%c%c:%c%c:%c%c", time[0], time[1], time[2], time[3], time[4], time[5]);

    // Output one-liner UTC, local time is global (keep exact UTC for cmd mode)
    snprintf(output, out_size, "La: %.6f Lo: %.6f UTC: %s Date: %s Alt: %sm Spd: %skn", lat_dd, lon_dd, time_fmt, date_fmt, alt, spd);
    if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        gnss_data.latitude = lat_dd;
        gnss_data.longitude = lon_dd;
        strncpy(gnss_data.time, local_time_fmt, sizeof(gnss_data.time)-1);
        gnss_data.time[sizeof(gnss_data.time)-1] = '\0';
        strncpy(gnss_data.date, local_date_fmt, sizeof(gnss_data.date)-1);
        gnss_data.date[sizeof(gnss_data.date)-1] = '\0';
        strncpy(gnss_data.altitude, alt, sizeof(gnss_data.altitude)-1);
        gnss_data.altitude[sizeof(gnss_data.altitude)-1] = '\0';
        strncpy(gnss_data.speed, spd, sizeof(gnss_data.speed)-1);
        gnss_data.speed[sizeof(gnss_data.speed)-1] = '\0';
        xSemaphoreGive(gnss_data.mutex);
    } else {
        printf("GNSS_ToOneLinerAndUpdate: failed to take gnss_data mutex\r\n");
    }
    // Requires user to repaint if they want current data rather than spamming fullscreen updates 
}

// Parses +CESQ response, updates global signal_data with mutex, returns true if parsed and updated successfully, false if error
static bool CESQ_ParseAndUpdate(const char *input) {
    uint8_t rxl, ber, rscp, ecno, rsrq, rsrp;
    if (sscanf(input, "%hhu,%hhu,%hhu,%hhu,%hhu,%hhu", &rxl, &ber, &rscp ,&ecno, &rsrq, &rsrp) != 6) {
        return false;
    }
    if (xSemaphoreTake(signal_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        signal_data.rxlev = rxl;
        signal_data.ber = ber;
        signal_data.rscp = rscp;
        signal_data.ecno = ecno;
        signal_data.rsrq = rsrq;
        signal_data.rsrp = rsrp;
        xSemaphoreGive(signal_data.mutex);
        return true;
    } else {
        printf("CESQ_ParseAndUpdate: failed to take signal_data mutex\r\n");
        return false;
    }
}


// Handles modem status checking, +CESQ polling, and GNSS polling. URC is still handled by main task (not time sensitive)
// One task for all background tasks other than URC becuase task switching overhead was inhibiting other tasks
// Use task tick count for correct delay times on each specifc background job
static void ModemBackgroundTask(void *pv) {
    (void)pv;
    char resp[128] = {0};
    bool gnss = false;

    TickType_t last_status_check = xTaskGetTickCount();
    TickType_t last_cesq_call = xTaskGetTickCount();
    TickType_t last_gnss_call = xTaskGetTickCount();
    TickType_t now = 0;

    for (;;) {
        now = xTaskGetTickCount();
        // Status handling if enough time has passed since last check (10s)
        // Status check can be high blocking for modemTask but want responsive system diagnosis
        if (now - last_status_check >= pdMS_TO_TICKS(10000)) {
            if (Modem_CheckStatus()) printf("Modem status changed!\r\n");
            last_status_check = now;
        }
        // No need to proceed, the modem is not ready for anything. Wait a full second before checking again (could be intentionally not ready)
        if (!modem_ready || !modem_net) {
            DEV_Delay_ms(1000);
            continue;
        }
        // CESQ handling if enough time has passed since last check (15s)
        if (now - last_cesq_call >= pdMS_TO_TICKS(15000)) {
            if (Modem_SendAT("AT+CESQ", resp, sizeof(resp), 2000)) {
                ReplaceControlChars(resp);
                char *data = strstr(resp, "+CESQ:");
                if (data) {
                    data += strlen("+CESQ:");
                    while (*data == ' ') data++;
                    // First try always fails due to OK appearing before response from previous command?
                    if (!CESQ_ParseAndUpdate(data)){
                        printf("ModemCESQTask: Failed to parse and update CESQ data\r\n");
                    }
                } else {
                    printf("ModemCESQTask: No +CESQ: in response!\r\n");
                }
            }
            last_cesq_call = now;
            memset(resp, 0, sizeof(resp));
        }
        // GNSS handling if enought time has passed since last check and GNSS is on.
        if (now - last_gnss_call >= pdMS_TO_TICKS(15000)) {
            if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                gnss = gnss_data.gnss_on;
                xSemaphoreGive(gnss_data.mutex);
            }
            if (gnss && modem_ready) {
                if (Modem_SendAT("AT+CGPSINFO", resp, sizeof(resp), 2000)) {
                    ReplaceControlChars(resp);
                    char *data = resp + strlen("AT+CGPSINFO +CGPSINFO: ");
                    while (*data == ' ') data++;
                    GNSS_ToOneLinerAndUpdate(data, resp, sizeof(resp));
                }
                
            }
            last_gnss_call = now;
            memset(resp, 0, sizeof(resp));
        }
        // Loop half a second if no jobs are ready (not expensive)
        DEV_Delay_ms(500);
    }
}


// Handles status, signal quality, and GNSS polling. URC is still handled by main task
static void Modem_StartBackgroundTask(void) {
    if (!modem_background_task_handle) {
        xTaskCreatePinnedToCore(ModemBackgroundTask, "modemBackground", 4096, NULL, 4, &modem_background_task_handle, 1);
    }
}



// Modem background task to handle command queue and URCs
// ready/powered/net State is external in display and handled by display events
// display events are called from here so give some time for the display task to process them and update modem state 
static void modemTask(void *pv) {
    (void)pv;
    //TaskHandle_t status_task_handle = NULL;
    char line[256];
    size_t idx = 0;

    printf("Waiting 20s for modem coldstart\r\n");
    DEV_Delay_ms(20000);

    // Create background task to handle status, gnss, and cesq polling
    Modem_StartBackgroundTask();
    DEV_Delay_ms(1000);


    // Init complete, enter main loop to handle commands and URCs
    for (;;) {
        // if modem not ready, check current status
        if (!modem_ready || !modemSerial) {
            DEV_Delay_ms(5000);
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
                    if (modem_mutex && xSemaphoreTake(modem_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
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
        if (modem_mutex && xSemaphoreTake(modem_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            while (modemSerial->available()) {
                int c = modemSerial->read();
                if (c < 0) break;
                // If we're waiting for OK and we see '>', send ready AT+CMGS
                if (current_cmd && current_cmd->waitForOK && c == '>') {
                    // Only treat as SMS prompt if the command sms
                    if (strncmp(current_cmd->cmd, "AT+CMGS", 7) == 0) {
                        strncat(current_cmd->resp, ">\n", sizeof(current_cmd->resp) - strlen(current_cmd->resp) - 1);
                        xSemaphoreGive(current_cmd->done_sem);
                        xSemaphoreGive(modem_mutex);
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
                    // DEBUG
                    if (line[0] != '\0') printf("Modem RX: %s\r\n", line);
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
            xSemaphoreGive(modem_mutex);
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

    // Mutex for modem uart access and state
    if (!modem_mutex) modem_mutex = xSemaphoreCreateMutex();
    // Mutex for gnss data access
    if (!gnss_data.mutex) gnss_data.mutex = xSemaphoreCreateMutex();
    // Mutex for signal data access
    if (!signal_data.mutex) signal_data.mutex = xSemaphoreCreateMutex();
    // Mutex for wifi data access
    if (!wifi_data.mutex) wifi_data.mutex = xSemaphoreCreateMutex();
    // Queue
    if (!modem_cmd_queue) modem_cmd_queue = xQueueCreate(4, sizeof(ModemCmd*));
    if (modemSerial) Modem_StartTask();
    
    return true;
}
