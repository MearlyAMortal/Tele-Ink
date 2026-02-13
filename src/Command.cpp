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

static int ValidateID(const char* str){
    if (!str || strlen(str) == 0) return -1;
    
    // Check all digits
    for (int i = 0; str[i]; i++) {
        if (str[i] < '0' || str[i] > '9') return -1;
    }
    
    int idx = atoi(str);
    return idx;
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

    // Check multi-line command mode first before handling base case
    // Send sms once collected number and message
    if (sms_send) {
        if (strcmp(in, "/exit") == 0) {
            Command_SetDone("Exiting SMS send");
            sms_send = false;
            sms_number[0] = '\0';
            return;
        }
        // We have number and message here
        if (Modem_SendSMS(sms_number, in, 45000)){
            Command_SetDone("Message sent");
            sms_send = false;
        } else {
            Command_SetDone("Message failed to send");
            sms_number[0] = '\0';
            sms_send = false;
        }
        return;
    } 
    // Read messages based on index
    else if (sms_read) {
        if (strncmp(in, "/exit", 5) == 0 ) {
            Command_SetDone("Exiting SMS read");
            sms_read = false;
            sms_count = 0;
            sms_read_all = false;
            return;
        } 
        char cmd[32] = {0};
        char tmp[256] = {0};

        // Deleting message
        if (strncmp(in, "/d ", 3) == 0) {
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
                Command_SetDone("Deleted message");
            } else {
                Command_SetDone("Error: Failed to delete");
            }
        }
        // Reading message by idx
        int idr = ValidateID(in);
        if (idr >= 0) {
            snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idr);
            Modem_SendAT(cmd, tmp, sizeof(tmp), 2000);
            ReplaceControlChars(tmp);
            Command_SetDone(tmp);
            if (!sms_read_all) --sms_count;
        } else {
            Command_SetDone("Error: Invalid MSG ID provided");
        }   
        return;
    }
    else if (at_mode) {
        if (strcmp(in, "/exit") == 0) {
            Command_SetDone("Exiting AT");
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
            ESP.restart();
        } else {
            strcpy(out, "Error: ESP command unrecognized - Did you mean /esp rst?");
        }
    }
    // Modem external control
    else if (strncmp(in, "/sim", 4) == 0) {
        if (strncmp(in, "/sim on", 7) == 0) {  
            Modem_TogglePWK(1200);
            strcpy(out, "Toggled pwk on modem for 1200ms");
        } else if (strncmp(in, "/sim off", 8) == 0) {  
            Modem_TogglePWK(3000);
            strcpy(out, "Toggled pwk on modem for 3000ms");
        } else if (strncmp(in, "/sim net", 8) == 0 && modem_ready) {
            char tmp[256] = {0};
            Modem_SendAT("AT+CREG?", tmp, sizeof(tmp), 5000);
            strcpy(out, "Checking network...");
        } else {
            strcpy(out, "Error: Modem command unrecognized");
        }
    }
    // Modem sms wizard /sms <number> 
    else if (strncmp(in, "/sms", 4) == 0) {
        if (!modem_ready){
            Command_SetDone("Error: Modem is not ready");
            return;
        }
        // Enable text mode safety
        char tmp[512] = {0};
        if (!Modem_SendAT("AT+CMGF=1", tmp, sizeof(tmp), 2000)) {
            Command_SetDone("Error: Text mode not enabled");
            return;
        }
        tmp[0] = '\0';
        int m = 0;
        // Reading recivied messages
        if (strncmp(in, "/sms r", 6) == 0) {
            // All msgs on sim
            if (strncmp(in, "/sms ra", 7) == 0) {
                Modem_SendAT ("AT+CMGL=\"ALL\"", tmp, sizeof(tmp), 5000);
                m = CountSmsMessages(tmp);
                snprintf(out, sizeof(out), "There are %d messages stored.", m);
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
                m = CountSmsMessages(tmp);
                snprintf(out, sizeof(out), "There are %d unread messages.", m);
                if (m > 0) {
                    sms_count = m; 
                    sms_read = true; 
                    sms_read_all = false;
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
    
    
    // Set cmd.buffer output and state
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strcpy(cmd_buffer.output, out);
        cmd_buffer.state = CMD_STATE_DONE;
        xSemaphoreGive(cmd_buffer.mutex);
    }
  
    return;
}   
