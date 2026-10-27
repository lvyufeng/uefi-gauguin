/** @file
  Bring the phone up as a USB device, so a host can read what this firmware saw.

  Why this exists: gauguin has no serial console, and the panel cannot be trusted as a log -
  whether the framebuffer at 0xA0000000 is still being scanned out depends on what the boot
  loader left running, and a boot that never repainted it looks exactly like one that did. Every
  diagnosis this port has made has therefore been a photograph read by hand.

  This is the other channel. Qualcomm's firmware already carries an EFI_USB_DEVICE_PROTOCOL that
  brings the DWC3 controller up in peripheral mode, which is how the stock fastboot works; the
  loader only has to hand it a descriptor set. The descriptors here present an interface of
  class 0xFF, subclass 0x42, protocol 0x03 - fastboot - so an ordinary `fastboot` binary on the
  host recognises the device without any driver of ours.

  This module deliberately does nothing but enumerate. Its one job right now is to answer a
  question the port cannot answer from the panel: does the device show up on the host at all?
  The commands come after that is known.

  Copyright (c) 2026, the UniXNU contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
//
// Qualcomm's EFIUsbDevice.h cannot be included here.
//
// It pulls in its own IndustryStandard/Usb.h, whose USB_BOS_DESCRIPTOR and USB_INTERFACE_
// ASSOCIATION_DESCRIPTOR collide with the MdePkg copies the rest of this module already has.
// Only two things are needed from it - the GUID and the one method - so they are declared here
// instead, with the layout taken from the header itself (USB_DEVICE_DESCRIPTOR_SET and
// USB_DEVICE_START_EX).
//
#define EFI_USB_DEVICE_GUID \
  { 0x021bd2ca, 0x51d2, 0x11e3, { 0x8e, 0x56, 0xb7, 0x54, 0x17, 0xc7, 0x0b, 0x44 } }

typedef struct {
  VOID    *DeviceDescriptor;
  VOID    **Descriptors;
  VOID    *SSDeviceDescriptor;
  VOID    **SSDescriptors;
  VOID    *DeviceQualifierDescriptor;
  VOID    *BinaryDeviceOjectStore;
  UINT8   StringDescriptorCount;
  VOID    **StringDescritors;
} USB_PROBE_DESCRIPTOR_SET;

typedef EFI_STATUS (*USB_PROBE_START_EX) (IN USB_PROBE_DESCRIPTOR_SET *Set);

//
// Only StartEx is called; the methods ahead of it are placeholders that keep its offset right.
// The order is the one in EFIUsbDevice.h, which is what the firmware publishes.
//
typedef struct {
  UINTN               Revision;
  VOID                *Start;
  VOID                *Send;
  VOID                *HandleEvent;
  VOID                *AllocateTransferBuffer;
  VOID                *FreeTransferBuffer;
  VOID                *Stop;
  VOID                *AbortXfer;
  VOID                *SetEndpointStallState;
  USB_PROBE_START_EX  StartEx;
} USB_PROBE_PROTOCOL;


//
// Vendor and product as a host needs to recognise fastboot: Google's, which is what adb and
// fastboot both accept without a udev rule. 0xD00D is the product id the AOSP fastboot gadget
// uses.
//
#define USB_VID            0x18D1
#define USB_PID            0xD00D

#define USB_EP_IN          0x81
#define USB_EP_OUT          0x02
#define USB_MAX_PACKET       512

//
// Descriptors as bytes.
//
// Written out rather than declared with the USB structures: Qualcomm's EFIUsbDevice.h pulls in
// its own IndustryStandard/Usb.h, whose USB_BOS_DESCRIPTOR and USB_INTERFACE_ASSOCIATION_
// DESCRIPTOR collide with MdePkg's, and one of the two has to be given up. These are the bytes
// the host reads, and that is all they need to be.
//
//   device:    18 bytes, BcdUSB 0x0210, VID 0x18D1 PID 0xD00D
//   config:    one interface, class 0xFF subclass 0x42 protocol 0x03 (fastboot), two bulk
//              endpoints of 512 bytes
//
STATIC UINT8  mDevice[] = {
  18, 0x01, 0x10, 0x02, 0x00, 0x00, 0x00, 64,
  0xD1, 0x18, 0x0D, 0xD0, 0x00, 0x01,
  1, 2, 3, 1
};

STATIC UINT8  mConfig[] = {
  9, 0x02, 32, 0, 1, 1, 0, 0x80, 0x32,
  9, 0x04, 0, 0, 2, 0xFF, 0x42, 0x03, 4,
  7, 0x05, 0x81, 2, USB_MAX_PACKET & 0xFF, USB_MAX_PACKET >> 8, 0,
  7, 0x05, 0x02, 2, USB_MAX_PACKET & 0xFF, USB_MAX_PACKET >> 8, 0,
};

STATIC UINT8  mStringLang[] = { 4, 0x03, 0x09, 0x04 };

STATIC UINT8  mStringMfr[] = {
  16, 0x03, 'U', 0, 'n', 0, 'i', 0, 'X', 0, 'N', 0, 'U', 0, ' ', 0
};

STATIC UINT8  mStringProduct[] = {
  16, 0x03, 'g', 0, 'a', 0, 'u', 0, 'g', 0, 'u', 0, 'i', 0, 'n', 0
};

STATIC UINT8  mStringSerial[] = {
  12, 0x03, 'p', 0, 'r', 0, 'o', 0, 'b', 0, 'e', 0
};

/**
  Bring the controller up in peripheral mode.

  @retval EFI_SUCCESS  The device is enumerable; a host should see it.
**/
STATIC
EFI_STATUS
ProbeStart (
  VOID
  )
{
  EFI_STATUS                 Status;
  USB_PROBE_PROTOCOL         *Usb;
  USB_PROBE_DESCRIPTOR_SET   Set;
  VOID                       *Config[2];
  VOID                       *Strings[4];

  EFI_GUID  UsbDeviceGuid = EFI_USB_DEVICE_GUID;

  Status = gBS->LocateProtocol (&UsbDeviceGuid, NULL, (VOID **)&Usb);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "USBPROBE: no device protocol: %r\n", Status));
    return Status;
  }

  Config[0]  = mConfig;
  Config[1]  = NULL;
  Strings[0] = mStringLang;
  Strings[1] = mStringMfr;
  Strings[2] = mStringProduct;
  Strings[3] = mStringSerial;

  ZeroMem (&Set, sizeof (Set));
  Set.DeviceDescriptor    = mDevice;
  Set.Descriptors         = Config;
  Set.StringDescriptorCount = 4;
  Set.StringDescritors    = Strings;

  Status = Usb->StartEx (&Set);
  DEBUG ((DEBUG_ERROR, "USBPROBE: StartEx %r\n", Status));
  return Status;
}

EFI_STATUS
EFIAPI
UsbProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  //
  // Announced on the panel as well, because the panel is where this port looks first and the
  // host answer arrives somewhere else entirely.
  //
  DEBUG ((DEBUG_ERROR, "USBPROBE: entry\n"));
  ProbeStart ();
  return EFI_SUCCESS;
}