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
PageType boot_page = PAGE_BOOT;
CommandBuffer cmd_buffer = {0};

bool modem_ready = false;
bool modem_net = false;
bool modem_powered = false;
bool sms_send = false;
bool sms_read = false;
int sms_count = 0;
bool at_mode = false;
//char* sms_number = NULL;

// Private
// Screen
static UBYTE *image_buf1 = NULL;
static UWORD image_size1 = 0;
static UBYTE *image_buf4 = NULL;
static UWORD image_size4 = 0;
static uint8_t GRAY_MODE = 4;
// screenTask
static SemaphoreHandle_t epd_mutex = NULL;
static QueueHandle_t dispQueue = NULL;
static TaskHandle_t display_task_handle = NULL;
static uint32_t idle_timeout_ms = 60000; //One minute
static int idle_timeout_count = 0; 
static TickType_t last_activity_tick = 0;
static uint32_t idle_page_tick_count = 0;
static bool page_change_evt = false;
static uint32_t partial_update_count = 0;


// Modem
static int unread_sms = 0;
static bool ringing = false;
static TickType_t end = 0;


// Wifi
static bool wifi_connected = false;
static bool wifi_on = false;

// FD Painting functions
static UWORD getImageSizeForMode(uint8_t gray);
static void paintConfigureForMode(uint8_t gray);
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
static void Display_HandlePartialUpdate(void);
static void displayTask(void *pv);
static void Display_Sleep(bool clear_screen);
static void Display_Wake(void);
static void Display_UpdateFullScreen(void);


// Paint functions for dynamic display modes
static UWORD getImageSizeForMode(uint8_t gray) {
    if (gray == 4) {
        return ((EPD_3IN7_WIDTH % 4 == 0) ? (EPD_3IN7_WIDTH / 4) : (EPD_3IN7_WIDTH / 4 + 1)) * EPD_3IN7_HEIGHT;
    } else {
        return ((EPD_3IN7_WIDTH % 8 == 0) ? (EPD_3IN7_WIDTH / 8) : (EPD_3IN7_WIDTH / 8 + 1)) * EPD_3IN7_HEIGHT;
    }
}
static void paintConfigureForMode(uint8_t gray) {
    if (gray == 4 && image_buf4) {
        Paint_SelectImage(image_buf4);
        Paint_SetScale(4);
    } else if (gray == 1 && image_buf1) {
        Paint_SelectImage(image_buf1);
        Paint_SetScale(2);
    }
}

// Page management 
static void setPage(PageType page) {
    last_page = current_page;
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


// Paint screen
static void paintPhoneScreen(void) {
    paintConfigureForMode(4);
    Paint_Clear(WHITE);
    // title
    Paint_DrawString_EN(10, 5, "erm", &Font24, BLACK, WHITE);
    
}
static void paintHomeScreen(void) {
    paintConfigureForMode(4);
    Paint_Clear(WHITE);
    // title
    Paint_DrawString_EN(10, 5, "Tele-Ink", &Font24, WHITE, BLACK);
    Paint_DrawString_EN(160, 10, "Version 0.2.1", &Font12, WHITE, BLACK);
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
    paintConfigureForMode(4);
    Paint_Clear(WHITE);
    // Paints everything in handlePartialUpdate
}
static void paintBlankScreen(void) {
    paintConfigureForMode(4);
    Paint_Clear(WHITE);
}
static void paintBootScreen(void) {
    paintConfigureForMode(4);
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
    Paint_DrawString_EN(10, 0, "Logan P", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(10, 20, "NoobPhone v0.2.1", &Font12, WHITE, BLACK);
    Paint_DrawNum(10, 33, 123456789, &Font12, BLACK, WHITE);
    Paint_DrawNum(10, 50, 987654321, &Font16, WHITE, BLACK);
    Paint_DrawString_EN(10, 150, "You can change modes w/ Sym", &Font24, BLACK, GRAY1);
    Paint_DrawString_EN(10, 175, "In command mode use  /<cmd>", &Font24, WHITE, GRAY2);
    Paint_DrawString_EN(10, 200, "You can execute AT commands", &Font24, WHITE, GRAY3);
    Paint_DrawString_EN(10, 225, "Global Roaming GNSS 4G Data", &Font24, WHITE, GRAY4);
}



// Wakes screen from sleep and gets ready for painting and displaying
static void Display_Wake(void) {
    if (screen_on) {
        printf("Display_Wake: screen is on\r\n"); 
        return;
    }
    // Resume current page after sleep
    if (current_page == PAGE_NONE && last_page != PAGE_NONE) {
        setPage(last_page); // GRAY_MODE should correlate...
    }

    // Init 1Gray
    if (GRAY_MODE == 1) {
        printf("Initializing 1Gray mode wake\r\n");
        EPD_3IN7_1Gray_Init();
        DEV_Delay_ms(10);
    } else {
        // Init 4Gray
        //printf("Initializing 4Gray mode not\r\n");
        //EPD_3IN7_4Gray_Init(); 
        //DEV_Delay_ms(100);
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
    DEV_Delay_ms(50);
    printf("Slept display\r\n");
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
// Non event calls
void Display_ClearCommandHistory(void) {
    if (cmd_buffer.mutex && xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cmd_buffer.history_count = 0;
        for (int i = 0; i < CMD_HISTORY_LINES; i++) {
            cmd_buffer.history[i][0] = '\0';
        }
        xSemaphoreGive(cmd_buffer.mutex);
    }
}


// Paint the current screen again in 4gray
static void Display_UpdateFullScreen(void) {
    printf("Display_UpdateFullScreen\r\n");
    if (GRAY_MODE == 1){
        //printf("Initializing 4Gray mode not\r\n");
        //EPD_3IN7_4Gray_Init();
        //DEV_Delay_ms(20);
    }
    paintCurrentPage();
    EPD_3IN7_4Gray_Display(image_buf4);

    if (GRAY_MODE == 1){
        printf("Initializing 1Gray mode UFS\r\n");
        EPD_3IN7_1Gray_Init();
        DEV_Delay_ms(10);
    }
}

// Paints 4gray background image then gets ready for partial update if needed
static void Display_HandleScreenChange(void) {
    printf("Display_HandleScreenChange\r\n");
    if (current_page == PAGE_NONE) return;

    paintCurrentPage();
    EPD_3IN7_4Gray_Display(image_buf4);
    
    // Start partial updates if needed
    if (current_page == PAGE_HOME || current_page == PAGE_IDLE || current_page == PAGE_COMMAND) {
        // Switch to 1 gray for partial updates
        printf("Initializing 1Gray mode HSC\r\n");
        EPD_3IN7_1Gray_Init();
        DEV_Delay_ms(10);
        GRAY_MODE = 1;
    } else {
        // Init 4Gray
        printf("Initializing 4Gray mode not\r\n");
        //EPD_3IN7_4Gray_Init(); 
        //DEV_Delay_ms(20);
        GRAY_MODE = 4;
    }
}


// Gets data from command buffer and calculates wrapping buffer for long strings
// paints data into respective locations and displays in 1Gray (no part disp = 500ms updates)
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
    // Draw current input with respective cursor for mode
    int input_y = 250;
    char display_line[CMD_BUFFER_SIZE + 2];
    if (sms_send){
        snprintf(display_line, sizeof(display_line), "> %s_", current_input);
    } else if (sms_read){
        snprintf(display_line, sizeof(display_line), "SMS: %s_", current_input);
    } else if (at_mode){
        snprintf(display_line, sizeof(display_line), "AT%s_", current_input);
    } else {
        snprintf(display_line, sizeof(display_line), "$ %s_", current_input);
    }

    // Protect long input strings (ptr math)
    int len = strlen(display_line);
    int total_lines = (len + 29) / 30;
    int history_shift = 22 * (total_lines - 1);
    // Calc history lines (inc wrapped)
    int total_history_lines = 0;
    for (int i = 0; i < history_count; i++) {
        int hlen = strlen(history_copy[i]);
        total_history_lines += (hlen + 29) / 30;
    }
    // History first shifted by input overflow (bottom up w wrap) 
    int hy = input_y - 25 - history_shift;
    for (int i = history_count - 1; i >= 0 && hy >= 0; i--) {
        int hlen = strlen(history_copy[i]);
        int hlines = (hlen + 29) / 30;

        // Start from last line of the entry
        int hoffset = (hlines - 1) * 30;

        for (int j = hlines - 1; j >= 0 && hy >= 0; j--) {
            char saved = '\0';
            if (hoffset + 30 < hlen) {
                saved = history_copy[i][hoffset + 30];
                history_copy[i][hoffset + 30] = '\0';
            }
            Paint_DrawString_EN(10, hy, history_copy[i] + hoffset, &Font20, BLACK, WHITE);

            if (saved != '\0') {
                history_copy[i][hoffset + 30] = saved;
            }
            hy -= 22;
            hoffset -= 30;
        }
    }
    // Calculate and display input line (with wrapping)
    int iy = input_y - (22 * (total_lines - 1));
    int offset = 0;
    while (offset < len) {
        char saved = '\0';
        if (offset + 30 < len) {
            saved = display_line[offset + 30];
            display_line[offset + 30] = '\0';
        }
        Paint_DrawString_EN(10, iy, display_line + offset, &Font20, WHITE, BLACK);

        if (saved != '\0'){
            display_line[offset + 30] = saved;
        }
        iy += 22;
        offset += 30;
    }
    
    // Display final image :)    
    EPD_3IN7_1Gray_Display(image_buf1);
}

static void HandlePartialUpdate_idle(void) {
    // small bouncing rectangle (VCR-style) animation
    // x and y are reversed! W = .x
    static bool init = false;
    static int rx, ry, rw, rh;
    static int prev_rx, prev_ry;
    static int vx, vy;

    int W = EPD_3IN7_HEIGHT;
    int H = EPD_3IN7_WIDTH;
    int margin = 4;
    int ex0, ey0, ex1, ey1;
    int dx0, dy0, dx1, dy1;
    int ix0, iy0, ix1, iy1;

    if (!init) {
        rw = 50; rh = 50;
        rx = (W - rw) / 2;
        ry = (H - rh) / 2;
        prev_rx = rx; 
        prev_ry = ry;
        // seed pseudo-random with tick count
        srand((unsigned) xTaskGetTickCount());
        do { vx = (int)(rand() % 5) + 5; } while (vx == 0);
        do { vy = (int)(rand() % 5) + 5; } while (vy == 0);
        init = true;
    }

    // erase previous rectangle area with a small margin
    margin = 4;
    ex0 = prev_rx - margin; if (ex0 < 0) ex0 = 0;
    ey0 = prev_ry - margin; if (ey0 < 0) ey0 = 0;
    ex1 = prev_rx + rw + margin; if (ex1 >= W) ex1 = W - 1;
    ey1 = prev_ry + rh + margin; if (ey1 >= H) ey1 = H - 1;
    Paint_ClearWindows(ex0, ey0, ex1, ey1, WHITE);

    // update position
    rx += vx;
    ry += vy;

    // bounce on horizontal edges
    if (rx <= 0) { // Left
        rx = 0;
        vx = -vx;
        vx += (int)(rand() % 3) - 2;
    } else if (rx + rw > W - 1) { // Right
        rx = (W - 1) - (rw - 1);
        if (rx < 0) rx = 0;
        vx = -vx;
        vx += (int)(rand() % 3) - 2;
    }

    // bounce on vertical edges
    if (ry <= 0) { // Top
        ry = 0;
        vy = -vy;
        vy += (int)(rand() % 3) - 2;
    } else if (ry + rh - 1 > H - 1) { // Bottom
        ry = (H - 1) - (rh - 1);
        if (ry < 0) ry = 0;
        vy = -vy;
        vy += (int)(rand() % 3) - 2;
    }

    // outer rectangle - clamp draw coordinates to valid inclusive range
    dx0 = rx; if (dx0 < 0) dx0 = 0;
    dy0 = ry; if (dy0 < 0) dy0 = 0;
    dx1 = rx + rw - 1; if (dx1 > W - 1) dx1 = W - 1;
    dy1 = ry + rh - 1; if (dy1 > H - 1) dy1 = H - 1;
    Paint_DrawRectangle(dx0, dy0, dx1, dy1, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    // inner rectangle - compute and clamp, draw only if valid
    ix0 = dx0 + 4; 
    iy0 = dy0 + 4;
    ix1 = dx1 - 4; 
    iy1 = dy1 - 4;
    if (ix1 > ix0 && iy1 > iy0) {
        Paint_DrawRectangle(ix0, iy0, ix1, iy1, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    }

    // picture
    ix0 = dx0 + ((dx1 - dx0 + 1) - (3 * Font24CN.Width)) / 2;
    iy0 = dy0 + ((dy1 - dy0 + 1) - Font24CN.Height) / 2;
    Paint_DrawString_CN((ix0) , (iy0), "闲置页", &Font24CN, WHITE, BLACK);

    // save previous position for next clear
    prev_rx = rx;
    prev_ry = ry;

    ++idle_page_tick_count;
    EPD_3IN7_1Gray_Display(image_buf1);
    //EPD_3IN7_1Gray_Display_Part(image_buf1, dx0, dy0, dx1, dy1);
    //EPD_3IN7_1Gray_Display_Part(image_buf1, 0, 0, W, H);
    //printf("Rectangle at (%d,%d) to (%d,%d)\r\n", dx0, dy0, dx1, dy1);
}
// Handle partial updates for current page (drawing and displaying)
static void Display_HandlePartialUpdate(void) {
    paintConfigureForMode(1);
    Paint_Clear(WHITE);

    if (current_page == PAGE_COMMAND) {
        HandlePartialUpdate_command();
    }
    else if (current_page == PAGE_HOME) {
        //Paint_DrawTime(370, 10, painttime, &Font20, WHITE, BLACK);
        //if (modem_net)
        EPD_3IN7_1Gray_Display(image_buf1);
    }
    else if (current_page == PAGE_IDLE) {
        HandlePartialUpdate_idle();
        //EPD_3IN7_1Gray_Display_Part(image_buf1);
        //EPD_3IN7_1Gray_Display(image_buf1);
    }
    else {
        printf("No partial update\r\n");
        DEV_Delay_ms(20);
        return;
    }
}

// Display task
static void displayTask(void *pv) {
    (void)pv;
    DisplayEvent evt;
    last_activity_tick = xTaskGetTickCount();
    // Time
    //PAINT_TIME paint_time;
    //paint_time.Hour = 0;
    //paint_time.Min = 0;
    //paint_time.Sec = 0;

    for (;;) {
        // Wait for event 100ms polling if none do partial update
        if (xQueueReceive(dispQueue, &evt, pdMS_TO_TICKS(POLL_MS)) == pdTRUE) {
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
                case DISP_EVT_SLEEP: Display_Sleep(true); xSemaphoreGive(epd_mutex); continue;
                case DISP_EVT_WAKE: break;
                case DISP_EVT_SHOW_HOME: setPage(PAGE_HOME); page_change_evt = true; break;
                case DISP_EVT_SHOW_COMMAND: setPage(PAGE_COMMAND); page_change_evt = true; break;
                case DISP_EVT_SHOW_IDLE: setPage(PAGE_IDLE); page_change_evt = true; break;
                case DISP_EVT_WIFI_ON: wifi_on = true; break;
                case DISP_EVT_WIFI_CONNECTED: wifi_connected = true; break; 
                case DISP_EVT_MODEM_POWERED: modem_powered = true; break;
                case DISP_EVT_MODEM_READY: modem_ready = true; modem_powered = true; break;
                case DISP_EVT_MODEM_NET: modem_ready = true; modem_powered = true; modem_net = true; break;
                case DISP_EVT_SMS_RECEIVED: ++unread_sms; break;
                case DISP_EVT_RING: ringing = true; break;
            }
            
            // Handle screen wake & changes
            if (image_buf1 && image_buf4) {
                // Chirp screen awake
                if(!screen_on) Display_Wake();
                // Switch screen or update current fullsscreen
                if (page_change_evt) {
                    if (last_page != current_page) {
                        Display_HandleScreenChange();
                    } 
                    page_change_evt = false;
                } else {
                    Display_UpdateFullScreen();
                }
                partial_update_count = 0;
            }
            xSemaphoreGive(epd_mutex);
        }

        
        //Partial update every 500ms (internal epd limit for 1gray_display) or repaint for ghosting
        if (GRAY_MODE == 1 && screen_on) {
            if (xSemaphoreTake(epd_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                if (partial_update_count >= 60) {
                    Display_UpdateFullScreen();
                    partial_update_count = 0;
                } else {
                    Display_HandlePartialUpdate();
                    ++partial_update_count;
                }
                xSemaphoreGive(epd_mutex);
            }
            last_activity_tick = xTaskGetTickCount();
        } 
        
        // Low activity
        if (screen_on && (xTaskGetTickCount() - last_activity_tick) >= pdMS_TO_TICKS(idle_timeout_ms)) {
            printf("Activity low in displayTask.\r\n");
            last_activity_tick = xTaskGetTickCount();
            ++idle_timeout_count;
        }

        // No activity sleeping ?
        if (idle_timeout_count >= 5 && screen_on){
            Display_Sleep(false); 
        }
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
    DEV_Delay_ms(200);


    image_size4 = getImageSizeForMode(4);
    image_size1 = getImageSizeForMode(1);
    if ((image_buf4 = (UBYTE *)malloc(image_size4)) == NULL) {
        printf("ERROR: Failed to allocate image_buf4 (%u bytes)\r\n", image_size4);
        free(image_buf4);
        return;
    }
    if ((image_buf1 = (UBYTE *)malloc(image_size1)) == NULL) {
        printf("ERROR: Failed to allocate image_buf1 (%u bytes)\r\n", image_size1);
        free(image_buf1);
        return;
    }
    // Create two empty canvass so paint APIs are ready to use
    Paint_NewImage(image_buf4, EPD_3IN7_WIDTH, EPD_3IN7_HEIGHT, 270, WHITE);
    Paint_NewImage(image_buf1, EPD_3IN7_WIDTH, EPD_3IN7_HEIGHT, 270, WHITE);
    DEV_Delay_ms(200);

    // Start with displaying boot screen manually
    setPage(PAGE_BOOT);
    paintBootScreen();
    EPD_3IN7_4Gray_Display(image_buf4);
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
