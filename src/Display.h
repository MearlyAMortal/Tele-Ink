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

typedef void (*PaintFn)(UBYTE *buf, UWORD size);

extern bool screen_on;
extern bool home_page;
extern bool command_page;
extern bool idle_page;


extern uint32_t command_page_lines;
extern char **command_page_data;
extern char *command_send;
extern char *command_resp;


void Display_Init(void);
void Display_Deinit(void);


typedef enum {
    DISP_EVT_NONE,
    DISP_EVT_SLEEP,
    DISP_EVT_SHOW_HOME, 
    DISP_EVT_SHOW_COMMAND,
    DISP_EVT_SHOW_IDLE,
    DISP_EVT_WIFI_ON,
    DISP_EVT_WIFI_CONNECTED,
    DISP_EVT_MODEM_POWERED,
    DISP_EVT_MODEM_READY,
    DISP_EVT_MODEM_NET,
    DISP_EVT_SMS_RECEIVED,
    DISP_EVT_RING,
    DISP_EVT_CUSTOM_MSG,
} DisplayEventType;

typedef struct {
    DisplayEventType type;
    const void *payload;
    //uint32_t hold_ms;
} DisplayEvent;

/* post an event to the display queue (returns true if queued) */


bool Display_PostEvent(const DisplayEvent *evt, TickType_t ticksToWait);
void Display_Event_ShowHome(void);
void Display_Event_ShowIdle(void);
void Display_Event_ShowCommand(void);
void Display_Event_WifiConnected(void);

void Display_SetIdleTimeout(uint32_t ms);






#endif // DISPLAY_H