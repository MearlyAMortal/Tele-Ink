#include "Display.h"
#include "ESP32_WiFi.h"
//#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t wifi_scan_task = NULL;
static uint32_t scan_interval = 60000;

// Save upload speed?
/*
static void wifiScanTask(void *pv) {
    (void)pv;
    for (;;) {
        int n = WiFi.scanNetworks();
        Serial.printf("WiFi scan found %d networks\r\n", n);
        for (int i = 0; i < n; ++i) {
            Serial.printf("%d: %s (%d) %s\r\n", i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "");
            
        }
        WiFi.scanDelete();
        DEV_Delay_ms(scan_interval);
    }
}

void WiFi_StartScanner(uint32_t interval_ms) {
    scan_interval = interval_ms;
    if (wifi_scan_task == NULL) {
        xTaskCreatePinnedToCore(wifiScanTask, "wifiScan", 4096, NULL, 1, &wifi_scan_task, 1);
    }
}

void WiFi_StopScanner(void) {
    if (wifi_scan_task) {
        vTaskDelete(wifi_scan_task);
        wifi_scan_task = NULL;
    }
}

bool WiFi_ConnectBlocking(const char *ssid, const char *pass, uint32_t timeout_ms) {
    if (!ssid || !ssid[0]) return false;
    Serial.printf("WiFi: connecting to %s\r\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
        DEV_Delay_ms(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi connected, LOCAL: %s\r\n", WiFi.localIP().toString().c_str());
        Display_Event_WifiConnected();
        return true;
    }
    Serial.println("WiFi connect timeout/fail");
    return false;
}
    */