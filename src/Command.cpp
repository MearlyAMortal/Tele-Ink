#include "Command.h"
// For public queue/buffer
#include "Display.h"
#include "Modem.h"
#include "ESP32_WiFi.h"
#include <stdio.h>
// FD
#include <string.h>

static char sms_number[32] = {0};

// Quick way to exit the if else tree if condition is not met
// Takes a string for the output command (usually an error)
static void Command_SetDone(const char* out){
    if (!out) return;
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        strncpy(cmd_buffer.output, out, sizeof(cmd_buffer.output) - 1);
        cmd_buffer.state = CMD_STATE_DONE;
        xSemaphoreGive(cmd_buffer.mutex);
    }
    // State will reflect processing if the mutex cannot be taken but should return back to typing.    
}

// Makes sure that the line is safe and starts at correct characters;
static void TrimRight(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            s[n - 1] = '\0';
            n--;
        } else {
            break;
        }
    }
}

// Returns true if 10 digit number (can include +CC)
static bool Sms_IsValidNumber(const char* s) {
    if (!s || *s == '\0') return false;
    int i = 0;
    if (s[i] == '+') i++;
    int digits = 0;
    for (;s[i]; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        digits++;
        if (digits > 15) return false;
    }
    return digits >= 10;
}


// Takes message number makes sure its numbers and subtracts 1 to correspond with idx not number
static int ValidateID(const char* str){
    if (!str || strlen(str) == 0) return -1;
    
    // Check all digits
    for (int i = 0; str[i]; i++) {
        if (str[i] < '0' || str[i] > '9') return -1;
    }
    
    int idx = atoi(str);
    return idx-1; // FIXING USER INPUT TO MATCH MODEM IDX NOT NUMBER
}


// Takes idx number after validateID and checks if it matches any of the saved sms idxs from the last AT+CMGL command and returns true if it does, false if not
static bool IsValidSmsID(int idx){
    for (int i = 0; i < 10; i++) {
        if (sms_ids[i] == idx) return true;
    }
    return false;
}

// Removes the given idx from the global sms_ids and shifts remaining ids to the left
static void RemoveSmsID(int idx){
    bool found = false;
    for (int i = 0; i < 10; i++) {
        if (sms_ids[i] == idx) {
            found = true;
        }
        if (found && i < 9) {
            sms_ids[i] = sms_ids[i + 1];
        }
    }
    if (found) {
        sms_ids[9] = -1;
    }
}


// Collect unread sms message idxs for later retrieval user "ALL" filter to collect unread and read messages
static int GetSmsIndices(const char* resp, const char* status_filter, int* ids, int max_ids) {
    int count = 0;
    const char* p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL && count < max_ids) {
        const char* status = strchr(p, ',');
        if (status) {
            if (strcmp(status_filter, "ALL") == 0 || strstr(status, status_filter)) {
                p += 6;
                while (*p == ' ') p++;
                int idx = atoi(p);
                ids[count++] = idx;
            }
        }
        p = strchr(p, '\n');
        if (!p) break;
    }
    return count;
}

// Make printable id response for display and set global ids
static void SetSmsNumbers(char* id_str, const int* ids, int num) {
    for (int i = 0; i < num && i < 10; i++) {
        // Save to global
        sms_ids[i] = ids[i];
        // Build display string
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", ids[i] + 1);
        strcat(id_str, buf);
        if (i < num - 1) {
            strcat(id_str, ",");
        }
    }
}



// Wizard for handling sending sms messages when in sms_send mode
static void SMS_SEND_Wizard(char *in) {
    if (strcmp(in, "/exit") == 0) {
        Command_SetDone("Exiting SMS send");
        sms_send = false;
        return;
    }
    // We have number and message here
    if (Modem_SendSMS(sms_number, in, 45000)){
        Command_SetDone("Message sent!");
    } else {
        Command_SetDone("Error: SMS failed to send");
    }
    sms_send = false;
    return;
}

static void SMS_READ_Wizard(char *in) {
    // No messages to read left from unread and not responding
    if (sms_count <= 0 && strcmp(in, "/s") != 0) {
        Command_SetDone("Exiting: No SMS to read");
        sms_read = false;
        sms_count = 0;
        sms_read_all = false;
        memset(sms_ids, -1, sizeof(sms_ids));
        return;
    }
    if (strncmp(in, "/exit", 5) == 0) {
        Command_SetDone("Exiting SMS read");
        sms_read = false;
        sms_count = 0;
        sms_read_all = false;
        memset(sms_ids, -1, sizeof(sms_ids));
        return;
    }
    // Responding to immediate previously read message with valid sms_number
    if (strcmp(in, "/s") == 0 && sms_number[0] != '\0') {
        sms_send = true;
        char buf[32];
        snprintf(buf, sizeof(buf), "Responding: %s", sms_number); // Ensure null termination
        Command_SetDone(buf);
        return;
    } 
    char cmd[32] = {0};
    // Need relativly large buffer in case large amount of messages or message content
    char tmp[512] = {0};
    // Deleting message
    if (strncmp(in, "/d", 2) == 0) {
        // Delete all msgs on sim
        if (strncmp(in, "/da", 3) == 0) {
            if (Modem_SendAT("AT+CMGD=1,4", tmp, sizeof(tmp), 5000)) {
                sms_count = 0;
                sms_unread_count = 0;
                sms_read = false;
                sms_read_all = false;
                memset(sms_ids, -1, sizeof(sms_ids));
                Command_SetDone("Successfully deleted all SMS");
            }
            else {
                Command_SetDone("Error: Failed to delete SMS");
            }
            return;
        }
        // Delete message by idx
        else if (strncmp(in, "/d ", 3) == 0) {
            int idd = ValidateID(in + 3);
            if (idd < 0 || !IsValidSmsID(idd)) {
                Command_SetDone("Error: Invalid index");
                return;
            }
            // Send delete AT
            snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", idd);
            if (Modem_SendAT(cmd, tmp, sizeof(tmp), 5000)) {
                --sms_count;
                RemoveSmsID(idd);
                Command_SetDone("Successfully deleted");
            }
            else {
                Command_SetDone("Error: Failed to delete");
            }
            return;
        }
    }
    // Reading message by idx
    int idr = ValidateID(in);
    if (idr >= 0 && IsValidSmsID(idr)) {
        // Send read AT and fix response and sms_count
        snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idr);
        Modem_SendAT(cmd, tmp, sizeof(tmp), 2000);
        ReplaceControlChars(tmp);
        // Delete message from unread and global ids since its read
        if (!sms_read_all) {
            --sms_count;
            RemoveSmsID(idr);
        }


        // Get phone number saved for response and only display message body to user
        char number[32] = {0};
        int quote_count = 0;
        const char *p = tmp;
        // Skip to phone number from response
        while (*p) {
            if (*p == '"') {
                quote_count++;
                if (quote_count == 3) {
                    const char *start = p + 1;
                    const char *end = strchr(start, '"');
                    if (end && end - start < sizeof(number)) {
                        strncpy(number, start, end - start);
                        number[end - start] = '\0';
                    }
                    break;
                }
            }
            p++;
        }
        // Check if valid number and save globally for response or later sending
        if (Sms_IsValidNumber(number)) {
            sms_number[0] = '\0';
            strncpy(sms_number, number, sizeof(sms_number) - 1);
            sms_number[sizeof(sms_number) - 1] = '\0';
        }

        // Set output to message body only
        snprintf(cmd, sizeof(cmd), "AT+CMGR=%d +CMGR: ", idr);

        // Finding header
        if (strstr(tmp, cmd) == NULL) {
            Command_SetDone("Error: Failed to parse message");
            return;
        }

        char *data = tmp + strlen(cmd);
        while (*data == ' ') data++;
        // skip the "REC UNREAD/READ" status at the start of the data
        if (*data == '"') {
            data++; // skip opening quote
            while (*data && *data != '"') data++;
            if (*data == '"') data++; // skip closing quote
            if (*data == ',') data++; // skip comma after status
            while (*data == ' ') data++;
        }
        // Remove trailing OK from message
        TrimRight(data);
        size_t len = strlen(data);
        if (len >= 2) {
            // Check for "OK" at the end (case-sensitive) before removing
            if ((len >= 2 && strcmp(data + len - 2, "OK") == 0)) {
                data[len - 2] = '\0';
                TrimRight(data);
            }
        }
        Command_SetDone(data);
    }
    else {
        Command_SetDone("Error: Invalid index");
    }
    return;
}

// Wizard for handling single line AT commands from command mode
static void AT_Wizard(char *in) {
    if (strcmp(in, "/exit") == 0) {
        Command_SetDone("Exiting AT mode");
        at_mode = false;
        return;
    } 
    char at_cmd[CMD_BUFFER_SIZE] = {0};
    // Parse for AT else repent AT into command to correspond w display
    snprintf(at_cmd, sizeof(at_cmd), "AT%s", in);
    // Send AT command and collect response
    char at_resp[CMD_BUFFER_SIZE] = {0};
    if (Modem_SendAT(at_cmd, at_resp, CMD_BUFFER_SIZE, 5000)){
        ReplaceControlChars(at_resp);
        // Remove input echo from response
        char *resp_data = at_resp + strlen(at_cmd);
        while (*resp_data == ' ') resp_data++;
        Command_SetDone(resp_data);
    } else {
        Command_SetDone("Error: AT failed or timed out");
    }
    return;
}

// Wizard for handling gnss mode inputs.
static void GNSS_Wizard(char *in) {
    bool was_on = false;
    if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        was_on = gnss_data.gnss_on;
        xSemaphoreGive(gnss_data.mutex);
    } else {
        Command_SetDone("Error: Cant take GNSS mutex");
        return;
    }
    // Exiting gnss mode but GNSS can still be on
    if (strcmp(in, "/exit") == 0) {
        if (was_on) {
            Command_SetDone("Exiting mode GNSS: ON");
        } else {
            Command_SetDone("Exiting mode GNSS: OFF");
        }
        gnss_mode = false;
        return;
    }
    char gnss_info[512] = {0};
    gnss_info[0] = '\0';
    if (strncmp(in, "on", 2) == 0) {
        if (was_on) {
            Command_SetDone("Error: GNSS is allready on");
        }
        else if (Modem_SendAT("AT+CGPS=1", gnss_info, sizeof(gnss_info), 5000)) {
            if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                gnss_data.gnss_on = true;
                xSemaphoreGive(gnss_data.mutex);
            } else {
                Command_SetDone("Error: Cant take GNSS mutex");
                return;
            }
            Command_SetDone("GNSS turned on wait for fix");
        } 
        else {
            Command_SetDone("Error: Failed to turn on GNSS");
        }
        return;
    }
    else if (strncmp(in, "off", 3) == 0) {
        if (!was_on) {
            Command_SetDone("Error: GNSS is allready off");
        }
        else if (Modem_SendAT("AT+CGPS=0", gnss_info, sizeof(gnss_info), 5000)) {
            if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                gnss_data.gnss_on = false;
                xSemaphoreGive(gnss_data.mutex);
            } else {
                Command_SetDone("Error: Cant take GNSS mutex");
                return;
            }
            Command_SetDone("GNSS turned off");
        } 
        else {
            Command_SetDone("Error: Failed to turn off GNSS");
        }
        return;
    }
    else if (strncmp(in, "info", 4) == 0) {
        if (!was_on) {
            Command_SetDone("Error: GNSS is not on");
        }
        else if (Modem_SendAT("AT+CGPSINFO", gnss_info, sizeof(gnss_info), 5000)) {
            ReplaceControlChars(gnss_info);
            char *data = gnss_info + strlen("AT+CGPSINFO +CGPSINFO: ");
            while (*data == ' ')
                data++;
            GNSS_ToOneLinerAndUpdate(data, gnss_info, sizeof(gnss_info));
            Command_SetDone(gnss_info);
        } 
        else {
            Command_SetDone("Error: Failed to get GNSS");
        }
        return;
    } else {
        Command_SetDone("Error: Unknown GNSS command");
    }
    return;
}


// Wizard for WiFi handling (under costruction) (only one mode at a time)
static void WiFi_Wizard(char *in) {
    // Edit wifi_mode even if wifi is still on
    if (strcmp(in, "/exit") == 0) {
        Command_SetDone("Exiting WiFi mode");
        wifi_mode = false;
        return;
    }
    bool was_on = false;
    bool was_scan = false;
    bool was_connected = false;
    bool was_host = false;
    // Try to take wifi_data mutex to copy state and return if cant so further take/gives are safe(ish)
    if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        was_on = wifi_data.wifi_on;
        was_scan = wifi_data.wifi_scan;
        was_connected = wifi_data.wifi_connected;
        was_host = wifi_data.wifi_host;
        xSemaphoreGive(wifi_data.mutex);
    } else {
        Command_SetDone("Error: Cant take WiFi mutex");
        return;
    }
    // Stop all wifi modes immediately via WiFi library
    if (strncmp(in, "stop", 4) == 0) {
        if (!was_on) {
            Command_SetDone("Error: Wifi is off allready");
            return;
        }
        if (was_scan) {
            if (WiFi_StopScanner()) {
                if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    wifi_data.wifi_scan = false;
                    wifi_data.wifi_on = false;
                    xSemaphoreGive(wifi_data.mutex);
                } else {
                    Command_SetDone("Error: Cant take WiFi mutex");
                    return;
                }
                Command_SetDone("Stopped WiFi scanning");
                return;
            }
            else {
                Command_SetDone("Error: Cant stop scanner");
                return;
            }
        }
        /*
        else if (was_connected) {
            if (WiFi_Disconnect()) {
                if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    wifi_data.wifi_connected = false;
                    xSemaphoreGive(wifi_data.mutex);
                } else {
                    Command_SetDone("Error: Cant take WiFi mutex");
                    return;
                }
                Command_SetDone("Disconnected!");
                return;
            }
            else {
                Command_SetDone("Error: Couldnt disconnect");
                return;
            }
        }
        */
        else if (was_host) {
            if (WiFi_StopHost()) {
                if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    wifi_data.wifi_host = false;
                    wifi_data.wifi_on = false;
                    xSemaphoreGive(wifi_data.mutex);
                } else {
                    Command_SetDone("Error: Cant take WiFi mutex");
                    return;
                }
                Command_SetDone("Stopped hosting");
                return;
            } 
            else {
                Command_SetDone("Error: Couldnt stop host");
                return;
            }
        } 
        else {
            Command_SetDone("Error: Unknown wifi state");
        }
        return;
    }
    // Wifi Scanning mode activation
    if (strncmp(in, "scan", 4) == 0) {
        if (WiFi_StartScanner(30000)) {
            if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                wifi_data.wifi_on = true;
                wifi_data.wifi_scan = true;
                xSemaphoreGive(wifi_data.mutex);
            } else {
                Command_SetDone("Error: Cant take WiFi mutex");
                return;
            }
            Command_SetDone("WiFi scan started...");
        } 
        else {
            Command_SetDone("Error: WiFi scan failed");
        }
        return;
    }
    else if (strncmp(in, "connect", 7) == 0) {
        Command_SetDone("Error: No connecting yet");
        return;
    }
    else if (strncmp(in, "host", 4) == 0) {
        if (WiFi_StartHost("Tele-Ink-AP", "password123")) {
            if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                wifi_data.wifi_on = true;
                wifi_data.wifi_host = true;
                xSemaphoreGive(wifi_data.mutex);
            } else {
                Command_SetDone("Error: Cant take WiFi mutex");
                return;
            }
            Command_SetDone("WiFi Soft AP started...");
        } 
        else {
            Command_SetDone("Error: Soft AP failed");
        }
        return;
    }
    else {
        Command_SetDone("Error: Not a valid WiFi cmd");
    }
    return;
}

// Helper to return true if wifi state is showing "off" or sets command done with an error and returns false
static bool WiFiOff() {
    if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (wifi_data.wifi_on) {
            xSemaphoreGive(wifi_data.mutex);
            Command_SetDone("Error: Turn WiFi off first");
            return false;
        }
        xSemaphoreGive(wifi_data.mutex);
    } else {
        Command_SetDone("Error: Cant take WiFi mutex");
        return false;
    }
    return true;
}

// Helper to return true if gnss state is showing "off" or sets command done with an error and returns false
static bool GNSSOff() {
    if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (gnss_data.gnss_on) {
            Command_SetDone("Error: Turn GNSS off first");
            xSemaphoreGive(gnss_data.mutex);
            return false;
        }
        xSemaphoreGive(gnss_data.mutex);
    } else {
        Command_SetDone("Error: Cant take GNSS mutex");
        return false;
    }
    return true;
}



// Interprets keyboard input and responds with an error or display/modem/etc event.
// Possibly the most ugly if else switch statement in existence (but it works)
void Command_Handle(void){
    if (!cmd_buffer.input) return;
    char in[CMD_BUFFER_SIZE] = {0};

    // Grab the command buffer and set state as processing
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        strncpy(in, cmd_buffer.input, sizeof(in) - 1);
        cmd_buffer.state = CMD_STATE_PROCESSING;
        xSemaphoreGive(cmd_buffer.mutex);
    } else {
        Command_SetDone("Error: Cant take CMD mutex");
        return;
    }

    // Trim input for easier parsing but also safety
    TrimRight(in);

    /* YOUR A WIZARD 
     * Handles non base case $ command modes like GNSS, SMS, WIFI etc
     * And check multi-line command mode first before handling base $
     * EX: my prompt for a ssid and password in seperate calls for one single command action when connecting to wifi 
    */

    // Send sms once collected number and message
    if (sms_send) {
        SMS_SEND_Wizard(in);
        return;
    } 
    // Read messages based on index (ru consumes messages, can also delete by index or all)
    else if (sms_read) {
        SMS_READ_Wizard(in);
        return;
    }
    // AT single line mode wizard
    else if (at_mode) {
        AT_Wizard(in);
        return;
    }
    // GNSS wizard
    else if (gnss_mode) {
        GNSS_Wizard(in);
        return;
    }
    // WiFi Wizard for scanning connecting and hosting (need to build more)
    else if (wifi_mode) {
        WiFi_Wizard(in);
        return;
    }

    // No wizardry needed so continue with muggle input

    // PARSE default $ INPUT
    // Not command, echo message
    if (in[0] != '/'){
        Command_SetDone(in);
        return;
    }
    // Default command error for now
    char out[CMD_BUFFER_SIZE] = {0};
    strncpy(out, "Error: Unknown command", sizeof(out) - 1);

    // If else tree of doom that can select mode or exectute specific commands
    // Help menu
    if (strcmp(in, "/help") == 0 || strcmp(in, "/h") == 0) {
        Command_SetDone("CMDS: /at /gnss /sms /sim /clear");
        return;
    } 
    // Clear history
    else if (strcmp(in, "/clear") == 0) {
        Display_ClearCommandHistory();
        Command_SetDone("History cleared!");
        return;
    }
    // ESP control
    else if (strncmp(in, "/esp", 4) == 0) {
        if (strcmp(in, "/esp rst") == 0) {
            Modem_TogglePWK(3000);
            DEV_Delay_ms(8000);
            ESP.restart();
        } else {
            Command_SetDone("Error: Unknown ESP command");
            return;
        }
    }
    // Modem external control
    else if (strncmp(in, "/sim", 4) == 0) {
        if (strcmp(in, "/sim on") == 0) {  
            if (!modem_ready) {
                Modem_TogglePWK(1200);
                Command_SetDone("Toggled pwk for modem ON");
            } else {
                Command_SetDone("Error: Modem is ON");
            }
            return;
        } else if (strcmp(in, "/sim off") == 0) {  
            if (modem_ready) {
                Modem_TogglePWK(3000);
                ResetGlobalModeState();
                Command_SetDone("Toggled pwk for modem OFF");
            } else {
                Command_SetDone("Error: Modem is OFF");
            }
            return;
        } else if (strcmp(in, "/sim rst") == 0) {  
            if (modem_ready) {
                ResetGlobalModeState();
                Modem_Restart();
                Command_SetDone("Restarted modem");
            } else {
                Command_SetDone("Error: Modem not ready");
            }
            return;
        } 
        else if (strcmp(in, "/sim net") == 0) {
            if (modem_ready) {
                char tmp[256] = {0};
                Modem_SendAT("AT+CREG?", tmp, sizeof(tmp), 5000);
                ReplaceControlChars(tmp);
                Command_SetDone(tmp);
            } else {
                Command_SetDone("Error: Modem not ready");
            }
            return;
        } else {
            Command_SetDone("Error: Unknown SIM command");
        }
        return;
    }
    // Start modem sms wizard /sms <number> 
    else if (strncmp(in, "/sms", 4) == 0) {
        if (!modem_ready){
            Command_SetDone("Error: Modem is not ready");
            return;
        }
        // Make sure GNSS is off first
        if (!GNSSOff()) return;
        // Make sure WiFi is off first
        if (!WiFiOff()) return;
        // Enable text mode
        char tmp[512] = {0};
        if (!Modem_SetCheckMode(1)){
            Command_SetDone("Error: Text mode not enabled");
            return;
        } 
        tmp[0] = '\0';
        int m = 0; //total msgs
        char id_str[32] = {0};
        int ids[10];
        // Reading recivied messages
        if (strncmp(in, "/sms r", 6) == 0) {
             // Unread msgs
            if (strncmp(in, "/sms ru", 7) == 0) {
                if (!Modem_SendAT ("AT+CMGL=\"REC UNREAD\"", tmp, sizeof(tmp), 5000)) {
                    Command_SetDone("Error: Failed to read SMS");
                    return;
                }
                m = GetSmsIndices(tmp, "\"REC UNREAD\"", ids, 10);
                SetSmsNumbers(id_str, ids, m);
                if (m > 0) {
                    sms_count = m; 
                    sms_unread_count = 0;
                    sms_read = true; 
                    sms_read_all = false;
                    snprintf(out, sizeof(out), "Unread: %d ID(s): %s", m, id_str);
                } else {
                    strncpy(out , "No new SMS messages", sizeof(out) - 1);
                }
                Command_SetDone(out);
                return;
            }
            // All msgs on sim
            else if (strncmp(in, "/sms ra", 7) == 0) {
                if (!Modem_SendAT ("AT+CMGL=\"ALL\"", tmp, sizeof(tmp), 5000)) {
                    Command_SetDone("Error: Failed to read SMS");
                    return;
                }
                // Grabs REC UNREAD and REC READ 
                m = GetSmsIndices(tmp, "ALL", ids, 10);
                SetSmsNumbers(id_str, ids, m);
                if (m > 0) {
                    sms_count = m; 
                    sms_unread_count = 0;
                    sms_read = true; 
                    sms_read_all = true;
                    snprintf(out, sizeof(out), "Stored: %d ID(s): %s", m, id_str);
                } else {
                    strncpy(out , "No messages stored", sizeof(out) - 1);
                }
                Command_SetDone(out);
                return;
            } else {
                Command_SetDone("Error: Unknown SMS command");
                return;
            }
        }
        // Grab number from send command
        else if (strncmp(in, "/sms s ", 7) == 0) {
            char new_sms_num[32] = {0};
            strncpy(new_sms_num, in + 7, sizeof(new_sms_num) - 1);
            new_sms_num[sizeof(new_sms_num)-1] = '\0';
            // Check valid number then set global number
            if (Sms_IsValidNumber(new_sms_num)) {
                snprintf(sms_number, sizeof(sms_number), "%s", new_sms_num);
                snprintf(out, sizeof(out), "Number set: %s", sms_number);
                sms_send = true;
            } else {
                strncpy(out, "Error: Invalid phone number", sizeof(out) - 1);
            }
            Command_SetDone(out);
            return;
        } 
        // Send mode without number but have previous number to send to
        else if (strcmp(in, "/sms s") == 0){
            if (sms_number[0] != '\0') {
                snprintf(out, sizeof(out), "Number set: %s", sms_number);
                sms_send = true;
            } else {
                strncpy(out, "Error: No number stored", sizeof(out) - 1);
            }
            Command_SetDone(out);
            return;
        } 
        
        Command_SetDone("SMS: /sms ra/ru/s <number>");
        return;
    }
    // Raw AT command - /AT <command> --> Modem_SendAT(<command>)
    // Modem task should dequeue and send command, then return response
    // Multiline AT commands from here not supported for now
    else if (strncmp(in, "/at", 3) == 0) {
        if (!modem_ready) {
            Command_SetDone("Error: Modem is not ready");
            return;
        }
         // Make sure GNSS is off first
        if (!GNSSOff()) return;
        // Make sure WiFi is off first
        if (!WiFiOff()) return;
        // QUICK COMMAND
        if (strlen(in) > 4 && in[3] == ' ') {
            char *at_cmd = in + 4;
            char at_resp[CMD_BUFFER_SIZE] = {0};
            if (!Modem_SendAT(at_cmd, at_resp, CMD_BUFFER_SIZE, 5000)){
                strncpy(out, "Error: AT failed or timed out", sizeof(out) - 1);
            } else {
                ReplaceControlChars(at_resp);
                strncpy(out, at_resp, sizeof(out) - 1);
            }
        }
        // ENTER AT MODE
        else if (strcmp(in, "/at") == 0) {
            at_mode = true;
            strncpy(out, "AT mode active", sizeof(out) - 1);
        }
        Command_SetDone(out);
        return;
    }
    // GNSS wizard entry or quick access
    else if (strcmp(in, "/gnss") == 0) {
        if (!modem_ready) {
            Command_SetDone("Error: Modem is not ready");
            return;
        }
        // Make sure WiFi is off first
        if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (wifi_data.wifi_on) {
                Command_SetDone("Error: Turn WiFi off first");
                xSemaphoreGive(wifi_data.mutex);
                return;
            }
            xSemaphoreGive(wifi_data.mutex);
        } else {
            Command_SetDone("Error: Cant take WiFi mutex");
            return;
        }

        gnss_mode = true;
        Command_SetDone("GNSS mode: on/off/info");
        return;
    }
    // WiFi wizard entry (not fully implemented)
    else if (strcmp(in, "/wifi") == 0) {
        if (!modem_ready) {
            Command_SetDone("Error: Modem is not ready");
            return;
        }
        // Prompt user to turn off passive modes that can run in background that can pull a lot of current
        // Not that it cant do it its just a lot of draw for PSU
        if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (gnss_data.gnss_on) {
                Command_SetDone("Error: Turn GNSS off first");
                xSemaphoreGive(gnss_data.mutex);
                return;
            }
            xSemaphoreGive(gnss_data.mutex);
        } else {
            Command_SetDone("Error: Cant take GNSS mutex");
            return;
        }
        wifi_mode = true;
        Command_SetDone("scan, connect, host, stop");
        return;
    } else {
        Command_SetDone("Error: Unknown command");
        return;
    }

    // Error was default, fallback if didnt exit early using Command_SetDone
    // Set cmd.buffer output and state
    Command_SetDone(out);
    return;
}   
