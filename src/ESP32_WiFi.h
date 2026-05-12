/*****************************************************************************
* | File      	:   Wifi.h
* | Author      :   Logan Puntous
* | Function    :   Multiple functionality from public calls.
* |                 Can create FreeRTOS WifiTask's for scanning and hosting, and can connect/disconnect from wifi networks.
* | Info        :   All public functions return bool so command processer can update internal state 
*----------------
* |	This version:   V0.0.1
* | Date        :   2026-3-14
* | Info        :
#
******************************************************************************/
#ifndef ESP32_WIFI_H
#define ESP32_WIFI_H

#include "DEV_Config.h"

bool WiFi_StartScanner(uint32_t interval_ms = 60000);
bool WiFi_StopScanner(void);
bool WiFi_ConnectBlocking(const char *ssid, const char *pass, uint32_t timeout_ms = 15000);
bool WiFi_Disconnect(void);
bool WiFi_StartHost(const char *ssid, const char *pass);
bool WiFi_StopHost(void);


#endif // ESP32_WIFI_H