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
    bool waitForOK;
    bool noTx;
    char cmd[256];
    char resp[1024];
    uint32_t timeout_ms;
    SemaphoreHandle_t done_sem;
    TickType_t start_tick;
} ModemCmd;


// Public
bool Modem_Init(HardwareSerial *serial, int rxPin, int txPin, int powerPin);
bool Modem_SendAT(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);
void Modem_TogglePWK(uint32_t duration_ms);
bool Modem_SendSMS(const char *message, const char* number, uint32_t timeout_ms);

// Private

/*
bool Modem_WriteRaw(const uint8_t *data, size_t len, uint32_t timeoutMs = 1000);
bool Modem_SendAT(const char *cmd, char *resp, size_t respLen, uint32_t timeoutMs);

bool Modem_AT(void);
bool Modem_CheckNetwork(void);
bool Modem_GetGNSSRaw(char *resp, size_t respLen, uint32_t timeoutMs);
bool Modem_GetGNSSutc(char *out, size_t outLen, uint32_t timeoutMs);
*/
#endif // MODEM_H
