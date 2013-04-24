/*
 *  ---------
 * |.**> <**.|  CardContact
 * |*       *|  Software & System Consulting
 * |*       *|  Minden, Germany
 * |.**> <**.|  Copyright (c) 2013. All rights reserved
 *  ---------
 *
 * See file LICENSE for details on licensing
 *
 * Abstract :       Simple abstraction layer for USB devices
 *
 * Author :         Frank Thater
 *
 * Last modified:   2013-04-22
 *
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <libusb-1.0/libusb.h>

#include "usb_device.h"

int Open(unsigned short pn, usb_device_t **device) {

    int rc, cnt, i;
    libusb_device **devs, *dev;
    const unsigned char *extra;

    rc = libusb_init(NULL );

    if (rc != 0) {
        return -1;
    }

    cnt = libusb_get_device_list(NULL, &devs);

    if (cnt < 0) {
        return -1;
    }

    /* Iterate through all devices to find a reader */
    i = 0;
    cnt = 0;

    while ((dev = devs[i++]) != NULL ) {
        struct libusb_device_descriptor desc;
        uint8_t bus_number = libusb_get_bus_number(dev);
        uint8_t device_address = libusb_get_device_address(dev);

        rc = libusb_get_device_descriptor(dev, &desc);

        if (rc < 0) {
            // error
            continue;
        }

        if (desc.idVendor == SCM_VENDOR_ID) {

#ifdef DEBUG
            if (desc.idProduct == SCM_SCR_35XX_DEVICE_ID) {
                printf("Found reader SCR_35XX (%04X:%04X)\n", desc.idVendor,
                        desc.idProduct);
            }

            if (desc.idProduct == SCM_SCR_3310_DEVICE_ID) {
                printf("Found reader SCR_3310 (%04X:%04X)\n", desc.idVendor,
                        desc.idProduct);
            }
#endif

            /*
             * Found the desired reader?
             */
            if (cnt == pn) {
                *device = malloc(sizeof(usb_device_t));
                memset(*device, 0, sizeof(usb_device_t));
                break;
            } else {
                cnt++;
            }
        }
    }

    if (dev != NULL ) { // reader found
        rc = libusb_open(dev, &((*device)->handle));

        if (rc != 0) {
            free(device);
            libusb_free_device_list(devs, 1);
            return -1;
        }

        rc = libusb_get_active_config_descriptor(dev, &((*device)->configuration_descriptor));
        if (rc != 0) {
            libusb_close((*device)->handle);
            free(device);
            libusb_free_device_list(devs, 1);
            return -1;
        }

        rc = libusb_claim_interface((*device)->handle, (*device)->configuration_descriptor->interface->altsetting->bInterfaceNumber);
        if (rc != 0) {
            libusb_close((*device)->handle);
            free(*device);
            libusb_free_device_list(devs, 1);
            return -1;
        }

        /*
         * Search for the bulk in/out endpoints
         */
        for (i = 0; i < (*device)->configuration_descriptor->interface->altsetting->bNumEndpoints; i++) {

            if ((*device)->configuration_descriptor->interface->altsetting->endpoint[i].bmAttributes
                    == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                /*
                 * Ignore the interrupt endpoint
                 */
                continue;
            }

            if (((*device)->configuration_descriptor->interface->altsetting->endpoint[i].bmAttributes
                    & LIBUSB_TRANSFER_TYPE_BULK) != LIBUSB_TRANSFER_TYPE_BULK) {
                /*
                 * No bulk endpoint - try the next one
                 */
                continue;
            }

            uint8_t bEndpointAddress = (*device)->configuration_descriptor->interface->altsetting->endpoint[i].bEndpointAddress;

            if ((bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                (*device)->bulk_in = bEndpointAddress;
            }

            if ((bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT)
                (*device)->bulk_out = bEndpointAddress;
        }

        if (!(*device)->configuration_descriptor->interface->altsetting->extra_length
            || (*device)->configuration_descriptor->interface->altsetting->extra_length != 54) {
                rc = -1;
        }

        extra = (*device)->configuration_descriptor->interface->altsetting->extra;
        (*device)->maxMessageLength = (extra[47] << 24) + (extra[46] << 16) + (extra[45] << 8) + extra[44];

        rc = 0;

    } else { // no reader found
        rc = -1;
    }

    libusb_free_device_list(devs, 1);

    return rc;
}



int Close(usb_device_t **device) {

    libusb_release_interface((*device)->handle,
            (*device)->configuration_descriptor->interface->altsetting->bInterfaceNumber);
    libusb_free_config_descriptor((*device)->configuration_descriptor);
    libusb_close((*device)->handle);
    free(*device);
    *device = NULL;

    libusb_exit(NULL );
    return 0;
}



int Write(usb_device_t *device, unsigned int length, unsigned char *buffer) {
    int rc;
    int send;

    rc = libusb_bulk_transfer(device->handle, device->bulk_out, buffer, length, &send, USB_WRITE_TIMEOUT);

    if (rc != 0 || (send != length)) {
#ifdef DEBUG
    	printf("libusb_bulk_transfer failed. rc = %i, send=%i, length=%i", rc, send, length);
#endif
        return -1;
    }

    return 0;
}



int Read(usb_device_t *device, unsigned int *length, unsigned char *buffer) {
    int rc;
    int read;

    rc = libusb_bulk_transfer(device->handle, device->bulk_in, buffer, *length,
            &read, USB_READ_TIMEOUT);

    if (rc != 0) {
        *length = 0;
        return -1;
    }

    *length = read;

    return 0;
}



int MaxMessageLength(usb_device_t *device) {
    return device->maxMessageLength;
}

