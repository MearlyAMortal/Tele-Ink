# Tele-Ink
## A FreeRTOS based 4GLTE/WiFi/GPS IoT device that utilizes a E-Ink display for low power

<img src="/examples/IMG_0247.JPG" alt="Alt Text" width="400" height="600">

# Features
1. Utilizes E-Ink display for low power/holding images while off
2. USB-C Charging and 2500Mah internal battery 
3. Full Qwerty keyboard with arrow keys and sym key
4. Hologram SIM for automatic network switching +43 Country code and 13 digit number
5. Full Global Roaming 4g/3g/2g/LTE/GNSS/PPP/etc (no voice)
6. WiFi is available for connection/hosting but commented out for now to save upload time.
7. Can send/recive SMS messages and get local time and coordinate information.
8. Can send raw AT commands and see full responses using command page.
9. Idle animation similar to old VCR tape machines.
10. Full control over hardware from command page.

# Usage
* You must wait ~20 seconds for modem coldstart + system recognition for AT
* Sym+key can be used for major screen updates and or page changes
* You can change screen page using sym+key (home h, idle i, command c)
* Sleep/Wake button clears screen on sleep (sym+esc)
* Home page shows time if GNSS=ON and dynamic relevant system information
* GNSS info updates every 15s if gnss_mode=true or 45s if its false (gnss_mode != gnss_on)
* Idle page has a VCR style box bouncing around on the edges of the screen
* Command page causes keyboard to enter sequential mode for typing
* Commands start with '/' char else echo input 
* Command prompt corresponds to specifc mode you are currently in ($, AT, >, :)
* Command modes include (Base $, at mode AT, sms write >, sms read :)
* In SMS read mode you can use /s to send a message to the number of the last message read
* Command modes can be exited using /exit
* It will save state, history and text buffer unless you /clear or esc key
* Command screen is encapsulated and dynamic so text will wrap and push elements up until there is room
* Queuing display events will cause a fullscreen refresh unless they are modem status changes not on the homescreen.
* WARNING the epd api from waveshare requires 4Gray to be initlizied before fullscreen changes. (It currently only initilizes once at start) STABLE

## Commands
```
$ TEXT               - Echo mode
$ /help              - Sends you here
$ /clear             - Clear history
$ /sim on/off        - Toggle pwk on modem
$ /sim net           - Send AT+CREG? and handle URC auto
$ /esp rst           - Restart ESP32
$ /at CMD            - Send raw single line AT command
$ /at                - Enter AT mode
  AT_                  - Type AT command ex: +CREG?, +CNUM (AT is prepented)
  AT/exit              - Exit AT mode
$ /sms s NUMBER      - Start SMS send mode to NUMBER
$ /sms s             - Start SMS send mode to saved NUMBER
  > MESSAGE            - Send SMS MESSAGE to NUMBER
  > /exit              - Exit SMS send mode
$ /sms ra            - Read all messages stored on sim 
$ /sms ru            - Read unread messages stored on sim
  : ID                 - Read message number ID and store number into buffer
  : /s                 - Uses previously read messages's number and enter SMS send mode
  : /d ID              - Delete message from sim
  : /da                - Deletes all messages from sim
  : /exit              - Exit SMS read mode
$ /gnss              - Enter GNSS mode
  GNSS: on             - Turn on GNSS and wait for fix
  GNSS: off            - Turn off GNSS
  GNSS: info           - Query time and location data UTC
  GNSS: /exit          - Exit GNSS mode
```
# High level Code functionality
1. Starting at Tele-Ink.ino
2. Screen is initilized via Waveshare original code modified
3. DisplayInit() Starts a new task that can read from a queue of display events
4. DisplayTask performs updates on the screen while holding the EPD mutex and initilizes 1Gray mode before updates
5. ModemInit() Starts a new task and queue then attempts to connect to the module via "AT" -> TX after cold start
6. ModemTask waits for commands in the queue from SendAT and or reads incoming data on RX
7. ModemStatusTask checks physical status of modem by sending AT/CREG in background every ~10s
8. ModemStatusTask takes modem_mutex when sending and reading UART to not interfere with SendAT() 
9. ModemGNSSTask pings AT+CGPSINFO and updates global information held in display.h
10. ModemGNSSTask automatically calculates rough timezone offset using longitute (does not account for DST yet)
11. ModemSendAT takes modem_mutex and the original task gives back after response or givin timeout
12. KeyboardInit() Starts a new task that polls for incoming data on I2C 0x5F (keypress when not 0x00)
13. Keyboardtask will enter sequential mode and start the cmd_buffer if the selected page is command page
14. KeyboardTask doesnt need debounce (release = 1 stroke), and when a special key is encountered, posts events for other tasks
15. KeyboardTask reads keys until enter and then the global input is set and Command.cpp starts processesing 
16. Command takes a global c-string as input and returns a response into the global resp buffer as well as posting events
17. Command history and command buffer is stored globally in hisplay.h struct until /clear
18. Command buffer is accessed atomically by display to show user text(producer/viewer)
19. Command buffer is accessed atomically for modem commands and updating responses (producer/consumer)
20. Command history is saved and can be accessed by using arrow keys
21. WiFi is underdeveloped at the moment but can poll for nearby networks or connect to one.
22. All of the heavy tasks have a timeout feature that will increase time between loops to save resources
23. Every 60 partial updates send fullscreen refresh event to prevent ghosting in daylight or use Sym+P to repaint
24. Display_1Gray_Part rotation coordinates not working so ~500ms refreshrate is best so far
25. Start at the homescreen and shift to idlescreen when no acitivity is detected

# Tech Stack
## Hardware/Communication
* Waveshare ESP32-WROOM-32E E-Ink driver module
* Waveshare E-Ink display black/white 3.7 inch
* Waveshare SIM7600G-H Modem module
* LTE antenna I-P.EX Unobstructed
* GNSS Cirocomm I-P.EX Unobstructed
* Hologram SIM card (not using API)
* M5Stack CardKB v1.1
* AmpRipper 4000 v1.0 PSU/BMS
* AdaFruit 1sLiPo 2500mAh battery
* UART (ESP32 <-> Modem)
* SPI (ESP32 <-> E-Ink)
* I2C (ESP32 <-> Keyboard)
## Software/Development Env
* C/C++
* Arduino
* FreeRTOS

# Hardware Modifications
* Replaced 471 SMD resistors on M5Stack keyboard with 472 (3.3v logic) (clk/dat)
* Replaced SMD resistor on AmpRipper for 1A max batt charge rate (AdaFruit Batt Max)
* Switch on ESP32 Driver board 1 = A  2 = ON

# Goals
1. Fix partial update buffer rotation coordinates (Waveshare API broken)
2. Add connections between BMS and ESP for battery info
3. Add RAW data mode AT+CMGF=0 but need encoding 
4. Add GNSS automatic DST calculation based on month/day/location?
5. Add PPP dial up support for remote WiFi hosting (unlikely and SLOW)

# Diagram
```
                  ┌──────────────────────────────────┐                                                            
                  │                                  │                                                            
                  │                                  ◄─────────────┐                                              
                  │                                  ◄───────────┐ │                                              
                  │        2500mAh LiPo 1s           │           │ │                DATA                          
                  │                                  │           │ │            ──────────────                    
                  │                                  │           │ │                POWER                         
                  │                                  │           │ │            ─────────────►                    
                  └──────────────────────────────────┘           │ │                                              
                                                                 │ │                                              
                                                                 │ │    USB IN                                    
                                                                 │ │       │                                      
                                                                 │ │       │                                      
                                                            ┌────▼─▼───────▼─────┐                                
                                                            │                    ┼────────────────────────────┐   
  ┌──────────────────────────────────────────┐              │                    ┼──────────────────────────┐ │   
  │               3.7" E-INK                 │              │     AMP Ripper     │                          │ │   
  │  ┌────────────────────────────────────┐  │              │      PSU/BMS       │                          │ │   
  │  │                                    │  │              │               xx   │                          │ │   
  │  │                                    │  │              └─┬─┬────────────────┘                          │ │   
  │  │                                    │  │         ┌──────┼ │                                           │ │   
  │  │                                    │  │         │     ┌┼┐│                                           │ │   
  │  │                                    │  │         │ ┌───┘│└┼                                           │ │   
  │  │                                    │  │         │ │    │ │                                           │ │   
  │  │                                    │  │         │ │    │ │                                           │ │   
  │  │ Cleared History!                   │  │         │ │    │ │                                           │ │   
  │  │ $ _                                │  │        ┌┘┌┘┌───▼─▼─────────┐                                 │ │   
  │  └────────────────────────────────────┘  ├────────┼─┼─┤               │     GND      ┌──────────────────▼─▼──┐
  │                                          │  SPI 4 │ │ │               ┼──────────────┼                       │
  └──────────────────────────────────────────┴────────┼─┼─┼               │    17-TXD    │                       │
                                                      └┐└┐│    ESP32W     ┼──────────────┤                       │
┌─────────────────────────────────────────────┐        │ ││               │    16-RXD    │                       │
│          M5Stack QWERTY Keyboard            ◄────────┘ ││               ┼──────────────┤                       │
│                                             ◄──────────┘│               │    4-PWK     │                       │
│                                             │           │               ┼──────────────┤      SIM7600G-H       │
│                                             │           └──┬──┬─────────┘              │                       │
│                                             │  DATA-21     │  │                        │                       │
│                                        472  ┼──────────────┘  │                        │                       │
│                                             │  CLK-22         │                        │                       │
│                                        472  ┼─────────────────┘              LTE───────┤                       │
│                                             │                               GNSS───────┤                       │
│                                             │                                          └───────────────────────┘
│                                             │                                                                   
│                                             │                                                                   
└─────────────────────────────────────────────┘                                                                   
```
## Sources
* https://www.waveshare.com/wiki/SIM7600G-H_4G_Module?srsltid=AfmBOoow9ujjO1206nzUsXVMr9o72mcQgfmwjMp-vEK3QVuOIhEVSpLd
* https://www.waveshare.com/wiki/E-Paper_ESP32_Driver_Board
* https://www.waveshare.com/wiki/E-Paper_API_Analysis?srsltid=AfmBOoqeSFM_rVWAgtaWrzRiQB7OrI90iRIy5Up24zcl5dhIv23WO0H_
* https://www.digikey.com/en/resources/conversion-calculators/conversion-calculator-ohms
* https://www.waveshare.com/w/upload/3/39/SIM7080_Series_AT_Command_Manual_V1.02.pdf?srsltid=AfmBOopDPUJbcJVsTY_KqjYtSbKTreGlTCFfHh68Vml8m4Wp6ItrzICq
* https://docs.m5stack.com/en/unit/cardkb_1.1
* https://www.tindie.com/products/kickstart_design/ampripper-4000-5v-4a-lipo-li-ion-battery-charger/
* https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/freertos.html
* https://content.u-blox.com/sites/default/files/products/documents/GNSS-Antennas_AppNote_%28UBX-15030289%29.pdf
* https://www.w3schools.com/c/c_ref_string.php

