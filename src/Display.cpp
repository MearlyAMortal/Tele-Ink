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
bool modem_ready = false;
bool modem_net = false;
bool modem_powered = false;
uint8_t modem_mode = 1;   // Default text mode not PDU mode
bool sms_send = false;
bool sms_read = false;
bool sms_read_all = false;
int sms_count = 0;
bool at_mode = false;
bool gnss_mode = false;
bool wifi_mode = false;
CommandBuffer cmd_buffer = {0};
GNSSData gnss_data = {0};
WifiData wifi_data = {0};
SignalData signal_data = {0};
// Private
// Screen
static UBYTE *image_buf1 = NULL;
static UWORD image_size1 = 0;
static UBYTE *image_buf4 = NULL;
static UWORD image_size4 = 0;
static uint8_t GRAY_MODE = 4;
static uint32_t display_w = 0;
static uint32_t display_h = 0;
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
static bool ringing = false;
// Paint
static char idle_c[2] = {0};


// Public function for setting last activity tick in display loop for resetting idle timer
void SetLastActivityTick(void) {
    last_activity_tick = xTaskGetTickCount();
    idle_timeout_count = 0; // Reset idle timeout count on activity
}

// Returns the image buffer size for a given gray mode
static UWORD getImageSizeForMode(uint8_t gray) {
    if (gray == 4) {
        return ((EPD_3IN7_WIDTH % 4 == 0) ? (EPD_3IN7_WIDTH / 4) : (EPD_3IN7_WIDTH / 4 + 1)) * EPD_3IN7_HEIGHT;
    } else {
        return ((EPD_3IN7_WIDTH % 8 == 0) ? (EPD_3IN7_WIDTH / 8) : (EPD_3IN7_WIDTH / 8 + 1)) * EPD_3IN7_HEIGHT;
    }
}
// Configure paint API and specific image buffer for givin gray mode 
static void paintConfigureForMode(uint8_t gray) {
    if (gray == 4 && image_buf4) {
        Paint_SelectImage(image_buf4);
        Paint_SetScale(4);
    } else if (gray == 1 && image_buf1) {
        Paint_SelectImage(image_buf1);
        Paint_SetScale(2);
    }
}
// Set the current page and update the last page
static void setPage(PageType page) {
    last_page = current_page;
    current_page = page;
}
// Painting the screen buffer with full screen static image for later interpolation or holding
static void paintHomeScreen(void) {
    paintConfigureForMode(4);
    Paint_Clear(WHITE);
    // title
    Paint_DrawString_EN(10, 5, "Tele-Ink", &Font24, WHITE, BLACK);
    //Paint_DrawString_EN(160, 10, "Version 0.2.6", &Font12, WHITE, BLACK);
    // Seperator line
    Paint_DrawLine(5, 30, Font24.Width * 16, 30, BLACK, DOT_PIXEL_2X2, LINE_STYLE_SOLID);


    // Vertical split
    Paint_DrawLine((display_w/2) + 40, 1, (display_w/2) + 40, (display_h/2) + 40, BLACK, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    // bottom split
    Paint_DrawLine(1, (display_h/2) + 40, display_w-1, (display_h/2) + 40, BLACK, DOT_PIXEL_2X2, LINE_STYLE_SOLID);


    // Modem text mode
    char buf[64] = {0};
    if (at_mode) {
        snprintf(buf, sizeof(buf), "CMD Mode: AT", at_mode);
    }
    else if (sms_read || sms_count) {
        snprintf(buf, sizeof(buf), "CMD Mode: SMS", at_mode);
    }
    else if (gnss_mode) {
        snprintf(buf, sizeof(buf), "CMD Mode: GNSS", at_mode);
    }
    else {
        snprintf(buf, sizeof(buf), "CMD Mode: Base", at_mode);
    }
    Paint_DrawString_EN(10, 40, buf, &Font16, BLACK, WHITE);

    //  status
    if (modem_powered && modem_ready){
        Paint_DrawString_EN(10, 60, "Modem +", &Font16, BLACK, WHITE);
        if (modem_net) {
            Paint_DrawString_EN(10, 60, "Network +", &Font16, BLACK, WHITE);
        } 
    } else {
        Paint_DrawString_EN(10, 60, "Modem -", &Font16, BLACK, WHITE);
    }

    // GNSS
    if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        if (gnss_data.gnss_on) {
            Paint_DrawString_EN(10, 80, "GNSS +", &Font16, BLACK, WHITE);
        } else {
            Paint_DrawString_EN(10, 80, "GNSS -", &Font16, BLACK, WHITE);
        }
        xSemaphoreGive(gnss_data.mutex);
    } else {
        Paint_DrawString_EN(10, 80, "GNSS ?", &Font16, BLACK, WHITE);
    }
    
    // SMS (temporary)
    if (sms_count > 0 && !sms_read) {
        char buf[32];
        snprintf(buf, sizeof(buf), "SMS New: %d", sms_count);
        Paint_DrawString_EN(10, 100, buf, &Font16, BLACK, WHITE);
    } else {
        Paint_DrawString_EN(10, 100, "SMS: 0", &Font16, BLACK, WHITE);
    }

    // WiFi
    if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        if (wifi_data.wifi_on) {
            Paint_DrawString_EN(10, 120, "WiFi +", &Font16, BLACK, WHITE);
            Paint_DrawString_EN(15, 140, "Idle", &Font16, WHITE, BLACK);
            if (wifi_data.wifi_connected) {
                Paint_DrawString_EN(10, 120, "WiFi ++", &Font16, BLACK, WHITE);
                Paint_DrawString_EN(15, 140, "Connected: SSID", &Font16, WHITE, BLACK);
            } else if (wifi_data.wifi_scan) {
                Paint_DrawString_EN(10, 120, "WiFi ++", &Font16, BLACK, WHITE);
                Paint_DrawString_EN(15, 140, "Scanning Networks", &Font16, WHITE, BLACK);
            } else if (wifi_data.wifi_host) {
                Paint_DrawString_EN(10, 120, "WiFi ++", &Font16, BLACK, WHITE);
                Paint_DrawString_EN(15, 140, "Hosting: SSID", &Font16, WHITE, BLACK);
            }
        } else {
            Paint_DrawString_EN(10, 120, "WiFi -", &Font16, BLACK, WHITE);
        }
        xSemaphoreGive(wifi_data.mutex);
    } 

    // Parse signal type and corresponding strength
    if (xSemaphoreTake(signal_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        char result[32] = {0};
        // find current network type and corresponding signal strength/quality
        // LTE (4G)
        if (signal_data.rsrq != 255 && signal_data.rsrp != 255) {
            if (signal_data.rsrq >= 0 && signal_data.rsrq <= 9) {
                // poor signal
                snprintf(result, sizeof(result), "4G LTE :(");
            } else if (signal_data.rsrq >= 10 && signal_data.rsrq <= 19) {
                // moderate signal
                snprintf(result, sizeof(result), "4G LTE :|");
            } else if (signal_data.rsrq >= 20 && signal_data.rsrq <= 30) {
                // good signal
                snprintf(result, sizeof(result), "4G LTE :)");
            } else if (signal_data.rsrq > 30) {
                // excellent signal
                snprintf(result, sizeof(result), "4G LTE :D");
            }
        } 
        // 3G fallback (UMTS/WCDMA)
        else if (signal_data.rscp != 255 && signal_data.ecno != 255) {
            snprintf(result, sizeof(result), "3G");
        }
        // 2G unlikely fallback (GSM)
        else if (signal_data.rxlev != 99 && signal_data.ber != 99) {
            snprintf(result, sizeof(result), "2G");
        }   
        // No signal
        else {
            snprintf(result, sizeof(result), "No Service");
        }
        Paint_DrawString_EN((display_w/2) + 30 - (strlen(result) * Font16.Width), 8, result, &Font16, WHITE, BLACK);

        xSemaphoreGive(signal_data.mutex);
    }

    // GNSS data (must be last)
    if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        // 
        if (gnss_data.time[0] == '\0') {
            Paint_DrawString_EN(display_w - 10 - (Font16.Width * strlen("GNSS Unavailable")), 5, "GNSS Unavailable", &Font16, BLACK, WHITE);
            xSemaphoreGive(gnss_data.mutex);
            return;
        }
        char result[64] = {0};
        double lon = gnss_data.longitude;
        // rouch longitute math for calculating local time from utc for display, not accounting for daylight savings or anything, just rough
        int local_time_offset = (int)(lon / 15); // 15 degrees of longitude per hour
        snprintf(result, sizeof(result), "Local Time UTC%+d", local_time_offset);
        Paint_DrawString_EN(display_w - 10 - (Font16.Width * strlen(result)), 10, result, &Font16, BLACK, WHITE);
        // Draw sun or moon based on rough local time, not accounting for date or anything, just rough
        int local_hour = 0;
        if (sscanf(gnss_data.time, "%2d", &local_hour) == 1) {
            local_hour = (local_hour + 24) % 24; // wrap around 24 hours
            if (local_hour >= 6 && local_hour < 18) {
                // Daytime: draw sun
                Paint_DrawCircle((display_w/2) + 70, (display_h/2)+1, 20, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
            } else {
                // Nighttime: draw moon
                Paint_DrawCircle((display_w/2) + 70, (display_h/2)+1, 20, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
                Paint_DrawCircle((display_w/2) + 65, (display_h/2)+1, 20, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
            }
        }

        memset(result, 0, sizeof(result));
        snprintf(result, sizeof(result), "Time: %s", gnss_data.time);
        Paint_DrawString_EN(display_w - 10 - (Font12.Width * strlen(result)), 30, result, &Font12, WHITE, BLACK);
        memset(result, 0, sizeof(result));
        snprintf(result, sizeof(result), "Date: %s", gnss_data.date);
        Paint_DrawString_EN(display_w - 10 - (Font12.Width * strlen(result)), 50, result, &Font12, WHITE, BLACK);
        memset(result, 0, sizeof(result));
        snprintf(result, sizeof(result), "Latitude: %.6f", gnss_data.latitude);
        Paint_DrawString_EN(display_w - 10 - (Font12.Width * strlen(result)), 70, result, &Font12, WHITE, BLACK);
        memset(result, 0, sizeof(result));
        snprintf(result, sizeof(result), "Longitude: %.6f", gnss_data.longitude);
        Paint_DrawString_EN(display_w - 10 - (Font12.Width * strlen(result)), 90, result, &Font12, WHITE, BLACK);
        memset(result, 0, sizeof(result));
        snprintf(result, sizeof(result), "Altitude M: %s", gnss_data.altitude);
        Paint_DrawString_EN(display_w - 10 - (Font12.Width * strlen(result)), 110, result, &Font12, WHITE, BLACK);
        memset(result, 0, sizeof(result));
        snprintf(result, sizeof(result), "Speed KN: %s", gnss_data.speed);
        Paint_DrawString_EN(display_w - 10 - (Font12.Width * strlen(result)), 130, result, &Font12, WHITE, BLACK);
        xSemaphoreGive(gnss_data.mutex);
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
    Paint_DrawString_EN(10, 5, "Tele-Ink v0.2.4", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(10, 20, "By: Logan Puntous", &Font12, WHITE, BLACK);
    Paint_DrawNum(10, 33, 123456789, &Font12, BLACK, WHITE);
    Paint_DrawNum(10, 50, 987654321, &Font16, WHITE, BLACK);
    Paint_DrawString_EN(10, 150, "You can change modes w/ sym", &Font24, BLACK, GRAY1);
    Paint_DrawString_EN(10, 175, "In command mode use  /<cmd>", &Font24, WHITE, GRAY2);
    Paint_DrawString_EN(10, 200, "You can execute AT commands", &Font24, WHITE, GRAY3);
    Paint_DrawString_EN(10, 225, "Global Roaming GNSS 4G Data", &Font24, WHITE, GRAY4);
}
// Paint the current page based on internal state using specifc paint function
static void paintCurrentPage(void) {
    switch (current_page) {
        case PAGE_HOME:    paintHomeScreen(); break;
        case PAGE_IDLE:    paintBlankScreen(); break;
        case PAGE_COMMAND: paintCommandScreen(); break;
        default: break;
    }
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

    // Somehow working where I initilize 1 gray and can do 4 gray updates? Im just going with it since its faster.
    // Init 1Gray
    if (GRAY_MODE == 1) {
        printf("Initializing 1Gray mode wake\r\n");
        EPD_3IN7_1Gray_Init();
        DEV_Delay_ms(10);
    } else {
        // Init 4Gray
        //printf("Initializing 4Gray mode not\r\n");
        printf("Initializing 1Gray mode wake supposed to be 4\r\n");
        EPD_3IN7_1Gray_Init();
        //EPD_3IN7_4Gray_Init(); 
        DEV_Delay_ms(10);
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

// Public call to post display event for queue to consume and handle in display task
bool Display_PostEvent(const DisplayEvent *evt, TickType_t ticksToWait) {
    if (!dispQueue) return false;
    return xQueueSend(dispQueue, evt, ticksToWait) == pdTRUE;
}
// Public easy access display event calls for common events
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

// Public call to clear the shared command history buffer in display task
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
    //printf("Display_HandleScreenChange\r\n");
    if (current_page == PAGE_NONE) return;

    paintCurrentPage();
    EPD_3IN7_4Gray_Display(image_buf4);
    
    // Start partial updates if needed
    if (current_page == PAGE_IDLE || current_page == PAGE_COMMAND) {
        // Switch to 1 gray for partial updates
        printf("Initializing 1Gray mode HSC\r\n");
        EPD_3IN7_1Gray_Init();
        DEV_Delay_ms(10);
        GRAY_MODE = 1;
    } else {
        // Init 4Gray
        //printf("Initializing 4Gray mode not\r\n");
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
    if (cmd_buffer.mutex && xSemaphoreTake(cmd_buffer.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
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
    } else if (gnss_mode){
        snprintf(display_line, sizeof(display_line), "GNSS: %s_", current_input);
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

// Clear screen, small pseudo random animation to prevent burn in during idle
static void HandlePartialUpdate_idle(void) {
    // small bouncing rectangle (VCR-style) animation
    // x and y are reversed! W = .x
    static bool init = false;
    static int rx, ry, rw, rh;
    static int prev_rx, prev_ry;
    static int vx, vy;
    idle_c[1] = '\0';
    if (idle_c[0] < 'A' || idle_c[0] > 'Z') {
        idle_c[0] = 'A';
    }

    int margin = 4;
    int ex0, ey0, ex1, ey1;
    int dx0, dy0, dx1, dy1;
    int ix0, iy0, ix1, iy1;
    int cx, cy;
    

    if (!init) {
        rw = 64; rh = 64;
        rx = (display_w - rw) / 2;
        ry = (display_h - rh) / 2;
        prev_rx = rx; 
        prev_ry = ry;
        // seed pseudo-random with tick count
        srand((unsigned) xTaskGetTickCount());
        do { vx = (int)(rand() % 8) + 4; } while (vx == 0);
        do { vy = (int)(rand() % 8) + 4; } while (vy == 0);
        init = true;
    }

    // erase previous rectangle area with a small margin
    margin = 4;
    ex0 = prev_rx - margin; if (ex0 < 0) ex0 = 0;
    ey0 = prev_ry - margin; if (ey0 < 0) ey0 = 0;
    ex1 = prev_rx + rw + margin; if (ex1 >= display_w) ex1 = display_w - 1;
    ey1 = prev_ry + rh + margin; if (ey1 >= display_h) ey1 = display_h - 1;
    Paint_ClearWindows(ex0, ey0, ex1, ey1, WHITE);

    // update position
    rx += vx;
    ry += vy;
    // bounce on horizontal edges
    if (rx <= 0) { // Left
        rx = 0;
        vx = -vx;
        vx += (int)(rand() % 3) - 1;
        idle_c[0] = ((idle_c[0]+1) - 'A') % 26 + 'A';
    } else if (rx + rw > display_w - 1) { // Right
        rx = (display_w - 1) - (rw - 1);
        if (rx < 0) rx = 0;
        vx = -vx;
        vx += (int)(rand() % 3) - 1;
        idle_c[0] = ((idle_c[0]+1) - 'A') % 26 + 'A';
    }
    // bounce on vertical edges
    if (ry <= 0) { // Top
        ry = 0;
        vy = -vy;
        vx += (int)(rand() % 3) - 1;
        idle_c[0] = ((idle_c[0]+1) - 'A') % 26 + 'A';
    } else if (ry + rh - 1 > display_h - 1) { // Bottom
        ry = (display_h - 1) - (rh - 1);
        if (ry < 0) ry = 0;
        vy = -vy;
        vx += (int)(rand() % 3) - 1;
        idle_c[0] = ((idle_c[0]+1) - 'A') % 26 + 'A';
    }

    // outer rectangle - clamp draw coordinates to valid inclusive range
    dx0 = rx; if (dx0 < 0) dx0 = 0;
    dy0 = ry; if (dy0 < 0) dy0 = 0;
    dx1 = rx + rw - 1; if (dx1 > display_w - 1) dx1 = display_w - 1;
    dy1 = ry + rh - 1; if (dy1 > display_h - 1) dy1 = display_h - 1;
    Paint_DrawRectangle(dx0, dy0, dx1, dy1, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    // inner rectangle - compute and clamp, draw only if valid
    ix0 = dx0 + 4; 
    iy0 = dy0 + 4;
    ix1 = dx1 - 4; 
    iy1 = dy1 - 4;
    Paint_DrawRectangle(ix0, iy0, ix1, iy1, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    // picture in picture
    cx = dx0 + ((dx1 - dx0 + 0) - Font24.Width) / 2;
    cy = dy0 + ((dy1 - dy0 + 3) - Font24.Height) / 2;
    Paint_DrawString_EN(cx, cy, idle_c, &Font24, WHITE, BLACK);
    //Paint_DrawString_CN(cx, cy, idle_c, &Font12CN, WHITE, BLACK);

    //DEBUG
    //Paint_DrawRectangle(0, 0, display_w, display_h, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    
    // Display final image
    // Allocate region buffer'
    /*
    UWORD region_h = dy1 - dy0 + 1;
    UWORD region_w = dx1 - dx0 + 1;
    UWORD region_bytes_per_row = (region_w + 7) / 8;
    UWORD full_bytes_per_row = (display_w + 7) / 8;
    UBYTE region_buf[region_bytes_per_row * region_h];
    memset(region_buf, 0, region_bytes_per_row * region_h);
    // Copy region from main image buffer to region buffer
    for (UWORD y = 0; y < region_h; y++) {
        for (UWORD x = 0; x < region_w; x++) {
            UWORD src_x = dx0 + y;
            UWORD src_y = dy0 + x;

            UWORD src_byte = src_x * full_bytes_per_row + (src_y / 8);
            UBYTE src_bit = 7 - (src_y % 8);
            UBYTE pixel = (image_buf1[src_byte] >> src_bit) & 0x01;

            UWORD dst_byte = y * region_bytes_per_row + (x / 8);
            UBYTE dst_bit = 7 - (x % 8);
            if (pixel)
                region_buf[dst_byte] |= (1 << dst_bit);
        }
    }
    */
    
    // DEBUG
    EPD_3IN7_1Gray_Display(image_buf1);
    //EPD_3IN7_1Gray_Display(region_buf);
    //EPD_3IN7_1Gray_Display_Part(image_buf1, dx0, dy0, dx1, dy1);
    //EPD_3IN7_1Gray_Display_Part(image_buf1, dy0, dx0, dy1, dx1);
    //EPD_3IN7_1Gray_Display_Part(region_buf, dy0, dx0, dy1, dx1);
    //EPD_3IN7_1Gray_Display_Part(region_buf, dy0, display_w - 1 - dx1, dy1, display_w - 1 - dx0); //90
    //EPD_3IN7_1Gray_Display_Part(region_buf, dy0, display_w - 1 - dx1, dy1, display_w - 1 - dx0); //270
    //EPD_3IN7_1Gray_Display_Part(region_buf, 0, display_w - 1 - 64, 64, display_w - 1 - 0); //270 regularized
    //EPD_3IN7_1Gray_Display_Part(region_buf, 0, 0, 64, 64);
    // Rotate 180 degrees from 270 since the api regularizes axis to 90 degree rotation for this function? :)
    //EPD_3IN7_1Gray_Display_Part(region_buf, display_w - 1 - dx1, display_h - 1 - dy1, display_w - 1 - dx0, display_h - 1 - dy0); //180
    
    // save previous position for next clear
    ++idle_page_tick_count;
    prev_rx = rx;
    prev_ry = ry;
}

// Handle partial updates for current page (drawing and displaying)
static void Display_HandlePartialUpdate(void) {
    paintConfigureForMode(1);
    Paint_Clear(WHITE);

    if (current_page == PAGE_COMMAND) {
        HandlePartialUpdate_command();
    }
    else if (current_page == PAGE_IDLE) {
        HandlePartialUpdate_idle();
    }
    else {
        printf("No partial update\r\n");
        DEV_Delay_ms(20);
        return;
    }
}

// Reset signal_data to unknown values (for display) on modem lost or reset
void SignalData_Reset(void) {
    if (signal_data.mutex && xSemaphoreTake(signal_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        signal_data.rxlev = 99;
        signal_data.ber = 99;
        signal_data.rscp = 255;
        signal_data.ecno = 255;
        signal_data.rsrq = 255;
        signal_data.rsrp = 255;
        xSemaphoreGive(signal_data.mutex);
    }
}

// Reset all public modes back to default if modem is lost or reset
void ResetGlobalModeState(void) {
    // AT
    at_mode = false;
    // SMS
    sms_send = false;
    sms_read = false;
    sms_read_all = false;
    sms_count = 0;
    // GNSS
    gnss_mode = false;
    if (xSemaphoreTake(gnss_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        gnss_data.gnss_on = false;
        xSemaphoreGive(gnss_data.mutex);
    }
    // Wifi
    wifi_mode = false;
    if (xSemaphoreTake(wifi_data.mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        wifi_data.wifi_on = false;
        wifi_data.wifi_connected = false;
        wifi_data.wifi_scan = false;
        wifi_data.wifi_host = false;
        xSemaphoreGive(wifi_data.mutex);
    }
}

// Display task
static void displayTask(void *pv) {
    (void)pv;
    DisplayEvent evt;
    last_activity_tick = xTaskGetTickCount();
    idle_c[0] = 'A'; // Start at 'A' for idle character

    // Set signal to unkown values to start
    // Mutex isnt created yet becuase this task starts before modemTask
    signal_data.rxlev = 99;
    signal_data.ber = 99; 
    signal_data.rscp = 255;
    signal_data.ecno = 255;
    signal_data.rsrq = 255;
    signal_data.rsrp = 255;

    // Set wifi to unkown values to start
    // Mutex isnt created yet becuase this task starts before modemTask
    wifi_data.wifi_on = false;
    wifi_data.wifi_scan = false;
    wifi_data.wifi_connected = false;
    wifi_data.ssid[0] = '\0';
    wifi_data.password[0] = '\0';
    wifi_data.wifi_host = false;

    // Main loop, initilization finished ATP
    for (;;) {
        // Wait for event 100ms polling if none do partial update
        if (xQueueReceive(dispQueue, &evt, pdMS_TO_TICKS(POLL_MS)) == pdTRUE) {
            // Take screen mutex
            if (!epd_mutex || (xSemaphoreTake(epd_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)) {
                printf("displayTask: failed to take epd_mutex for event (timeout)\r\n");
                DEV_Delay_ms(POLL_MS*2);
                continue;
            }
            SetLastActivityTick();
            // update internal state based or wake/sleep on event
            switch (evt.type) {
                case DISP_EVT_SLEEP: Display_Sleep(true); xSemaphoreGive(epd_mutex); continue;
                case DISP_EVT_WAKE: break;
                case DISP_EVT_SHOW_HOME: setPage(PAGE_HOME); page_change_evt = true; break;
                case DISP_EVT_SHOW_COMMAND: setPage(PAGE_COMMAND); page_change_evt = true; break;
                case DISP_EVT_SHOW_IDLE: setPage(PAGE_IDLE); page_change_evt = true; break;
                case DISP_EVT_MODEM_POWERED: modem_powered = true; break;
                case DISP_EVT_MODEM_READY: modem_ready = true; modem_powered = true; break;
                case DISP_EVT_MODEM_NET: modem_ready = true; modem_powered = true; modem_net = true; break;
                case DISP_EVT_MODEM_LOST: modem_ready = false; modem_net = false; SignalData_Reset(); ResetGlobalModeState(); break;
                case DISP_EVT_SMS_RECEIVED: ++sms_count; break;
                case DISP_EVT_RING: ringing = true; break;
            }

            // Only update homescreen with external modem state changes 
            if (evt.type == DISP_EVT_MODEM_READY || evt.type == DISP_EVT_MODEM_NET || evt.type == DISP_EVT_MODEM_LOST
                || evt.type == DISP_EVT_SMS_RECEIVED || evt.type == DISP_EVT_RING) {
                if (current_page != PAGE_HOME) {
                    xSemaphoreGive(epd_mutex);
                    continue;
                }
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
                    if (current_page == PAGE_IDLE) {
                        Display_UpdateFullScreen();
                    }
                    partial_update_count = 0;
                } else {
                    Display_HandlePartialUpdate();
                    ++partial_update_count;
                }
                xSemaphoreGive(epd_mutex);
            }
            SetLastActivityTick();
        } 
        
        // Low activity
        if (screen_on && current_page != PAGE_IDLE && (xTaskGetTickCount() - last_activity_tick) >= pdMS_TO_TICKS(idle_timeout_ms)) {
            printf("Activity low in displayTask.\r\n");
            last_activity_tick = xTaskGetTickCount();
            ++idle_timeout_count;
        }

        // No activity show idle page
        if (screen_on && idle_timeout_count >= 3 && current_page != PAGE_IDLE) {
            printf("Switching to idle page due to inactivity.\r\n");
            Display_Event_ShowIdle();
            idle_timeout_count = 0; 
        }
    }
}

// Start display task if not already running
static void Display_StartTask(void) {
    if (!display_task_handle) {
        screen_on = true;
        xTaskCreatePinnedToCore(displayTask, "display", 8192, NULL, 2, &display_task_handle, 0);
    }
}

// Initialize display ds and start display task (does not send any events)
void Display_Init(void) {
    // Mutex creation for displayTask and command buffer
    if (!epd_mutex && !cmd_buffer.mutex) {
        // Main mutex for display ops
        epd_mutex = xSemaphoreCreateMutex();
        // Mutex for shared command buffer
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
    display_w = EPD_3IN7_HEIGHT;
    display_h = EPD_3IN7_WIDTH;
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
