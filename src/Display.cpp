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

#define POLL_MS 100

// Public
bool screen_on = false;
PageType current_page = PAGE_NONE;
PageType last_page = PAGE_NONE;
CommandBuffer cmd_buffer = {0};


// Private
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
static bool page_change_evt = false;
static uint32_t partial_update_count = 0;


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

// FD Painting functions
static UWORD getImageSize(void);
static void paintHomeScreen(void);
static void paintCommandScreen(void);
static void paintBlankScreen(void);
static void paintBootScreen(void);
static void paintPhoneScreen(void);
// FD Display task functions
static void Display_StartTask(void);
static void Display_HandleScreenChange(void);
static void HandlePartialUpdate_command(void);
static void HandlePartialUpdate_idle(void);
static void Display_HandlePartialUpdate(PAINT_TIME &spaintime);
static void displayTask(void *pv);
static void Display_Sleep(bool clear_screen);
static void Display_Wake(void);
static void Display_UpdateFullScreen(void);



static UWORD getImageSize(void) {
    return ((EPD_3IN7_WIDTH % 4 == 0) ? (EPD_3IN7_WIDTH / 4) : (EPD_3IN7_WIDTH / 4 + 1)) * EPD_3IN7_HEIGHT;
}

// Paint screen (assume 4 gray)
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


// Page management (only meaningful within display task)
static void setPage(PageType page) {
    last_page = current_page;
    current_page = page;
    if (page != PAGE_NONE) page_change_evt = true;
}
static void paintCurrentPage(void) {
    switch (current_page) {
        case PAGE_HOME:    paintHomeScreen(); break;
        case PAGE_IDLE:    paintBlankScreen(); break;
        case PAGE_COMMAND: paintCommandScreen(); break;
        default: break;
    }
}


// Wakes screen from sleep and clears paint(ASSUMES mutex is taken!)
static void Display_Wake(void) {
    if (screen_on) {
        printf("Display_Wake: screen is on\r\n"); 
        return;
    }
    if (current_page == PAGE_NONE) {
        printf("Display_Wake: page not selected\r\n");
        return;
    }

    EPD_3IN7_4Gray_Init();
    DEV_Delay_ms(100);

    if (GRAY_MODE == 1){
        EPD_3IN7_1Gray_Init();
        SCALE = 2;
    } else {
        SCALE = 4;
    }

    screen_on = true;
    printf("Woke display\r\n");
}
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
    printf("Slept display\r\n");
    DEV_Delay_ms(50);
    screen_on = false;
}

// Post event to display task
bool Display_PostEvent(const DisplayEvent *evt, TickType_t ticksToWait) {
    if (!dispQueue) return false;
    return xQueueSend(dispQueue, evt, ticksToWait) == pdTRUE;
}
// Public event calls
void Display_Event_Wake(void) {
    DisplayEvent e = { .type = DISP_EVT_WAKE, .payload = NULL};
    Display_PostEvent(&e, 0);
}
void Display_Event_Sleep(void) {
    DisplayEvent e = { .type = DISP_EVT_SLEEP, .payload = NULL};
    Display_PostEvent(&e, 0);
}
void Display_Event_ShowHome(void) {
    DisplayEvent e = { .type = DISP_EVT_SHOW_HOME, .payload = NULL};
    Display_PostEvent(&e, 0);
}
void Display_Event_ShowCommand(void) {
    DisplayEvent e = { .type = DISP_EVT_SHOW_COMMAND, .payload = NULL};
    Display_PostEvent(&e, 0);
}
void Display_Event_ShowIdle(void){
    DisplayEvent e = { .type = DISP_EVT_SHOW_IDLE, .payload = NULL};
    Display_PostEvent(&e, 0);
}
void Display_Event_WifiConnected(void) {
    DisplayEvent e = { .type = DISP_EVT_WIFI_CONNECTED, .payload = NULL};
    Display_PostEvent(&e, 0);
}

void Display_ClearCommandHistory(void) {
    if (cmd_buffer.mutex && xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cmd_buffer.history_count = 0;
        for (int i = 0; i < CMD_HISTORY_LINES; i++) {
            cmd_buffer.history[i][0] = '\0';
        }
        xSemaphoreGive(cmd_buffer.mutex);
    }
}


static void Display_UpdateFullScreen(void) {
    printf("Display_UpdateFullScreen\r\n");
    if (GRAY_MODE == 1) {
        GRAY_MODE = 4;
        SCALE = 4;
        paintCurrentPage();
        GRAY_MODE = 1;
        SCALE = 2;
    } else {
        GRAY_MODE = 4;
        SCALE = 4;
        paintCurrentPage();
    }
    EPD_3IN7_4Gray_Display(reusable_buf);
    DEV_Delay_ms(10);
}

// Handle full screen changes (not protected by mutex)
static void Display_HandleScreenChange(void) {
    if (current_page == PAGE_NONE) return;
    Display_UpdateFullScreen();
    //DEV_Delay_ms(100);
    
    // Start partial updates if needed
    if (current_page == PAGE_HOME || current_page == PAGE_IDLE || current_page == PAGE_COMMAND) {
        // Switch to 1 gray for partial updates
        EPD_3IN7_1Gray_Init();
        GRAY_MODE = 1;
        SCALE = 2;
    }
}

static void HandlePartialUpdate_command(void) {
    // Copy data from cmd_buffer while holding mutex briefly
    static char current_input[CMD_BUFFER_SIZE];
    static char history_copy[CMD_HISTORY_LINES][CMD_BUFFER_SIZE];
    int history_count = 0;

    if (cmd_buffer.mutex && xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strcpy(current_input, cmd_buffer.input);
        history_count = cmd_buffer.history_count;
        for (int i = 0; i < history_count && i < CMD_HISTORY_LINES; i++) {
            strcpy(history_copy[i], cmd_buffer.history[i]);
        }
        xSemaphoreGive(cmd_buffer.mutex);
    }
    else {
        printf("Display: failed to take cmd_buffer.mutex\r\n");
        current_input[0] = '\0';
        history_count = 0;
    }

    int input_y = 250;

    // Draw current input with cursor at fixed bottom position
    char display_line[CMD_BUFFER_SIZE + 2];
    snprintf(display_line, sizeof(display_line), "> %s_", current_input);

    Paint_DrawString_EN(5, input_y, display_line, &Font16, WHITE, BLACK);

    // Draw history scrolling upward from above the input line
    int lines_to_show = (input_y - 35) / 18;
    int start_idx = (history_count > lines_to_show) ? (history_count - lines_to_show) : 0;

    int y = input_y - 18;
    for (int i = history_count - 1; i >= start_idx; i--) {
        Paint_DrawString_EN(5, y, history_copy[i], &Font16, BLACK, WHITE);
        y -= 18;
    }
}

static void HandlePartialUpdate_idle(void) {
    // Paint_DrawCircle(240, 140, (idle_page_tick_count % 30)+5, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_ClearWindows(200, 100, 280, 180, WHITE);
    if (idle_page_tick_count % 3 == 0) {
        Paint_DrawPoint(240, 140, BLACK, DOT_PIXEL_2X2, DOT_STYLE_DFT);
    }
    else if (idle_page_tick_count % 3 == 1) {
        Paint_DrawPoint(240, 140, BLACK, DOT_PIXEL_3X3, DOT_STYLE_DFT);
    }
    else {
        Paint_DrawPoint(240, 140, BLACK, DOT_PIXEL_5X5, DOT_STYLE_DFT);
    }
    //EPD_3IN7_1Gray_Display(reusable_buf);
    ++idle_page_tick_count;
}
// Handle partial updates for current page (drawing and displaying)
static void Display_HandlePartialUpdate(PAINT_TIME &spaintime) {
    // Paint
    Paint_SelectImage(reusable_buf);
    Paint_SetScale(2);
    Paint_Clear(WHITE);

    if (current_page == PAGE_COMMAND) {
        HandlePartialUpdate_command();
    }
    else if (current_page == PAGE_HOME) {
        // Paint_ClearWindows(370, 0, 479, 40, WHITE);
        Paint_DrawTime(370, 10, &spaintime, &Font20, WHITE, BLACK);
        // EPD_3IN7_1Gray_Display(reusable_buf); // FLASHING CLEAR HERE FOR SOME REASON
    }
    else if (current_page == PAGE_IDLE) {
        HandlePartialUpdate_idle();
    }
    else {
        printf("No partial update\r\n");
        xSemaphoreGive(epd_mutex);
        DEV_Delay_ms(20);
        return;
    }
    EPD_3IN7_1Gray_Display(reusable_buf);
    DEV_Delay_ms(20);
    // End Partial update
}

// Display task
static void displayTask(void *pv) {
    (void)pv;
    //PAINT_TIME <- Modem;
    static TickType_t last_partial_update = 0;
    DisplayEvent evt;
    PAINT_TIME sPaint_time;
    sPaint_time.Hour = 0;
    sPaint_time.Min = 0;
    sPaint_time.Sec = 0;

    last_activity_tick = xTaskGetTickCount();


    for (;;) {
        // Wait for event 100ms polling if none do partial update
        if (xQueueReceive(dispQueue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Take screen mutex
            if (!epd_mutex || (xSemaphoreTake(epd_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)) {
                printf("displayTask: failed to take epd_mutex for event (timeout)\r\n");
                DEV_Delay_ms(POLL_MS*2);
                continue;
            }

            last_activity_tick = xTaskGetTickCount();
            idle_timeout_count = 0;
            // update internal state based or wake/sleep on event
            switch (evt.type) {
                case DISP_EVT_SLEEP: Display_Sleep(false); xSemaphoreGive(epd_mutex); continue;
                case DISP_EVT_WAKE: break; //Display_Wake(); xSemaphoreGive(epd_mutex); continue;
                case DISP_EVT_SHOW_HOME: setPage(PAGE_HOME); break;
                case DISP_EVT_SHOW_COMMAND: setPage(PAGE_COMMAND); break;
                case DISP_EVT_SHOW_IDLE: setPage(PAGE_IDLE); break;
                case DISP_EVT_WIFI_ON: wifi_on = true; break;
                case DISP_EVT_WIFI_CONNECTED: wifi_connected = true; break; 
                case DISP_EVT_MODEM_POWERED: modem_powered = true; break;
                case DISP_EVT_MODEM_READY: modem_ready = true; modem_powered = true; break;
                case DISP_EVT_MODEM_NET: modem_ready = true; modem_powered = true; modem_net = true; break;
                case DISP_EVT_SMS_RECEIVED: ++unread_sms; break;
                case DISP_EVT_RING: ringing = true; break;
            }
            
            // Handle screen wake & changes
            if (reusable_buf) {
                // Chirp screen awake
                if(!screen_on) {
                    Display_Wake();
                } else if (!page_change_evt){
                    // Redraw current page
                    //Display_UpdateFullScreen();
                }

                // Switch screen or update current fullsscreen
                if (page_change_evt && last_page != current_page) {
                    Display_HandleScreenChange();
                    page_change_evt = false;
                } 
                //last_partial_update = xTaskGetTickCount();
                partial_update_count = 0;
            }

            xSemaphoreGive(epd_mutex);
            if (evt.type == DISP_EVT_RING) ringing = false;
        }


        //EPD width=280 height=480

        //Partial update only if 500ms have elapsed
        if (GRAY_MODE == 1 && screen_on && (xTaskGetTickCount() - last_partial_update) >= pdMS_TO_TICKS(100)) {
            // Refresh display after 30 partial updates to avoid ghosting
            /*
            if (partial_update_count >= 30) {
                if (xSemaphoreTake(epd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                    Display_UpdateFullScreen();
                    xSemaphoreGive(epd_mutex);
                }
                last_partial_update = xTaskGetTickCount();
                partial_update_count = 0;
                continue;
            } */
            if (xSemaphoreTake(epd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                Display_HandlePartialUpdate(sPaint_time);
                xSemaphoreGive(epd_mutex);
            }
            last_partial_update = xTaskGetTickCount();
            ++partial_update_count;
        } 
        
        // Low activity
        if (screen_on && (xTaskGetTickCount() - last_activity_tick) >= pdMS_TO_TICKS(idle_timeout_ms)) {
            printf("Activity low in displayTask.\r\n");
            last_activity_tick = xTaskGetTickCount();
            ++idle_timeout_count;
        }

        // No activity sleeping ?
        if (idle_timeout_count >= 5 && screen_on){
            //Display_Sleep(false); 
        }

        DEV_Delay_ms(50);
    }
}



static void Display_StartTask(void) {
    if (display_task_handle) return;
    screen_on = true;
    //Core 0, Priority 2
    xTaskCreatePinnedToCore(displayTask, "display", 8192, NULL, 2, &display_task_handle, 0);
}

// Initialize display ds and start display task (does not send any events)
void Display_Init(void) {
    // Mutex creation for displayTask and command buffer
    if (!epd_mutex && !cmd_buffer.mutex) {
        epd_mutex = xSemaphoreCreateMutex();
        cmd_buffer.mutex = xSemaphoreCreateMutex();
        if (!epd_mutex || !cmd_buffer.mutex) {
            printf("ERROR: Failed to create necessary mutex for display!\r\n");
            return;
        }
    }
    
    // Command buffer init
    cmd_buffer.input[0] = '\0';
    cmd_buffer.output[0] = '\0';
    cmd_buffer.history_count = 0;
    cmd_buffer.state = CMD_STATE_IDLE;

    if (GRAY_MODE != 4){
        printf("ERROR: Expected GRAY_MODE 4\r\n");
        return;
    } 
    
    // Init EPD
    EPD_3IN7_4Gray_Init();
    //EPD_3IN7_4Gray_Clear();
    DEV_Delay_ms(500);

    reusable_size = getImageSize();
    if((reusable_buf = (UBYTE *)malloc(reusable_size)) == NULL) {
        printf("ERROR: Failed to allocate reusable_buf (%u bytes)\r\n", reusable_size);
        return;
    }
    
    // Create an empty canvas so paint APIs are ready to use
    Paint_NewImage(reusable_buf, EPD_3IN7_WIDTH, EPD_3IN7_HEIGHT, 270, WHITE);
    
    DEV_Delay_ms(200);

    // Start with displaying boot screen
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
