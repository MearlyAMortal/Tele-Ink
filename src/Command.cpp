#include "Command.h"
#include "Display.h"
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
    if (xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    cmd_buffer.state = CMD_STATE_PROCESSING;
    char in[CMD_BUFFER_SIZE] = {0};
    strcpy(in, cmd_buffer.input);

    printf("Command_Handle: %s\r\n", in);

    // Not command, echo message
    if (in[0] != '/'){
        strcpy(cmd_buffer.output, in);
        cmd_buffer.state = CMD_STATE_DONE;
        printf("Command handled: %s -> %s\r\n", in, cmd_buffer.output);
        xSemaphoreGive(cmd_buffer.mutex);
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

    // Output into buffer, state done then give mutex and post event for finished command
    strcpy(cmd_buffer.output, out);
    cmd_buffer.state = CMD_STATE_DONE;

    printf("Command handled: %s -> %s\r\n", in, cmd_buffer.output);
    xSemaphoreGive(cmd_buffer.mutex);
   //Display_Event_DoneCommand();
    return;
}   
