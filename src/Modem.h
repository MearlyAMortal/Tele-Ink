/*****************************************************************************
* | File      	:   Modem.h
* | Author      :   Logan Puntous
* | Function    :   owns UART to the SIM7600G-H,
*                   a background task reading URCs,
*                   and a command queue for sending AT commands and receiving responses.
* | Info        :
*----------------
* |	This version:   V0.0.1
* | Date        :   2025-12-18
* | Info        :
#
******************************************************************************/



#ifndef MODEM_H
#define MODEM_H

#include "DEV_Config.h"
#include "Display.h"

typedef struct ModemCmd {
    bool noTx;
    char match_prefix[16]; // If set, response must contain this to be considered valid
    char cmd[256];
    char resp[2048];
    uint32_t timeout_ms;
    SemaphoreHandle_t done_sem;
    TickType_t start_tick;
} ModemCmd;

// HTTP request/response structs and enums
enum ModemHttpMethod {
    MODEM_HTTP_GET,
    MODEM_HTTP_POST
};

struct ModemHttpRequest {
    ModemHttpMethod method;
    const char *url;
    const char *body;
};

struct ModemHttpResponse {
    int status_code;
    char body[2048];
};




// Public
bool Modem_Init(HardwareSerial *serial, int rxPin, int txPin, int powerPin);
void Modem_Restart(void);
bool Modem_Status(void);
void Modem_TogglePWK(uint32_t duration_ms);
bool Modem_SetCheckMode(uint8_t mode);
bool Modem_SendAT(const char *cmd, const char *prefix, char *resp, size_t resp_len, uint32_t timeout_ms);
bool Modem_SendSMS(const char* number, const char *message, uint32_t timeout_ms);
bool Modem_SendHttpRequest(const ModemHttpRequest *request, ModemHttpResponse *response);


void GNSS_ToOneLinerAndUpdate(const char *input, char *output, size_t out_size);
void ReplaceControlChars(char* s);

#endif // MODEM_H
