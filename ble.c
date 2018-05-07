//
// Bluetooth Low Energy (BLE) sample client.  Uses the BlueZ Bluetooth
// protocol stack for Linux.  This sample client is intended to be the
// simplest possible implementation for the purpose of connecting to a
// specific known BLE device and receiving notifications from a specific
// known GATT characteristic of that device.
//
// Developed in March 2018 using BlueZ release 5.49.  This application was
// derived from the "bluetoothctl" example client in the /client folder of
// the source at git.kernel.org/pub/scm/bluetooth/bluez.git.
//
// Copyright (C) 2018 Stellar LLC
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "src/shared/mainloop.h"
#include "gdbus.h"
#include "bleClient.h"

// Forward declarations.
static void bleState (int event);

// States for establishing communication with remote BLE device.
enum {
    STATE_INIT = 0,
    STATE_CONTROLLER_OFF,
    STATE_CONTROLLER_ON,
    STATE_SCAN,
    STATE_SCAN_STOPPED,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_ACQUIRE_NOTIFY,
    STATE_ROCK_N_ROLL
};

// Events passed into bleState()
enum {
    CLIENT_READY = 1,
    POWER_ON,
    DEVICE_ADDED,
    DEVICE_DETECTED,
    SCAN_STOPPED,
    DEVICE_READY,
    NOTIFY_ACQUIRED,
    DEVICE_DISCONNECTED
};

DBusConnection *dbus_conn;
static GMainLoop *mainLoop;

extern GDBusProxy   adapter,
                    device;
        

void quit(void)
{
    if (!mainLoop)
            return;

    g_main_loop_quit(mainLoop);
}

// Notification received, do something productive with it.  This is what all
// the other support code is meant to achieve.
static void notification(int value)
{
    static int called = 0;
    static uint32_t led[] = {0xFF000080, 0x00FF0080, 0x0000FF80}; // Red, green blue
    
    fprintf(stderr, "Notification: %d\n", value);
    
    if (0 == value)
        bluez_write_attribute(led[called/2]);

    if (++called < 6)
        return;
    fprintf(stderr, "Exiting program.\n");
    quit();
}

// This function is called only from the following interfaces:
//
//  Adapter1                 /org/bluez/hci0  (Discovering)
//  Device1                  /org/bluez/hci0/dev_00_A0_50_3E_47_9D (RSSI, Connected, ServicesResolved)
//  GattCharacteristic1      /org/bluez/hci0/dev_00_A0_50_3E_47_9D/service0011/service000c/char000d (NotifyAcquired)
//
static void propertyChanged(const char *interface, const char *name, int value)
{
    gboolean yes = (TRUE == value);
    
    fprintf(stderr, "propertyChanged(): on interface %s: %s%s\n", interface,
        name, (-1 == value) ? "" : ((TRUE == value) ? ": yes" : ": no"));

    if (!strcmp(interface, "org.bluez.Device1"))
    {
        // If Bluez daemon is telling us it has resolved our remote BLE
        // device's services,  we can now successfully enable notifications.
        if (!strcmp(name, "ServicesResolved")  &&  yes)
                bleState(DEVICE_READY);

        // Scan has detected the remote BLE device's advertisement
        if (!strcmp(name, "RSSI"))
            bleState(DEVICE_DETECTED);
        
        // Device has disconnected.
        if (!strcmp(name, "Connected") &&  !yes)
            bleState(DEVICE_DISCONNECTED);
    }
    else if (!strcmp(interface, "org.bluez.Adapter1"))
    {
        // Controller is just powered on, move to scanning state.
        if (!strcmp(name, "Powered")  && yes)
            bleState(POWER_ON);

        // Device found and scanning now stopped.  Move to connecting state.
        // Change state when "Discovering" changes to "no"
        else if (!strcmp(name, "Discovering") && !yes)
            bleState(SCAN_STOPPED);
    }
    else if (!strcmp(interface, "org.bluez.GattCharacteristic1"))
    {
        // Controller is just powered on, move to scanning state.
        if (!strcmp(name, "NotifyAcquired")  && yes)
            bleState(NOTIFY_ACQUIRED);
    }
}

static void client_ready(GDBusClient *client, void *user_data)
{
    // Controller proxy is initialized.  Start the process to establish
    // communication with the external BLE device.
    bleState(CLIENT_READY);
}

// State machine to step through the procedure to establish a connection
// with our desired BLE device and to receive notifications from it.
static void bleState(int event)
{
    static int currentState = STATE_INIT;
    DBusMessageIter iter;
    dbus_bool_t yes;
    
    while (1)
    {
        if (DEVICE_DISCONNECTED == event)
            currentState = STATE_CONTROLLER_ON;

        switch (currentState)
        {
            // No idea what state we're in.
            case STATE_INIT:
                // Can't do anything until our controller is set up.
                if (event != CLIENT_READY)
                    return;

                currentState = STATE_CONTROLLER_OFF;

                // Back through the loop to the next state...
                break;

            // Assume we are starting from scratch, so first query the
            // controller to see if it is powered up.
            case STATE_CONTROLLER_OFF:
                if (bluez_read_property_boolean(&adapter, "Powered", &yes) != 0)
                    return;

                // Controller is off, power it up now.
                if (!yes)
                {
                    bluez_power_on();
                    return;
                }

                // Controller is up, next step is to discover the
                // remote BLE device.
                currentState = STATE_CONTROLLER_ON;

                // Back through the loop to the next state...
                break;

            case STATE_CONTROLLER_ON:
                // Our BLE device may be in Bluez's database from before this time
                // running this program.  Check to see if it is connected.
                // If our BLE device is not in the database, this function call
                // returns non-zero and sets 'yes' to FALSE.
                bluez_read_property_boolean(&device, "Connected", &yes);

                // Our BLE device is either not connected, or it is not in the Bluez
                // daemon's database.  Either way, start scanning.
                if (!yes)
                {
                    currentState = STATE_SCAN;
                    bluez_scan(TRUE);
                    return;
                }

                currentState = STATE_CONNECTED;

                // Back through the loop to the next state...
                break;

            // Our BLE device detected, stop scan now.
            case STATE_SCAN:
                if (event != DEVICE_DETECTED)
                    return;

                bluez_scan(FALSE); // this should probably be queued for the main loop to do...  check out g_idle_add()
                currentState = STATE_SCAN_STOPPED;

                // Exit until propertyChanged() detects that discovery is
                // stopped.
                return;

            // Scanning now stopped, connect to our BLE device.
            case STATE_SCAN_STOPPED:
                if (event != SCAN_STOPPED)
                    return;

                fprintf(stderr, "Attempting to connect...\n");
                if (TRUE == bluez_connect())
                    currentState = STATE_CONNECTING;

                return;

            case STATE_CONNECTING:
                if (event != DEVICE_READY)
                    return;

                currentState = STATE_CONNECTED;

                // Back through the loop to the next state...
                break;

            case STATE_CONNECTED:
                bluez_acquire_notify(notification);
                
                currentState = STATE_ACQUIRE_NOTIFY;
                return;

            case STATE_ACQUIRE_NOTIFY:
                if (NOTIFY_ACQUIRED == event)
                    currentState = STATE_ROCK_N_ROLL;
                return;

            default:
                return;
        }
    }
}

int main(void)
{
    mainLoop = g_main_loop_new(NULL, FALSE);
        
    dbus_conn = g_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, NULL);

    bluez_client_init(dbus_conn, BLUEZ_SERVICE, BLUEZ_PATH, client_ready);

    bluez_set_property_change_fn(propertyChanged);

    // Program does not return from this call until the main loop exits
    // due to a call to g_main_loop_quit().
    g_main_loop_run(mainLoop);

    g_main_loop_unref(mainLoop);
    mainLoop = NULL;

    // Shut down notification input pipe, disconnect from DBus watches, and
    // cancel and free any DBus messaging in progress.
    bluez_client_exit();

    // After calls into the BlueZ "helper libraries" the reference count on
    // the DBus connection to the system bus is hopelessly unbalanced,
    // finishing with 5 or 6 left over.  This doesn't seem to do any long term
    // harm to the system, however, since after exiting the program and
    // restarting the reference count does not accumulate.
    dbus_connection_unref(dbus_conn);

    return 0;
}
