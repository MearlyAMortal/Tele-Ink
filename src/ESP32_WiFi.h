/*****************************************************************************
* | File      	:   Wifi.h
* | Author      :   Logan Puntous
* | Function    :   Multiple functionality from public calls.
* |                 Can create WifiTask for scanning
* | Info        :
*----------------
* |	This version:   V0.0.1
* | Date        :   2025-12-18
* | Info        :
#
******************************************************************************/
#ifndef ESP32_WIFI_H
#define ESP32_WIFI_H

#include "DEV_Config.h"

void WiFi_StartScanner(uint32_t interval_ms = 60000);
bool WiFi_ConnectBlocking(const char *ssid, const char *pass, uint32_t timeout_ms = 15000);
void WiFi_StopScanner(void);



#endif // ESP32_WIFI_H