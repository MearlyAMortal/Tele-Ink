#include "Display.h"
#include "EPD.h"
#include "GUI_Paint.h"
#include "ImageData.h"
// FD
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"


// Public
bool screen_on = false;

bool home_page = false;
bool command_page = false;
bool idle_page = false;


// Command page data
uint32_t command_page_lines = 0;
char **command_page_data = NULL;
char *command_send = NULL;
char *command_resp = NULL;


// Screen
static SemaphoreHandle_t epd_mutex = NULL;
static QueueHandle_t dispQueue = NULL;
static TaskHandle_t display_task_handle = NULL;
static UBYTE *reusable_buf = NULL;
static UWORD reusable_size = 0;
static uint8_t GRAY_MODE = 4;
static uint8_t SCALE = 4;
static uint32_t idle_timeout_ms = 60000; //One minute
static int idle_timeout_count = 0; 
static TickType_t last_activity_tick = 0;
static uint32_t idle_page_tick_count = 0;



// Modem
static bool modem_ready = false;
static bool modem_net = false;
static bool modem_powered = false;
static int unread_sms = 0;
static bool ringing = false;
static TickType_t end = 0;

// Wifi
static bool wifi_connected = false;
static bool wifi_on = false;

// FD
PageType current_page = PAGE_NONE;
static UWORD getImageSize(void);
static void paintHomeScreen(void);
static void paintCommandScreen(void);
static void paintBlankScreen(void);
static void paintBootScreen(void);
static void paintPhoneScreen(void);
static void Display_StartTask(void);
//static void powerOn(void);
//static void powerOff(void);
static void Display_HandleScreenEvent(void);
static void displayTask(void *pv);
static void Display_Sleep(bool clear_screen);
static void Display_Wake(void);




static UWORD getImageSize(void) {
    return ((EPD_3IN7_WIDTH % 4 == 0) ? (EPD_3IN7_WIDTH / 4) : (EPD_3IN7_WIDTH / 4 + 1)) * EPD_3IN7_HEIGHT;
}

/*
static void powerOn(void) {
    if (DISPLAY_PWR_PIN >= 0) {
        pinMode(DISPLAY_PWR_PIN, OUTPUT);
        digitalWrite(DISPLAY_PWR_PIN, HIGH);
        DEV_Delay_ms(10);
    }
    screen_on = true;
}
static void powerOff(void) {
    if (DISPLAY_PWR_PIN >= 0) {
        digitalWrite(DISPLAY_PWR_PIN, LOW);
        DEV_Delay_ms(10);
    }
    screen_on = false;
}
*/


/* draw the specific screen into reusable_buf (caller must hold mutex) */
static void paintPhoneScreen(void) {
    if (!reusable_buf) return;
    Paint_SelectImage(reusable_buf);
    if (GRAY_MODE == 4){
        Paint_SetScale(4);
    } else {
        Paint_SetScale(2);
    }
    Paint_Clear(WHITE);
    // title
    Paint_DrawString_EN(10, 5, "erm", &Font24, BLACK, WHITE);
    
}
static void paintHomeScreen(void) {
    if (!reusable_buf) return;
    Paint_SelectImage(reusable_buf);
    Paint_SetScale(SCALE);
    
    Paint_Clear(WHITE);
    // title
    Paint_DrawString_EN(10, 5, "Tele-Ink", &Font24, WHITE, BLACK);
    Paint_DrawString_EN(160, 10, "Version 0.1.4", &Font12, WHITE, BLACK);
    //  status
    if (modem_powered && modem_ready){
        Paint_DrawString_EN(10, 40, "Modem +", &Font16, BLACK, WHITE);
        if (modem_net) {
            Paint_DrawString_EN(10, 40, "Modem ++", &Font16, BLACK, WHITE);
        } 
    } else {
        Paint_DrawString_EN(10, 40, "Modem -", &Font16, BLACK, WHITE);
    }
    
    // WIFI
    if (wifi_on){
        Paint_DrawString_EN(10, 60, "Wifi +", &Font16, BLACK, WHITE);
        if (wifi_connected){
            Paint_DrawString_EN(10, 60, "Wifi ++", &Font16, BLACK, WHITE);
        }
    } else {
        Paint_DrawString_EN(10, 60, "Wifi -", &Font16, BLACK, WHITE);
    }
    
     
}
static void paintCommandScreen(void) {
    if (!reusable_buf) return;
    Paint_SelectImage(reusable_buf);
    Paint_SetScale(SCALE);
    Paint_Clear(WHITE);
    // Text area
    /*
    for (uint32_t i = 0; i < command_page_lines; ++i) {
        if (command_send != NULL) {
            Paint_DrawString_EN(10, 10 + i * 20, command_page_data[i], &Font24, WHITE, BLACK);
        }
        if (command_resp != NULL) {
            Paint_DrawString_EN(10, 20 + i * 20, command_page_data[i+1], &Font24, BLACK, WHITE);
        }
    }
    */
}
static void paintBlankScreen(void) {
    if (!reusable_buf) return;
    Paint_SelectImage(reusable_buf);
    Paint_SetScale(SCALE);
    Paint_Clear(WHITE);
}
static void paintBootScreen(void) {
    if (!reusable_buf) return;
    Paint_SelectImage(reusable_buf);
    Paint_SetScale(4);
    Paint_Clear(WHITE);

    Paint_DrawPoint(10, 80, BLACK, DOT_PIXEL_1X1, DOT_STYLE_DFT);
    Paint_DrawPoint(10, 90, BLACK, DOT_PIXEL_2X2, DOT_STYLE_DFT);
    Paint_DrawPoint(10, 100, BLACK, DOT_PIXEL_3X3, DOT_STYLE_DFT);
    Paint_DrawLine(20, 70, 70, 120, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawLine(70, 70, 20, 120, BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawRectangle(20, 70, 70, 120, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(80, 70, 130, 120, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawCircle(45, 95, 20, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawCircle(105, 95, 20, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawLine(85, 95, 125, 95, BLACK, DOT_PIXEL_1X1, LINE_STYLE_DOTTED);
    Paint_DrawLine(105, 75, 105, 115, BLACK, DOT_PIXEL_1X1, LINE_STYLE_DOTTED);
    Paint_DrawString_EN(10, 0, "waveshare", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(10, 20, "NoobPhone v0.0.1", &Font12, WHITE, BLACK);
    Paint_DrawNum(10, 33, 123456789, &Font12, BLACK, WHITE);
    Paint_DrawNum(10, 50, 987654321, &Font16, WHITE, BLACK);
    Paint_DrawString_EN(10, 150, "GRAY1 with black background", &Font24, BLACK, GRAY1);
    Paint_DrawString_EN(10, 175, "GRAY2 with white background", &Font24, WHITE, GRAY2);
    Paint_DrawString_EN(10, 200, "GRAY3 with white background", &Font24, WHITE, GRAY3);
    Paint_DrawString_EN(10, 225, "GRAY4 with white background", &Font24, WHITE, GRAY4);
}

// Page management
static void setPage(PageType page) {
    current_page = page;
}

static void paintCurrentPage(void) {
    switch (current_page) {
        case PAGE_HOME:    paintHomeScreen(); break;
        case PAGE_IDLE:    paintBlankScreen(); break;
        case PAGE_COMMAND: paintCommandScreen(); break;
        default: break;
    }
}


static void Display_StartTask(void) {
    if (display_task_handle) return;
    screen_on = true;
    //Core 0, Priority 2
    xTaskCreatePinnedToCore(displayTask, "display", 8192, NULL, 2, &display_task_handle, 0);
}

/* Initilize the display with boot screen */
void Display_Init(void) {
    //Mutex creation for displayTask
    if (!epd_mutex) {
        epd_mutex = xSemaphoreCreateMutex();
        if (!epd_mutex) {
            printf("ERROR: Failed to create epd_mutex!\r\n");
            return;
        }
    }

    if (GRAY_MODE != 4){
        printf("ERROR: Expected GRAY_MODE 4\r\n");
        return;
    } 
    //powerOn();
    
    //Init EPD
    EPD_3IN7_4Gray_Init();
    EPD_3IN7_4Gray_Clear();
    DEV_Delay_ms(500);

    reusable_size = getImageSize();
    if((reusable_buf = (UBYTE *)malloc(reusable_size)) == NULL) {
        printf("ERROR: Failed to allocate reusable_buf (%u bytes)\r\n", reusable_size);
        return;
    }
    
    // create an empty canvas so paint APIs are ready to use
    Paint_NewImage(reusable_buf, EPD_3IN7_WIDTH, EPD_3IN7_HEIGHT, 270, WHITE);
    
    DEV_Delay_ms(200);

    // Just start with displaying boot screen
    paintBootScreen();
    EPD_3IN7_4Gray_Display(reusable_buf);
    DEV_Delay_ms(1000);

    // create queue & task
    if (!dispQueue) {
        dispQueue = xQueueCreate(8, sizeof(DisplayEvent));
        if (!dispQueue) {
            printf("ERROR: Failed to create dispQueue!\r\n");
            return;
        }
    }
    
    Display_StartTask();
}
// Deinit: shutdown and free resources (free buffer & delete mutex) (ASSUMES MUTEX IS HELD)
void Display_Deinit(void) {
    if (!screen_on) {
        Display_Wake();
    } 
    Display_Sleep(true);

    // Stop the display task if we created it (and it's not the caller)
    if (display_task_handle && display_task_handle != xTaskGetCurrentTaskHandle()) {
        printf("Display_Deinit: deleting display task\n");
        vTaskDelete(display_task_handle);
        display_task_handle = NULL;
    } else if (display_task_handle == xTaskGetCurrentTaskHandle()) {
        // clear handle so others know it's gone
        display_task_handle = NULL;
    }

    if (dispQueue) {
        vQueueDelete(dispQueue);
        dispQueue = NULL;
    }
    if (reusable_buf) {
        free(reusable_buf);
        reusable_buf = NULL;
        reusable_size = 0;
    }
}

// Wakes screen from sleep and clears paint(ASSUMES mutex is taken!)
static void Display_Wake(void) {
    if (screen_on) {
        printf("Display_Wake: screen is on\r\n"); 
        return;
    }
    //powerOn();

    EPD_3IN7_4Gray_Init();

    DEV_Delay_ms(100);
    if (GRAY_MODE == 1){
        EPD_3IN7_1Gray_Init();
        SCALE = 2;
    } else {
        SCALE = 4;
    }

    DEV_Delay_ms(50);
    screen_on = true;

    printf("Display_Wake: Painting\r\n");
    Paint_SelectImage(reusable_buf);
    Paint_SetScale(SCALE);
    Paint_Clear(WHITE);
}
// Sends screen into sleep and or clears screen first (ASSUMES mutex is taken!)
static void Display_Sleep(bool clear_screen) {
    if (!screen_on) {
        printf("Display_Sleep: screen is not on\r\n"); 
        return;
    }

    if (clear_screen) {
        EPD_3IN7_4Gray_Clear();
        setPage(PAGE_NONE);
    }
        
    EPD_3IN7_Sleep();
    printf("Display_Sleep: Slept\r\n");
    DEV_Delay_ms(50);
    //powerOff();
    screen_on = false;
}




/* post event to queue */
bool Display_PostEvent(const DisplayEvent *evt, TickType_t ticksToWait) {
    if (!dispQueue) return false;
    return xQueueSend(dispQueue, evt, ticksToWait) == pdTRUE;
}
/* Add display events to queue from outside display*/
void Display_Event_ShowHome() {
    DisplayEvent e = { .type = DISP_EVT_SHOW_HOME, .payload = NULL};
    Display_PostEvent(&e, 0);
}
void Display_Event_ShowCommand() {
    DisplayEvent e = { .type = DISP_EVT_SHOW_COMMAND, .payload = NULL};
    Display_PostEvent(&e, 0);
}
void Display_Event_ShowIdle(){
    DisplayEvent e = { .type = DISP_EVT_SHOW_IDLE, .payload = NULL};
    Display_PostEvent(&e, 0);
}
void Display_Event_WifiConnected() {
    DisplayEvent e = { .type = DISP_EVT_WIFI_CONNECTED, .payload = NULL};
    Display_PostEvent(&e, 0);
}

static void Display_HandleScreenEvent(void) {
    if (current_page == PAGE_NONE) return;

    if (GRAY_MODE == 1) {
        EPD_3IN7_4Gray_Init();
        DEV_Delay_ms(50);
    }
    // 4Gray Clear screen first
    EPD_3IN7_4Gray_Clear();
    DEV_Delay_ms(20);
    GRAY_MODE = 4;
    SCALE = 4;

    // Display in 4 gray
    paintCurrentPage();
    EPD_3IN7_4Gray_Display(reusable_buf);
    DEV_Delay_ms(20);
    
    // Start partial updates if needed
    if (current_page == PAGE_HOME || current_page == PAGE_IDLE || current_page == PAGE_COMMAND) {
        // Switch to 1 gray for partial updates
        EPD_3IN7_1Gray_Init();
        DEV_Delay_ms(20);
        GRAY_MODE = 1;
        SCALE = 2;
    }
}

/* display task processes events and updates screen on demand */
static void displayTask(void *pv) {
    (void)pv;
    //PAINT_TIME <- Modem;
    DisplayEvent evt;
    PAINT_TIME sPaint_time;
    sPaint_time.Hour = 0;
    sPaint_time.Min = 0;
    sPaint_time.Sec = 0;

    last_activity_tick = xTaskGetTickCount();


    for (;;) {
        // Wait for event 500ms polling
        if (xQueueReceive(dispQueue, &evt, pdMS_TO_TICKS(500)) == pdTRUE) {
            // Take screen mutex
            if (!epd_mutex || (xSemaphoreTake(epd_mutex, pdMS_TO_TICKS(2000)) != pdTRUE)) {
                printf("displayTask: failed to take epd_mutex for event (timeout)\r\n");
                continue;
            }

            last_activity_tick = xTaskGetTickCount();
            idle_timeout_count = 0;
            // update internal state based on event
            switch (evt.type) {
                case DISP_EVT_SLEEP: // Keyboard low activity 
                    Display_Sleep(false);
                    if (epd_mutex) xSemaphoreGive(epd_mutex);
                    continue;
                case DISP_EVT_SHOW_HOME: setPage(PAGE_HOME); break;
                case DISP_EVT_SHOW_COMMAND: setPage(PAGE_COMMAND); break;
                case DISP_EVT_SHOW_IDLE: setPage(PAGE_IDLE); break;
                case DISP_EVT_WIFI_ON: wifi_on = true; break;
                case DISP_EVT_WIFI_CONNECTED: wifi_connected = true; break; 
                case DISP_EVT_MODEM_POWERED: modem_powered = true; break;
                case DISP_EVT_MODEM_READY: modem_ready = true; modem_powered = true; break;
                case DISP_EVT_MODEM_NET: modem_net = true; break;
                case DISP_EVT_SMS_RECEIVED: ++unread_sms; break;
                case DISP_EVT_RING: ringing = true; break;
            }

            if (reusable_buf) {
                // Chirp screen awake
                if(!screen_on){
                    printf("Wakeup screen\r\n");
                    Display_Wake();
                }
                // Handle drawing appropriate screen
                Display_HandleScreenEvent();
            } else {
                printf("ERROR with display buffer\r\n");
            }

            if (epd_mutex) xSemaphoreGive(epd_mutex);
            if (evt.type == DISP_EVT_RING) ringing = false;
        }


        // Partial update screen EPD width=280 height=480
        if (GRAY_MODE == 1 && screen_on) {
            // Start Partial update
            if (epd_mutex && xSemaphoreTake(epd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE){
                // Paint
                paintCurrentPage();
                if (current_page == PAGE_HOME) {
                    //Paint_ClearWindows(370, 0, 479, 40, WHITE);
                    Paint_DrawTime(370, 10, &sPaint_time, &Font20, WHITE, BLACK);
                    EPD_3IN7_1Gray_Display(reusable_buf); // FLASHING CLEAR HERE FOR SOME REASON
                }
                if (current_page == PAGE_IDLE) {
                    //Paint_DrawCircle(240, 140, (idle_page_tick_count % 30)+5, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
                    Paint_ClearWindows(200, 100, 280, 180, WHITE);
                    if ( idle_page_tick_count % 3 == 0 ){
                       Paint_DrawPoint(240, 140, BLACK, DOT_PIXEL_2X2, DOT_STYLE_DFT); 
                    } else  if ( idle_page_tick_count % 3 == 1 ) {
                        Paint_DrawPoint(240, 140, BLACK, DOT_PIXEL_3X3, DOT_STYLE_DFT);
                    } else {
                        Paint_DrawPoint(240, 140, BLACK, DOT_PIXEL_5X5, DOT_STYLE_DFT);
                    }
                    EPD_3IN7_1Gray_Display(reusable_buf);
                    ++idle_page_tick_count;
                }
                if (current_page == PAGE_COMMAND) { 
                    //paintCommandScreen();
                    //Paint_ClearWindows(10, 200, 270, 280, WHITE);
                    //Paint_DrawTime(200, 210, &sPaint_time, &Font16, WHITE, BLACK);
                    //EPD_3IN7_1Gray_Display(reusable_buf);
                }
                DEV_Delay_ms(20);
            }
            // End Partial update
            if (epd_mutex) xSemaphoreGive(epd_mutex);  
        } 
        
        // Track low activity
        if ((xTaskGetTickCount() - last_activity_tick) >= pdMS_TO_TICKS(idle_timeout_ms) && screen_on) {
            printf("Activity low in displayTask.\r\n");
            last_activity_tick = xTaskGetTickCount();
            ++idle_timeout_count;
        }

        // No activity sleeping ?
        if (idle_timeout_count >= 5 && screen_on){
            Display_Sleep(false); 
        }

        DEV_Delay_ms(10);
    }
}



