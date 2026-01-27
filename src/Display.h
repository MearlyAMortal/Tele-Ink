/*****************************************************************************
* | File      	:   Display.h
* | Author      :   Logan Puntous
* | Function    :   Owns one reusable framebuffer,
*                   EPD power sequence, 
*                   display task and Display_PostEvent queue.
* | Info        :
*----------------
* |	This version:   V0.0.1
* | Date        :   2025-12-18
* | Info        :
#
******************************************************************************/
#ifndef DISPLAY_H
#define DISPLAY_H

#include "DEV_Config.h"

// Canvas
typedef void (*PaintFn)(UBYTE *buf, UWORD size);

// Constructor / Destructor
void Display_Init(void);
void Display_Deinit(void);

// Page management
typedef enum {
    PAGE_NONE = 0,
    PAGE_HOME,
    PAGE_IDLE,
    PAGE_COMMAND,
} PageType;

extern PageType current_page;
extern PageType last_page;
extern bool screen_on;

// Display events
typedef enum {
    DISP_EVT_NONE,
    DISP_EVT_SLEEP, // Screen major updates
    DISP_EVT_SHOW_HOME, 
    DISP_EVT_SHOW_COMMAND,
    DISP_EVT_SHOW_IDLE,
    DISP_EVT_WIFI_ON, // Wifi
    DISP_EVT_WIFI_CONNECTED,
    DISP_EVT_MODEM_POWERED, // Modem
    DISP_EVT_MODEM_READY,
    DISP_EVT_MODEM_NET,
    DISP_EVT_SMS_RECEIVED,
    DISP_EVT_RING,
    //DISP_EVT_UPDATE_COMMAND, // KB
    //DISP_EVT_DONE_COMMAND,
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
    SemaphoreHandle_t mutex; // Protects access to input/output buffers
} CommandBuffer;

extern CommandBuffer cmd_buffer;

// Post event to display task
bool Display_PostEvent(const DisplayEvent *evt, TickType_t ticksToWait);
void Display_Event_Sleep(void);
void Display_Event_ShowHome(void);
void Display_Event_ShowIdle(void);
void Display_Event_ShowCommand(void);
void Display_Event_WifiConnected(void);
// Update internal display ds
void Display_ClearCommandHistory(void);
//void Display_Event_UpdateLineBuffer(void);
//void Display_Event_DoneCommand(void);


#endif // DISPLAY_H