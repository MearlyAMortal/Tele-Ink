#include "Display.h"
#include "ESP32_WiFi.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t wifi_scan_task = NULL;
static TaskHandle_t wifi_host_task = NULL;
static uint32_t scan_interval = 60000;


// Scan for WiFi networks and print results to serial, also updates the home page with current scans
static void wifiScanTask(void *pv) {
    (void)pv;
    for (;;) {
        int n = WiFi.scanNetworks();
        printf("WiFi scan found %d networks\r\n", n);
        for (int i = 0; i < n; ++i) {
            printf("%d: %s (%d) %s\r\n", i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "");
        }
        WiFi.scanDelete();
        DEV_Delay_ms(scan_interval);
    }
}

// Starts the scanner task from command terminal
bool WiFi_StartScanner(uint32_t interval_ms) {
    scan_interval = interval_ms;
    if (wifi_scan_task) return true; // Already running
    return xTaskCreatePinnedToCore(wifiScanTask, "wifiScan", 4096, NULL, 2, &wifi_scan_task, 1) == pdPASS;
}

// stops the scanner task from command terminal returns true if task stopped or was not running
bool WiFi_StopScanner(void) {
    if (wifi_scan_task) {
        vTaskDelete(wifi_scan_task);
        wifi_scan_task = NULL;
        WiFi.scanDelete();
    }
    return (wifi_scan_task == NULL);
}

/*
// Connects to WiFi with given ssid and password, blocks until connected or timeout, returns true if connected, false if timeout or error
bool WiFi_ConnectBlocking(const char *ssid, const char *pass, uint32_t timeout_ms) {
    if (!ssid || !ssid[0]) return false;
    printf("WiFi: connecting to %s\r\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
        DEV_Delay_ms(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
        char ip_str[16];
        strncpy(ip_str, WiFi.localIP().toString().c_str(), sizeof(ip_str)-1);
        ip_str[sizeof(ip_str)-1] = '\0';
        printf("WiFi connected, LOCAL: %s\r\n", ip_str);
        Display_Event_WifiConnected();
        return true;
    }
    printf("WiFi connection failed\r\n");
    return false;
}

// Disconnects from WiFi if currently connected, returns true if disconnected, false if not connected or error
bool WiFi_Disconnect(void) {
    WiFi.disconnect();
    printf("WiFi disconnected\r\n");
}
*/


// Task to monitor hosting state and print connected clients every 8s
static void wifiHostTask(void *pv) {
    (void)pv;
    for (;;) {
        // Check for clients every 8s and update display
        uint8_t num_clients = WiFi.softAPgetStationNum();
        printf("WiFi hosting, connected clients: %d\r\n", num_clients);
        
        DEV_Delay_ms(30000);
    }
}

// Host wifi with given ssid and password and start task that will monitor clients
bool WiFi_StartHost(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return false;
    // Allready hosting and task running, just return true
    // JUST FOR SAFETY the command processer should not allow to be here
    if (wifi_host_task && ((WiFi.getMode() & WIFI_AP) != 0)) {
        printf("Already hosting WiFi\r\n");
        return true;
    }
    // Set wifi to ap mode
    if (!WiFi.mode(WIFI_AP)) {
        printf("Failed to set WiFi mode to AP\r\n");
        return false;
    }
    // Start hosting
    if (!WiFi.softAP(ssid, pass)) {
        printf("Failed to start WiFi AP\r\n");
        return false;
    }
    // Start task to monitor clients or disconnect ap if task fails to start
    if (xTaskCreatePinnedToCore(wifiHostTask, "wifiHost", 4096, NULL, 2, &wifi_host_task, 1) != pdPASS) {
        printf("Failed to create wifi host task\r\n");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }
    // Return true if everything started successfully
    return ((WiFi.getMode() & WIFI_AP) != 0) && (wifi_host_task != NULL);
}

// Stop hosting wifi and stop host task returns true if not hosting after call
bool WiFi_StopHost(void) {
    if (wifi_host_task) {
        vTaskDelete(wifi_host_task);
        wifi_host_task = NULL;
    } 
    // Explicity check task pointer
    const bool task_stopped = (wifi_host_task == NULL);
    const bool disconnect_called = WiFi.softAPdisconnect(true);
    const bool mode_off = WiFi.mode(WIFI_OFF);
    DEV_Delay_ms(100);
    const bool ap_stopped = ((WiFi.getMode() & WIFI_AP) == 0);
    // Make sure we return a realistic value so command can update internal state
    return task_stopped && disconnect_called && mode_off && ap_stopped;
}





