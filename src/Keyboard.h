/*****************************************************************************
* | File      	:   Keyboard.h
* | Author      :   Logan Puntous
* | Function    :   Initilize I2C, Create keyTask to poll keyboard and queue keypresses
* | Info        :
*----------------
* |	This version:   V0.0.1
* | Date        :   2025-12-18
* | Info        :
#
******************************************************************************/

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include "DEV_Config.h"
#include "Wire.h"

bool Keyboard_Init(TwoWire *i2cInstance, uint8_t i2cAddress);
//void Keyboard_Start(void);
bool Keyboard_GetChar(char *out, TickType_t wait);

//void Keyboard_Stop(void);




#endif // KEYBOARD_H