#include "Command.h"
// For public queue/buffer
#include "Display.h"
#include "Modem.h"
#include <stdio.h>
// FD
#include <string.h>

static char sms_number[32] = {0};
static bool sms_read_all = false;


// Quick way to exit the if else tree if condition is not met
// Takes a string for the output command (usually an error)
static void Command_SetDone(const char* out){
    if (!out) return;
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strcpy(cmd_buffer.output, out);
        cmd_buffer.state = CMD_STATE_DONE;
        xSemaphoreGive(cmd_buffer.mutex);
    }
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

// Removes anything not ASCII and makes it a space
static void ReplaceControlChars(char* s) {
    if (!s) return;
    for (char *c = s; *c; c++) {
        if (*c < 0x20 || *c >= 0x7F) {
            *c = ' ';
        }
    }
}


// Returns true if 10 digit number (can include +CC)
static bool Sms_IsValidNumber(const char* s) {
    if (!s || *s == '\0') return false;
    int i = 0;
    if (s[i] == '+') i++;
    int digits = 0;
    for (; s[i]; ++i) {
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

// Helper for parsing CMGS response
static int CountSmsMessages(const char* resp) {
    if (!resp) return 0;
    int count = 0;
    const char* p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL) {
        count++;
        p += 6;
    }
    return count;
}

// Collect unread sms message idxs for later retrieval
static int GetUnreadSmsIndices(const char* resp, int* ids, int max_ids) {
    int count = 0;
    const char* p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL && count < max_ids) {
        const char* status = strchr(p, ',');
        if (status && strstr(status, "\"REC UNREAD\"")) {
            p += 6;
            while (*p == ' ') p++;
            int idx = atoi(p);
            ids[count++] = idx;
        }
        p = strchr(p, '\n');
        if (!p) break;
    }
    return count;
}

// make printable id response for display
static void SetUnreadSmsNumbers(char* id_str, const int* ids, int um) {
    for (int i = 0; i < um; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", ids[i] + 1);
        strcat(id_str, buf);
        if (i < um - 1) {
            strcat(id_str, ",");
        }
    }
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


// Converts GNSS input to a one-line summary
void GNSS_ToOneLiner(const char *input, char *output, size_t out_size) {
    char lat[16], ns, lon[16], ew, date[8], time[10], alt[8], spd[8];
    // Parse the fields
    int n = sscanf(input, "%15[^,],%c,%15[^,],%c,%6[^,],%9[^,],%7[^,],%7[^,]",
        lat, &ns, lon, &ew, date, time, alt, spd);
    if (n < 8) {
        snprintf(output, out_size, "Error: Failed to fix or parse");
        return;
    }
    double lat_dd = nmea_to_decimal(lat, ns);
    double lon_dd = nmea_to_decimal(lon, ew);

    // Format date and time
    char date_fmt[16], time_fmt[16];
    snprintf(date_fmt, sizeof(date_fmt), "20%c%c-%c%c-%c%c", date[4], date[5], date[2], date[3], date[0], date[1]);
    snprintf(time_fmt, sizeof(time_fmt), "%c%c:%c%c:%c%c", time[0], time[1], time[2], time[3], time[4], time[5]);

    // Output one-liner
    snprintf(output, out_size,
        "La: %.6f Lo: %.6f UTC: %s Date: %s Alt: %sm Spd: %skn",
        lat_dd, lon_dd, time_fmt, date_fmt, alt, spd);
}



// Interprets keyboard input and responds with an error or display/modem/etc event.
void Command_Handle(void){
    if (!cmd_buffer.input) return;

    char in[CMD_BUFFER_SIZE] = {0};
    // Copy input command while holding mutex briefly
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cmd_buffer.state = CMD_STATE_PROCESSING;
        strcpy(in, cmd_buffer.input);
        xSemaphoreGive(cmd_buffer.mutex);
    }

    TrimRight(in);

    // WIZARDS
    // Check multi-line command mode first before handling base case
    // Send sms once collected number and message
    if (sms_send) {
        if (strcmp(in, "/exit") == 0) {
            Command_SetDone("Exiting SMS send");
            sms_send = false;
            return;
        }
        
        // We have number and message here
        if (Modem_SendSMS(sms_number, in, 45000)){
            Command_SetDone("Message sent!");
            sms_send = false;
        } else {
            Command_SetDone("Error: SMS failed to send");
            sms_send = false;
        }
        return;
    } 
    // Read messages based on index (ru consumes messages, can also delete by index or all)
    else if (sms_read) {
        if (sms_count <= 0 && strcmp(in, "/s") != 0) {
            Command_SetDone("Exiting: No SMS to read");
            sms_read = false;
            sms_count = 0;
            sms_read_all = false;
            return;
        }
        if (strncmp(in, "/exit", 5) == 0) {
            Command_SetDone("Exiting SMS read");
            sms_read = false;
            if (sms_read_all){
                sms_count = 0;
                sms_read_all = false;
            }
            return;
        } 

        // Responding to immediate previously read message with valid sms_number
        if (strcmp(in, "/s") == 0 && sms_number[0] != '\0') {
            sms_send = true;
            char buf[32];
            snprintf(buf, sizeof(buf), "Responding to: %s", sms_number); // Ensure null termination
            Command_SetDone(buf);
            return;
        }
        char cmd[32] = {0};
        char tmp[512] = {0};
        // Deleting message
        if (strncmp(in, "/d", 2) == 0) {
            // Delete all msgs on sim
            if (strncmp(in, "/da", 3) == 0) {
                Modem_SendAT ("AT+CMGD=1,4", tmp, sizeof(tmp), 5000);
                if (strstr(tmp, "OK")) {
                    sms_count = 0; 
                    sms_read = false; 
                    sms_read_all = false;
                    Command_SetDone("Successfully deleted all SMS");
                } else {
                    Command_SetDone("Error: Failed to delete all SMS");
                }
                return;
            }
            // Delete message by idx
            else if (strncmp(in, "/d ", 3) == 0) {
                int idd = ValidateID(in + 3);
                if (idd < 0 || idd >= sms_count) {
                    Command_SetDone("Error: Invalid index");
                    return;
                }
                // Send delete AT
                snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", idd);
                Modem_SendAT(cmd, tmp, sizeof(tmp), 5000);
                if (strstr(tmp, "OK")) {
                    --sms_count;
                    Command_SetDone("Successfully deleted index");
                } else {
                    Command_SetDone("Error: Failed to delete");
                }
                return;
            }
        }
        // Reading message by idx
        int idr = ValidateID(in);
        if (idr >= 0) {
            // Send read AT and fix response and sms_count
            snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idr);
            Modem_SendAT(cmd, tmp, sizeof(tmp), 2000);
            if (!sms_read_all) --sms_count;
            ReplaceControlChars(tmp);
            // Get phone number saved for response and only display message body to user
            char *num_start = strchr(tmp, '"');
            char number[32] = {0};
            int quote_count = 0;
            const char *p = tmp;
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
            
            //TrimRight(number);
            if (Sms_IsValidNumber(number)) {
                printf("Saving number: %s\r\n", number);
                sms_number[0] = '\0';
                strncpy(sms_number, number, sizeof(sms_number) - 1);
                sms_number[sizeof(sms_number) - 1] = '\0';
            } 
            // Set output to message body only
            // Skip the header
            char *data = tmp + strlen("AT+CMGR=X +GMGR: ");
            while (*data == ' ') data++;
            Command_SetDone(data);
        } else {
            Command_SetDone("Error: Invalid message number");
        }   
        return;
    }
    else if (at_mode) {
        if (strcmp(in, "/exit") == 0) {
            Command_SetDone("Exiting AT mode");
            at_mode = false;
            return;
        } 
        char at_cmd[CMD_BUFFER_SIZE] = {0};
        // Parse for AT else repent AT into command to correspond w display
        snprintf(at_cmd, sizeof(at_cmd), "AT%s", in);

        char at_resp[CMD_BUFFER_SIZE] = {0};
        if (!Modem_SendAT(at_cmd, at_resp, CMD_BUFFER_SIZE, 5000)){
            Command_SetDone("Error: AT failed or timed out");
        } else {
            ReplaceControlChars(at_resp);
            Command_SetDone(at_resp);
        }
        return;
    }
    else if (gnss_mode) {
        if (strcmp(in, "/exit") == 0) {
            Command_SetDone("Exiting GNSS mode");
            gnss_mode = false;
            return;
        }  
        char gnss_info[512] = {0};
        gnss_info[0] = '\0';
        if (strncmp(in, "on", 2) == 0) {
            if (!Modem_SendAT("AT+CGPS=1", gnss_info, sizeof(gnss_info), 5000)){
                Command_SetDone("Error: Failed to turn on GNSS");
            } else {
                ReplaceControlChars(gnss_info);
                Command_SetDone("GNSS turned on wait for fix");
                gnss_on = true;
            }
        } 
        else if (strncmp(in, "off", 3) == 0) {
            if (!Modem_SendAT("AT+CGPS=0", gnss_info, sizeof(gnss_info), 5000)){
                Command_SetDone("Error: Failed to turn off GNSS");
            } else {
                ReplaceControlChars(gnss_info);
                Command_SetDone("GNSS turned off");
                gnss_on = false;
            }
        }
        else if (strncmp(in, "info", 4) == 0) {
            if (!gnss_on) {
                Command_SetDone("Error: GNSS is not on");
                return;
            }
            if (!Modem_SendAT("AT+CGPSINFO", gnss_info, sizeof(gnss_info), 5000)){
                Command_SetDone("Error: Failed to get GNSS");
            } else {
                ReplaceControlChars(gnss_info);
                char *data = gnss_info + strlen("AT+CGPSINFO +CGPSINFO: ");
                while (*data == ' ') data++;

                GNSS_ToOneLiner(data, gnss_info, sizeof(gnss_info));
                Command_SetDone(gnss_info);
            }
        }
        else {
            Command_SetDone("Error: Unknown GNSS command");
        }
        return;
    }


    // PARSE default $ INPUT
    // Not command, echo message
    if (in[0] != '/'){
        Command_SetDone(in);
        return;
    }
    // Default command error for now
    char out[CMD_BUFFER_SIZE] = {0};
    strncpy(out, "Error: Unknown command", sizeof(out)-1);

    // If else tree of doom that can select mode or exectute specific commands
    // Help menu
    if (strcmp(in, "/help") == 0) {
        strcpy(out, "Check GitHub README");
    } 
    // Clear history
    else if (strcmp(in, "/clear") == 0) {
        Display_ClearCommandHistory();
        strcpy(out, "History cleared");
    }
    // ESP control
    else if (strncmp(in, "/esp", 4) == 0) {
        if (strcmp(in, "/esp rst") == 0) {
            Modem_TogglePWK(3000);
            DEV_Delay_ms(8000);
            ESP.restart();
        } else {
            strcpy(out, "Error: ESP command unrecognized");
        }
    }
    // Modem external control
    else if (strncmp(in, "/sim", 4) == 0) {
        if (strncmp(in, "/sim on", 7) == 0) {  
            Modem_TogglePWK(1200);
            strcpy(out, "Toggled pwk on modem for on");
        } else if (strncmp(in, "/sim off", 8) == 0) {  
            Modem_TogglePWK(3000);
            strcpy(out, "Toggled pwk on modem for off");
        } else if (strncmp(in, "/sim net", 8) == 0 && modem_ready) {
            char tmp[256] = {0};
            Modem_SendAT("AT+CREG?", tmp, sizeof(tmp), 5000);
            strcpy(out, tmp);
        } else {
            strcpy(out, "Error: Modem command unrecognized");
        }
    }
    // Start modem sms wizard /sms <number> 
    else if (strncmp(in, "/sms", 4) == 0) {
        if (!modem_ready){
            Command_SetDone("Error: Modem is not ready");
            return;
        }
        // Enable text mode
        char tmp[512] = {0};
        if (!Modem_SetCheckMode(1)){
            Command_SetDone("Error: Text mode not enabled");
            return;
        } 
        tmp[0] = '\0';
        int m = 0; //msgs
        int um = 0; //unread msgs
        // Reading recivied messages
        if (strncmp(in, "/sms r", 6) == 0) {
            // All msgs on sim
            if (strncmp(in, "/sms ra", 7) == 0) {
                Modem_SendAT ("AT+CMGL=\"ALL\"", tmp, sizeof(tmp), 5000);
                m = CountSmsMessages(tmp);
                snprintf(out, sizeof(out), "Total SMS stored on sim: %d", m);
                if (m > 0) {
                    sms_count = m; 
                    sms_read = true; 
                    sms_read_all = true;
                }
                Command_SetDone(out);
                return;
            } 
            // Unread msgs
            else if (strncmp(in, "/sms ru", 7) == 0) {
                Modem_SendAT ("AT+CMGL=\"REC UNREAD\"", tmp, sizeof(tmp), 5000);
                int ids[10];
                um = GetUnreadSmsIndices(tmp, ids, 10);
                char id_str[64] = {0};
                SetUnreadSmsNumbers(id_str, ids, um);
                if (um > 0) {
                    sms_count = um; 
                    sms_read = true; 
                    sms_read_all = false;
                    snprintf(out, sizeof(out), "Unread: %d ID(s): %s", um, id_str);
                } else {
                    strcpy(out , "No new SMS messages");
                }
                Command_SetDone(out);
                return;
            }
        }   
        // Grab number from send command
        else if (strncmp(in, "/sms s ", 7) == 0) {
            strncpy(sms_number, in + 7, sizeof(sms_number)-1);
            sms_number[sizeof(sms_number)-1] = '\0';
            // Keep only first token (stop at whitespace)
            char *sp = strpbrk(sms_number, " \t");
            if (sp) *sp = '\0';
            // Check valid number
            if (Sms_IsValidNumber(sms_number)) {
                snprintf(out, sizeof(out), "Number set: %s", sms_number);
                sms_send = true;
            } 
        } 
        // Use last valid number if there is one
        else if (sms_number[0] != '\0'){
            snprintf(out, sizeof(out), "Number set: %s", sms_number);
            sms_send = true;
        } 
        if (!sms_send && !sms_read) strncpy(out, "Usage: /sms s/ra/ru <number> ", sizeof(out)-1);
    }
    // Raw AT command - /AT <command> --> Modem_SendAT(<command>)
    // Modem task should dequeue and send command, then return response
    // Multiline AT commands from here not supported for now
    else if (strncmp(in, "/at", 3) == 0) {
        if (!modem_ready) {
            Command_SetDone("Error: Modem is not ready");
            return;
        }
        // QUICK COMMAND
        if (strlen(in) > 4 && in[3] == ' ') {
            char *at_cmd = in + 4;
            char at_resp[CMD_BUFFER_SIZE] = {0};
            if (!Modem_SendAT(at_cmd, at_resp, CMD_BUFFER_SIZE, 5000)){
                strcpy(out, "Error: AT command failed or timed out");
            } else {
                ReplaceControlChars(at_resp);
                strcpy(out, at_resp);
            }
        }
        // ENTER AT MODE
        else if (strcmp(in, "/at") == 0) {
            at_mode = true;
            strcpy(out, "AT mode active");
        }
    }
    // GNSS easy
    else if (strcmp(in, "/gnss") == 0) {
        if (!modem_ready) {
            Command_SetDone("Error: Modem is not ready");
            return;
        }
        gnss_mode = true;
        strcpy(out, "GNSS mode activated use: 'on'");
    }
    
    
    // Set cmd.buffer output and state
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strcpy(cmd_buffer.output, out);
        cmd_buffer.state = CMD_STATE_DONE;
        xSemaphoreGive(cmd_buffer.mutex);
    }
  
    return;
}   
