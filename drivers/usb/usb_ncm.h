// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * USB CDC-NCM (Network Control Model) function for the NXP KSDK device stack
 */

#ifndef ODR_USB_NCM_H
#define ODR_USB_NCM_H

#include <stdbool.h>
#include <stdint.h>

// TODO(hal): built on the NXP KSDK and ST USB device stacks as wired in the legacy trees; to be redone on TinyUSB
#if defined(CPU_MK22FN512VLL12)
#include "usb.h"
#include "usb_device.h"
#include "usb_device_class.h"

// Class driver entry points, see s_UsbDeviceClassInterfaceMap in usb_device_class.c
usb_status_t USB_DeviceNcmInit(uint8_t controllerId, usb_device_class_config_struct_t *config, class_handle_t *handle);
usb_status_t USB_DeviceNcmDeinit(class_handle_t handle);
usb_status_t USB_DeviceNcmEvent(void *handle, uint32_t event, void *param);
#elif defined(STM32F405xx)
#include "usbd_def.h"

// ST USB device library class, registered instead of USBD_CDC in network mode (see usb_device.c)
extern USBD_ClassTypeDef USBD_NCM;
void usbNcmSetMacAddress(const uint8_t hostMac[6]);
#endif

// True once the host has selected the data interface alternate setting 1 (the link is up)
bool usbNcmIsUp(void);
uint8_t usbNcmGetDataAlternate(void);

// One Ethernet frame to the host. Returns false if the link is down or the previous frame is still being sent
bool usbNcmSendFrame(const uint8_t *frame, int length);

// Main task: hands the frames received from the host to ipGatewayEthernetIn()
void usbNcmTick(void);

#endif
