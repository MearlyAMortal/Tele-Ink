# Tele-Ink
## A WIP FreeRTOS based 4G/WiFi IoT device that utilizes a E-Ink display for low power

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

# Hardware Modifications
* Replaced 471 SMD resistors on M5Stack keyboard with 472 (3.3v logic)
* Replaced SMD resistor on AmpRipper for 1A max batt charge rate
* Switch on ESP32 Driver board 1 = A  2 = ON

