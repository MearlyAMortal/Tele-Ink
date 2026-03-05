#include "src/DEV_Config.h"
#include "src/EPD.h"
#include "src/GUI_Paint.h"
#include "src/Display.h"
#include "src/Modem.h"
#include "src/Keyboard.h"
//#include "src/ESP32_WiFi.h"
#include "ImageData.h"
#include "credentials.h"
#include <stdlib.h>


#define MODEM_RX_PIN 16
#define MODEM_TX_PIN 17
#define MODEM_POWER_PIN 4
#define KEYBOARD_I2C 0x5F
#define KEYBOARD_SDA_PIN 21 //14 <used by screen>
#define KEYBOARD_SCL_PIN 22 //13 <used by screen>



void setup() {
    Serial.begin(115200);
    DEV_Module_Init();

    // Display
    Display_Init(); 

    // Modem
    Modem_Init(&Serial2, MODEM_RX_PIN, MODEM_TX_PIN, MODEM_POWER_PIN);

    // Keyboard
    uint8_t addr7 = KEYBOARD_I2C;
    Wire.begin(KEYBOARD_SDA_PIN, KEYBOARD_SCL_PIN);
    Keyboard_Init(&Wire, addr7);
    
    // Starting page
    Display_Event_ShowHome();
    printf("Setup complete!\r\n");

}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
