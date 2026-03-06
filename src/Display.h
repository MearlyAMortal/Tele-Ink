/*****************************************************************************
* | File      	:   Display.h
* | Author      :   Logan Puntous
* | Function    :   Owns one reusable framebuffer,
*                   EPD power sequence, 
*                   display task and Display_PostEvent queue.
* | Info        :   Not Re-initilizing 4gray correctly ATM (no issues yet)
*----------------
* |	This version:   V0.0.1
* | Date        :   2025-12-18
* | Info        :
#
******************************************************************************/
#ifndef DISPLAY_H
#define DISPLAY_H
#include "GUI_Paint.h" //for paint_time
#include "DEV_Config.h"

// Canvas
typedef void (*PaintFn)(UBYTE *buf, UWORD size);

// Constructor / Destructor
void Display_Init(void);
void Display_Deinit(void);

// Page management
typedef enum {
    PAGE_NONE = 0,
    PAGE_BOOT,
    PAGE_HOME,
    PAGE_IDLE,
    PAGE_COMMAND,
} PageType;

extern PageType current_page;
extern PageType last_page;
extern bool screen_on;

// Display events
typedef enum {
    DISP_EVT_NONE = 0,
    DISP_EVT_WAKE, // Screen major updates
    DISP_EVT_SLEEP,
    DISP_EVT_SHOW_HOME, 
    DISP_EVT_SHOW_COMMAND,
    DISP_EVT_SHOW_IDLE,
    DISP_EVT_MODEM_POWERED, // Modem
    DISP_EVT_MODEM_READY,
    DISP_EVT_MODEM_NET,
    DISP_EVT_MODEM_LOST,
    DISP_EVT_SMS_RECEIVED,
    DISP_EVT_RING,
    DISP_EVT_CUSTOM_MSG,
} DisplayEventType;

typedef struct {
    DisplayEventType type;
    const char *payload;
} DisplayEvent;

// Command buffer for display
#define CMD_BUFFER_SIZE 256
#define CMD_HISTORY_LINES 10
// IDLE(display) -> TYPING(keyboard) -> PROCESSING(command) -> DONE(command) -> IDLE(display) 
typedef enum {
    CMD_STATE_IDLE = 0,
    CMD_STATE_TYPING,
    CMD_STATE_PROCESSING,
    CMD_STATE_DONE,
} CommandState;

typedef struct {
    char input[CMD_BUFFER_SIZE];
    char output[CMD_BUFFER_SIZE];
    char history[CMD_HISTORY_LINES][CMD_BUFFER_SIZE];
    int history_count;
    CommandState state;
    SemaphoreHandle_t mutex;
} CommandBuffer;

extern CommandBuffer cmd_buffer;

// modem
extern bool modem_ready; // Modem is ready to recive data
extern bool modem_net; // Modem has proved that it can respond and network status home or roaming (not literally on the network, this naming convention can be confusing)
extern bool modem_powered; 
extern uint8_t modem_mode;
extern int sms_count; 
// Internal mode state for Keyboard / command / modem
extern bool sms_send;
extern bool sms_read;
extern bool sms_read_all;
extern bool at_mode;
extern bool gnss_mode;
extern bool wifi_mode;

// Wifi
typedef struct {
    bool wifi_on;
    bool wifi_scan;
    bool wifi_connected;
    char ssid[32];
    char password[64];
    bool wifi_host;
    // Add hostname etc for hosting capabilities in the future
    SemaphoreHandle_t mutex;
} WifiData;
extern WifiData wifi_data;

// GNSS
typedef struct {
    bool gnss_on;
    char speed[8];    //KN
    char altitude[8]; //M
    char date[12];    //DDMMYY
    char time[10];    //HHMMSS
    double latitude;  //DD
    double longitude; //DD
    SemaphoreHandle_t mutex;
} GNSSData;
extern GNSSData gnss_data;

// Network signal data
typedef struct {
    uint8_t rxlev; // 0-63, 99=unknown 2G
    uint8_t ber;    // 0-7, 99=unknown 
    uint8_t rscp;  // 0-96, 255=unknown 3G
    uint8_t ecno;  // 0-49, 255=unknown
    uint8_t rsrq;  // 0-34, 255=unknown 4GLTE
    uint8_t rsrp;  // 0-97, 255=unknown
    SemaphoreHandle_t mutex;
} SignalData;
extern SignalData signal_data;


// Post event to display task
bool Display_PostEvent(const DisplayEvent *evt, TickType_t ticksToWait);
void Display_Event_Wake(void);
void Display_Event_Sleep(void);
void Display_Event_ShowHome(void);
void Display_Event_ShowIdle(void);
void Display_Event_ShowCommand(void);
// Update internal display ds
void Display_ClearCommandHistory(void);
void SetLastActivityTick(void);
void SignalData_Reset(void);
void ResetGlobalModeState(void);



#endif // DISPLAY_H