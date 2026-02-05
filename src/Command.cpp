#include "Command.h"
// For public queue/buffer
#include "Display.h"
#include "Modem.h"
// FD
#include <string.h>


static void Command_HelpPage(char *r);



// Places help info in response
static void Command_HelpPage(char *r) {
    if (!r) return;
    r[0] = '\0';
    strcat(r, "Available commands:\n");
    strcat(r, "/help                 - show this message\n");
    strcat(r, "/clear                - clear command history\n");
    strcat(r, "/status               - show system status\n");
    strcat(r, "/AT                   - send AT command(!) ex. AT+CREG?\n");
    strcat(r, "/gnss                 - get GNSS data\n");
    strcat(r, "/wifiscan             - show nearby wifi SSID\n");
    strcat(r, "/wificonnect [ssid]   - prompt password and connect\n");
}



// Interprets keyboard input and responds with an error or display/modem/etc event. (Protected)
void Command_Handle(void){
    if (!cmd_buffer.input) return;
    char in[CMD_BUFFER_SIZE] = {0};
    // Copy input command while holding mutex briefly
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cmd_buffer.state = CMD_STATE_PROCESSING;
        strcpy(in, cmd_buffer.input);
        xSemaphoreGive(cmd_buffer.mutex);
    }

    // Not command, echo message
    if (in[0] != '/'){
        if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            strcpy(cmd_buffer.output, in);
            cmd_buffer.state = CMD_STATE_DONE;
            xSemaphoreGive(cmd_buffer.mutex);
        }
        return;
    }
    // Default command error for now
    char out[CMD_BUFFER_SIZE] = {0};
    strcpy(out, "Error: Unknown command");

    // Help menu
    if (strcmp(in, "/help") == 0) {
        Command_HelpPage(out);
    } 
    // Clear history
    else if (strcmp(in, "/clear") == 0) {
        Display_ClearCommandHistory();
        strcpy(out, "History cleared");
    }
    // ESP control
    else if (strncmp(in, "/esp", 4) == 0) {
        if (strcmp(in, "/esp pwk") == 0) {
            ESP.restart();
        }
    }
    // Modem control
    else if (strncmp(in, "/sim", 4) == 0) {
        if (strncmp(in, "/sim pwk1", 9) == 0) {  
            Modem_TogglePWK(1200);
            strcpy(out, "Toggled pwk on modem for 1.2s");
        } else if (strncmp(in, "/sim pwk2", 9) == 0) {  
            Modem_TogglePWK(2500);
            strcpy(out, "Toggled pwk on modem for 2s");
        } else {
            strcpy(out, "Error: Modem command unrecognized");
        }
    }
    // Raw AT command - /AT <command> --> Modem_SendAT(<command>)
    // Modem task should dequeue and send command, then return response
    else if (strncmp(in, "/at", 3) == 0) {
        if (strlen(in) > 4 && in[3] == ' ') {
            char *at_cmd = in + 4;
            char at_resp[CMD_BUFFER_SIZE] = {0};
            if (!Modem_SendAT(at_cmd, at_resp, CMD_BUFFER_SIZE, 5000)){
                strcpy(out, "Error: AT command failed or timed out");
            } else {
                strcpy(out, at_resp);
            }
        }
    }
    
    // Set cmd.buffer output and state
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strcpy(cmd_buffer.output, out);
        cmd_buffer.state = CMD_STATE_DONE;
        xSemaphoreGive(cmd_buffer.mutex);
    }
  
    //printf("Command handled: %s -> %s\r\n", in, cmd_buffer.output);
    return;
}   
