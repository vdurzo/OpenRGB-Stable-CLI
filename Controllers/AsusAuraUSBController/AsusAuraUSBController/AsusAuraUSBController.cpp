/*---------------------------------------------------------*\
| AsusAuraUSBController.cpp                                 |
|                                                           |
|   Driver for ASUS Aura USB device                         |
|                                                           |
|   Martin Hartl (inlart)                       25 Apr 2020 |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <cstring>
#include "AsusAuraUSBController.h"
#include "LogManager.h"
#include "StringUtils.h"

AuraUSBController::AuraUSBController(hid_device* dev_handle, const char* path, std::string dev_name)
{
    dev                 = dev_handle;
    location            = path;
    name                = dev_name;
    config_table_valid  = false;

    /*-----------------------------------------------------*\
    | Start from a known state.  If a request below fails,  |
    | the version string must be empty (not uninitialized   |
    | memory), otherwise it differs on every run and saved  |
    | zone sizes and profiles never match this device again |
    \*-----------------------------------------------------*/
    memset(config_table, 0x00, sizeof(config_table));
    memset(version,      0x00, sizeof(version));

    GetFirmwareVersion();
    GetConfigTable();
}

AuraUSBController::~AuraUSBController()
{
    hid_close(dev);
}

unsigned int AuraUSBController::GetChannelCount()
{
    return((unsigned int)device_info.size());
}

std::string AuraUSBController::GetDeviceLocation()
{
    return("HID: " + location);
}

std::string AuraUSBController::GetDeviceName()
{
    return(name);
}

std::string AuraUSBController::GetSerialString()
{
    wchar_t serial_string[128];
    int ret = hid_get_serial_number_string(dev, serial_string, 128);

    if(ret != 0)
    {
        return("");
    }

    return(StringUtils::wstring_to_string(serial_string));
}

std::string AuraUSBController::GetDeviceVersion()
{
    /*-----------------------------------------------------*\
    | The firmware string fills all 16 bytes and may not be |
    | null terminated, so never read past the buffer        |
    \*-----------------------------------------------------*/
    return(std::string(version, strnlen(version, sizeof(version) - 1)));
}

bool AuraUSBController::IsConfigTableValid() const
{
    return(config_table_valid);
}

bool AuraUSBController::SendRequestAndReadReply
    (
    unsigned char   request,
    unsigned char   expected_reply,
    unsigned char*  reply_buf
    )
{
    const int   max_attempts    = 3;
    const int   read_timeout_ms = 100;
    const int   max_reads       = 10;

    for(int attempt = 0; attempt < max_attempts; attempt++)
    {
        unsigned char usb_buf[65];

        /*-------------------------------------------------*\
        | Drain any stale input reports.  At boot, or when  |
        | another process is talking to the controller, old |
        | replies can be queued and would otherwise be read |
        | as the answer to this request                     |
        \*-------------------------------------------------*/
        for(int drained = 0; drained < 32; drained++)
        {
            if(hid_read_timeout(dev, usb_buf, sizeof(usb_buf), 0) <= 0)
            {
                break;
            }
        }

        /*-------------------------------------------------*\
        | Send the request                                  |
        \*-------------------------------------------------*/
        memset(usb_buf, 0x00, sizeof(usb_buf));

        usb_buf[0x00]   = 0xEC;
        usb_buf[0x01]   = request;

        if(hid_write(dev, usb_buf, sizeof(usb_buf)) < 0)
        {
            LOG_WARNING("[%s] Aura USB request 0x%02X: write failed (attempt %d)", name.c_str(), request, attempt + 1);
            continue;
        }

        /*-------------------------------------------------*\
        | Read with a timeout and only accept the reply we  |
        | asked for; a blocking read with no timeout could  |
        | hang detection forever                            |
        \*-------------------------------------------------*/
        for(int reads = 0; reads < max_reads; reads++)
        {
            memset(reply_buf, 0x00, 65);

            int bytes = hid_read_timeout(dev, reply_buf, 65, read_timeout_ms);

            if(bytes <= 0)
            {
                break;
            }

            if(reply_buf[1] == expected_reply)
            {
                return(true);
            }
        }

        LOG_WARNING("[%s] Aura USB request 0x%02X: no valid reply (attempt %d of %d)", name.c_str(), request, attempt + 1, max_attempts);
    }

    return(false);
}

const std::vector<AuraDeviceInfo>& AuraUSBController::GetAuraDevices() const
{
    return(device_info);
}

void AuraUSBController::GetConfigTable()
{
    unsigned char usb_buf[65];

    if(SendRequestAndReadReply(AURA_REQUEST_CONFIG_TABLE, 0x30, usb_buf))
    {
        memcpy(config_table, &usb_buf[4], 60);
        config_table_valid = true;

        LOG_DEBUG("[%s] ASUS Aura USB config table:", GetDeviceVersion().c_str());

        for(int i = 0; i < 60; i+=6)
        {
            LOG_DEBUG("[%s] %02X %02X %02X %02X %02X %02X", GetDeviceVersion().c_str(),
                                                            config_table[i + 0],
                                                            config_table[i + 1],
                                                            config_table[i + 2],
                                                            config_table[i + 3],
                                                            config_table[i + 4],
                                                            config_table[i + 5]);
        }
    }
    else
    {
        /*-------------------------------------------------*\
        | Previously this called "delete this" from inside  |
        | the constructor, after which the detector kept    |
        | using the freed object (use after free).  Now the |
        | table stays zeroed and the detector checks        |
        | IsConfigTableValid() and discards the device.     |
        \*-------------------------------------------------*/
        LOG_ERROR("[%s] Could not read config table, can not add device", name.c_str());
    }
}

void AuraUSBController::GetFirmwareVersion()
{
    unsigned char usb_buf[65];

    if(SendRequestAndReadReply(AURA_REQUEST_FIRMWARE_VERSION, 0x02, usb_buf))
    {
        memcpy(version, &usb_buf[2], 16);
        version[16] = '\0';
    }
    else
    {
        LOG_ERROR("[%s] Could not read firmware version", name.c_str());
    }
}

void AuraUSBController::SendDirect
    (
    unsigned char   device,
    unsigned char   led_count,
    RGBColor*  colors
    )
{
    unsigned char usb_buf[65];
    unsigned char offset         =      0x00;
    unsigned char sent_led_count =      LEDS_PER_PACKET;
    bool apply                   =      false;
    while(!apply)
    {
        if(offset + sent_led_count > led_count)
        {
            sent_led_count = led_count - offset;
        }


        if(offset + sent_led_count == led_count)
        {
            apply = true;
        }
        /*-----------------------------------------------------*\
        | Zero out buffer                                       |
        \*-----------------------------------------------------*/
        memset(usb_buf, 0x00, sizeof(usb_buf));

        /*-----------------------------------------------------*\
        | Set up message packet                                 |
        \*-----------------------------------------------------*/
        usb_buf[0x00]   = 0xEC;
        usb_buf[0x01]   = AURA_CONTROL_MODE_DIRECT;
        usb_buf[0x02]   = (apply ? 0x80 : 0x00) | device;
        usb_buf[0x03]   = offset;
        usb_buf[0x04]   = sent_led_count;

        /*-----------------------------------------------------*\
        | Copy in color data bytes                              |
        \*-----------------------------------------------------*/
        for(unsigned char led_idx = 0; led_idx < sent_led_count; led_idx++)
        {

            usb_buf[0x05 + (led_idx * 3)] = RGBGetRValue(colors[offset + led_idx]);
            usb_buf[0x06 + (led_idx * 3)] = RGBGetGValue(colors[offset + led_idx]);
            usb_buf[0x07 + (led_idx * 3)] = RGBGetBValue(colors[offset + led_idx]);
        }

        /*-----------------------------------------------------*\
        | Send packet                                           |
        \*-----------------------------------------------------*/
        hid_write(dev, usb_buf, 65);

        offset += sent_led_count;
    }
}
