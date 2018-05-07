//
// client.h
// Author: Paul DeKeyser
//
// Created  04/18/2018
// Modified 04/21/2018
//
// Declarations and definitions for interfacing to simplified Bluez bluetooth
// client.

#ifndef CLIENT_H
#define CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

// Constants
#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_PATH "/org/bluez"
#define ROOT_PATH "/"

// UUIDs for the single device and two characteristics this client deals with.    
#define UUID_CHARACTERISTIC_RD "0003caa2-0000-1000-8000-00805f9b0131"
#define UUID_CHARACTERISTIC_WR "0003cbb1-0000-1000-8000-00805f9b0131"
#define UUID_DEVICE "0003cbbb-0000-1000-8000-00805f9b0131"


typedef void (* PropertyCallback) (const char *interface, const char *name, int yes);
typedef void (* NotificationCallback) (int);

// Function prototypes
void        bluez_acquire_notify            (NotificationCallback cb);
void        bluez_client_init               (DBusConnection *connection, const char *service,
                                             const char *path, GDBusClientFunction ready);
void        bluez_client_exit               (void);
gboolean    bluez_connect                   (void);
void        bluez_power_on                  (void);
int         bluez_read_property_boolean     (GDBusProxy *proxy, const char *name, gboolean *yes);
void        bluez_scan                      (gboolean on);
gboolean    bluez_set_property              (GDBusProxy *proxy, const char *name, int type, const void *value);
void        bluez_set_property_change_fn    (PropertyCallback fn);
void        bluez_write_attribute           (uint32_t value);



#ifdef __cplusplus
}
#endif

#endif // CLIENT_H

