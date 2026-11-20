// SPDX-License-Identifier: GPL-2.0-only
// Board control for lidar's EFR32MG12: no sensors, buttons or SPI flash; the
// only control line is PA5, which Amazon's firmware drives high at start-up
// (VCOM enable on Silicon Labs' BRD4161A/BRD4166A, whose USART0 PA0/PA1
// layout this board shares).
#ifndef SL_BOARD_CONTROL_CONFIG_H
#define SL_BOARD_CONTROL_CONFIG_H

// <<< Use Configuration Wizard in Context Menu >>>

// <q SL_BOARD_ENABLE_VCOM> Drive the VCOM enable line high
#define SL_BOARD_ENABLE_VCOM            1

// <<< end of configuration section >>>

// <<< sl:start pin_tool >>>
// <gpio> SL_BOARD_ENABLE_VCOM
#define SL_BOARD_ENABLE_VCOM_PORT       gpioPortA
#define SL_BOARD_ENABLE_VCOM_PIN        5
// <<< sl:end pin_tool >>>

#endif // SL_BOARD_CONTROL_CONFIG_H
