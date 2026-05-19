/*****************************************************************************
* | File      	:   Command.h
* | Author      :   Logan Puntous
* | Function    :   Retreives commands from the keyboard buffer, processes them, and updates the display/modem/cmd_buffer/state/etc accordingly.
* | Info        :   Basically the largest if else statement ever. 
* |	This version:   V0.0.1
* | Date        :   2026-1-23
* | Info        :
#
******************************************************************************/

#ifndef COMMAND_H
#define COMMAND_H

#include "DEV_Config.h"

void Command_Handle(void);

#endif // COMMAND_H