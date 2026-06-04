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
#include "GUI_Paint.h"
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
    PAGE_DYNAMIC_WINDOW,
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
    DISP_EVT_SHOW_DYNAMIC_WINDOW,
    DISP_EVT_MODEM_STATE_CHANGED,
    DISP_EVT_SMS_RECEIVED,
} DisplayEventType;

typedef struct {
    DisplayEventType type;
    const char *payload;
} DisplayEvent;


// Command buffer for display
#define CMD_BUFFER_SIZE 256
#define CMD_HISTORY_LINES 16
#define CMD_INPUT_HISTORY_LINES 16
// IDLE(display)(init) => TYPING(keyboard) -> PROCESSING(command calling outside) -> DONE(command) -> IDLE(keyboard) -> TYPING(keyboard)
typedef enum {
    CMD_STATE_IDLE = 0,
    CMD_STATE_TYPING,
    CMD_STATE_PROCESSING,
    CMD_STATE_DONE,
} CommandState;

typedef struct {
    char input[CMD_BUFFER_SIZE];
    char output[CMD_BUFFER_SIZE];
    // For displaying past input/output
    char history[CMD_HISTORY_LINES][CMD_BUFFER_SIZE];
    int history_count;
    // For arrow key movement in command menu
    char input_history[CMD_INPUT_HISTORY_LINES][CMD_BUFFER_SIZE];
    int input_history_count;
    CommandState state;
    SemaphoreHandle_t mutex;
} CommandBuffer;

extern CommandBuffer cmd_buffer;

// modem
extern uint8_t modem_mode; // add to modem state?
extern int sms_count; 
extern int sms_unread_count; // Actual new incoming message count via URC
extern int sms_ids[10];
// Internal mode state for Keyboard / command / modem
extern bool sms_send;
extern bool sms_read;
extern bool sms_read_all;
extern bool at_mode;
extern bool gnss_mode;
extern int gnss_update_count;
extern bool http_mode;

// Selectable/Scaleable/Programmable poll rate for each type of data collection. Changed in command.cpp /pr x 1=low, 2=med, 3=high from each respective polling mode wizard
typedef enum {
    POLL_RATE_LOW = 1,
    POLL_RATE_MEDIUM,
    POLL_RATE_HIGH,
} PollRate;

extern bool polling_rate_changed;

// Modem state data holding data collected from background modem task
typedef struct {
    int capability; // 0-3 for (dead, alive, registered, pdp)
    PollRate poll_rate;
    SemaphoreHandle_t mutex;
} ModemState;
extern ModemState modem_state;
 
// GNSS
typedef struct {
    bool gnss_on;
    char speed[8];      //KN
    char altitude[8];   //M
    char date[12];      //DDMMYY
    char time[10];      //HHMMSS
    double latitude;    //DD
    double longitude;   //DD
    PollRate poll_rate;
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
    PollRate poll_rate;
    SemaphoreHandle_t mutex;
} SignalData;
extern SignalData signal_data;


// Post event to display task
bool Display_PostEvent(const DisplayEvent *evt, TickType_t ticksToWait);
void Display_Event_Wake(void);
void Display_Event_Sleep(void);
void Display_Event_ModemStateChanged(void);
void Display_Event_ShowHome(void);
void Display_Event_ShowIdle(void);
void Display_Event_ShowCommand(void);
void Display_Event_ShowDynamicWindow(void);
// Public functions for display to call or from other files
void Display_ClearCommandHistory(void);
void SetLastActivityTick(void);
void ModemState_Reset(void);
void SignalData_Reset(void);
void GnssData_Reset(void);
void ResetGlobalModeState(void);
bool ChangePollingRate(bool status, bool signal, bool gnss, PollRate new_rate);
int GetCurrentModemState(void);
CommandState GetCurrentCommandState(void);



#endif // DISPLAY_H