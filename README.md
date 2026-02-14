# Tele-Ink
## A FreeRTOS based 4G/WiFi IoT device that utilizes a E-Ink display for low power

<img src="/examples/IMG_0247.JPG" alt="Alt Text" width="400" height="600">

# Usage
* You must wait ~20 seconds for modem coldstart + system recognition for AT
* You can change screen page using sym+key (home h, idle i, command c)
* Sleep/Wake button clears screen on sleep (sym+esc)
* Home page shows time(soon) and dynamic relevant system information
* Idle page has a VCR style box bouncing around on the edges of the screen
* Display_1Gray_Part not working so ~500ms refreshrate is best so far
* Command page causes keyboard to enter sequential mode for typing
* Commands start with / else echo input 
* Command buffer is accessed atomically by display to show user text(producer/viewer)
* Command history is saved and can be accessed by using arrow keys
* Command prompt corresponds to specifc mode you are currently in ($, AT, >, :)
* Command modes include (Base $, at mode AT, sms read :, sms write >)
* Command modes can be exited using /exit
* It will save state, history and text buffer unless you /clear or esc from page
* Command screen is encapsulated and dynamic so text will wrap and push elements up until there is room
* Queuing display events will cause a fullscreen refresh

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
  AT_                  - Type AT command ex: H, +CNUM (AT is prepented)
  AT/exit              - Exit AT mode
$ /sms s NUMBER      - Start SMS send mode to NUMBER
$ /sms s             - Start SMS send mode to saved NUMBER
  > MESSAGE            - Send SMS MESSAGE to NUMBER
  > /exit              - Exit SMS send mode
$ /sms ra            - Read all messages stored on sim 
$ /sms ru            - Read unread messages stored on sim
  : ID                 - If there are X messages to read choose idx starting at 0
  : /d ID              - Delete message from sim
  : /exit              - Exit SMS read mode
```
# High level Code functionality
1. Starting at Tele-Ink.ino
2. Screen is initilized via Waveshare original code modified
3. DisplayInit() Starts a new task that can read from a queue of display events
4. DisplayTask performs updates on the screen while holding the EPD mutex
5. ModemInit() Starts a new task and queue then attempts to connect to the module via "AT" -> TX
6. ModemTask waits for commands in the queue and or reads incoming data 
7. ModemSendAT takes semaphore and the original task gives back (for response safety)
8. KeyboardInit() Starts a new task that polls for incoming data on I2C 0x5F
9. KeyboardTask doesnt need debounce (release = 1 stroke), and when a key is encountered, posts events for other tasks
10. WiFi is underdeveloped at the moment but can poll for nearby networks or connect to one.
11. All of the tasks have a timeout feature that will increase time between loops to save resources
12. Every 60 partial updates send fullscreen refresh to prevent ghosting

# Tech Stack
## Hardware/Communication
* Waveshare E-Ink ESP32-WROOM-32E driver module
* Waveshare E-Ink display black/white 3.7 inch
* Waveshare SIM7600G-H Modem module
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
* Replaced 471 SMD resistors on M5Stack keyboard with 472 (3.3v logic)
* Replaced SMD resistor on AmpRipper for 1A max batt charge rate
* Switch on ESP32 Driver board 1 = A  2 = ON

# Diagram
```
                  ┌──────────────────────────────────┐                                                            
                  │                                  │                                                            
                  │                                  ┼─────────────┐                                              
                  │                                  ┼───────────┐ │                                              
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
  │  ┌────────────────────────────────────┐  │              │                    │                          │ │   
  │  │                                    │  │              │               xx   │                          │ │   
  │  │                                    │  │              └─┬─┬────────────────┘                          │ │   
  │  │                                    │  │         ┌──────┼ │                                           │ │   
  │  │                                    │  │         │     ┌┼┐│                                           │ │   
  │  │                                    │  │         │ ┌───┘│└┼                                           │ │   
  │  │                                    │  │         │ │    │ │                                           │ │   
  │  │                                    │  │         │ │    │ │                                           │ │   
  │  │                                    │  │         │ │    │ │                                           │ │   
  │  │                                    │  │        ┌┘┌┘┌───▼─▼─────────┐                                 │ │   
  │  └────────────────────────────────────┘  ├────────┼─┼─┤               │     GND      ┌──────────────────▼─▼──┐
  │                                          │    SPI │ │ │               ┼──────────────┼                       │
  └──────────────────────────────────────────┴────────┼─┼─┼               │    17-TXD    │                       │
                                                      └┐└┐│    ESP32      ┼──────────────┤                       │
┌─────────────────────────────────────────────┐        │ ││               │    16-RXD    │                       │
│              M5Stack Keyboard               ◄────────┘ ││               ┼──────────────┤                       │
│                                             ◄──────────┘│               │     4-PWK    │                       │
│                                             │           │               ┼──────────────┤      SIM7600G-H       │
│                                             │           └──┬──┬─────────┘              │                       │
│                                             │  DATA-21     │  │                        │                       │
│                                             ┼──────────────┘  │                        │                       │
│                                             │  CLK-22         │                        │                       │
│                                             ┼─────────────────┘                        │                       │
│                                             │                                          │                       │
│                                             │                                          └───────────────────────┘
│                                             │                                                                   
│                                             │                                                                   
└─────────────────────────────────────────────┘                                                                   
```
