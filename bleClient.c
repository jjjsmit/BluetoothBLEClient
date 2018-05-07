//
// bleClient.c
//
// Created  03/20/18
// Modified 04/22/18
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
// This sample client is the work of Paul DeKeyser of Stellar LLC,
// Irvine, California.
//
// The BlueZ code included in this module is from the following BlueZ
// example files:
//    gdbus/client.c
//    gdbus/object.c
//    client/gatt.c
//
// Following is the copyright header from these BlueZ source files:
/*
 *
 *  [for gdbus/client.c and gdbus/object.c]:
 *  D-Bus helper library
 *
 *  Copyright (C) 2004-2011  Marcel Holtmann <marcel@holtmann.org>
 *
 *  [for client/gatt.c]:
 *  Copyright (C) 2014  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <glib.h>
#include <dbus/dbus.h>

#include "gdbus/gdbus.h"
#include "bleClient.h"

#define METHOD_CALL_TIMEOUT (300 * 1000)

#ifndef DBUS_INTERFACE_OBJECT_MANAGER
#define DBUS_INTERFACE_OBJECT_MANAGER DBUS_INTERFACE_DBUS ".ObjectManager"
#endif

#define MAX_BLUEZ_PATH 64
#define MAX_BLUEZ_INTERFACE 32
#define MAX_PROPERTIES 4
#define MAX_PROXIES 3

#define error(fmt...)

extern DBusConnection *dbus_conn;

//- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Forward declarations.

gboolean            bluez_dbus_msg_recurse  (DBusMessageIter *iter, DBusMessageIter *sub,
                                             int type, void *value);
static void         bluez_add_property      (GDBusProxy *proxy, const char *name,
                                             DBusMessageIter *iter, gboolean send_changed);
static void         bluez_discovery_filter  (void);
static GDBusProxy * bluez_screen_interface  (GDBusClient *client, const char *path,
                                             const char *interface, DBusMessageIter *iter);
static gboolean     bluez_screen_uuid       (DBusMessageIter *iter, const char *uuidWanted);

struct GDBusClient
{
	DBusConnection *dbus_conn;
	char *service_name;
	char *base_path;
	char *root_path;
	guint watch;
	guint added_watch;
	guint removed_watch;
	DBusPendingCall *pending_call;
	DBusPendingCall *get_objects_call;
	gboolean connected;
	GDBusProxyFunction proxy_added;
	GDBusClientFunction ready;
        PropertyCallback propertyCallback;
};

struct prop_entry
{
	char *name;
	int type;
	DBusMessage *msg;
};

struct GDBusProxy 
{
	GDBusClient *client;
	char obj_path[MAX_BLUEZ_PATH];
	char interface[MAX_BLUEZ_INTERFACE];
        struct prop_entry property[MAX_PROPERTIES];  // leave room for properties in structure
	guint watch;
	GDBusPropertyFunction prop_func;
	void *prop_data;
	GDBusProxyFunction removed_func;
	void *removed_data;
	gboolean pending;
};

// Statically allocated proxies for all supported interfaces.
GDBusProxy adapter =
{
    .property[0].name = "Powered",
    .property[1].name = "Discovering",
    .property[2].name = ""
},
device =
{
    .property[0].name = "RSSI",
    .property[1].name = "Connected",
    .property[2].name = "ServicesResolved",
    .property[3].name = ""
},
characteristicRd =
{
    .property[0].name = "NotifyAcquired",
    .property[1].name = ""
},
characteristicWr =
{
    .property[0].name = ""
};

// Statically allocated client structure for a single client.
GDBusClient btClient =
{
    .service_name = BLUEZ_SERVICE,
    .base_path = BLUEZ_PATH,
    .root_path = ROOT_PATH,
    .connected = FALSE
};

//-----------------------------------------------------------------------------
// Functions extracted from Bluez module client/gatt.c.  Support for writing
// attributes removed.

#include "src/shared/io.h"
#include <unistd.h>

struct chrc {
	char *path;
	struct io *notify_io;
};

struct pipe_io {
	GDBusProxy *proxy;
	struct io *io;
	uint16_t mtu;
        NotificationCallback cb;
};

static struct pipe_io notify_io;

static void notify_io_destroy(void)
{
	io_destroy(notify_io.io);
	memset(&notify_io, 0, sizeof(notify_io));
}

static bool pipe_read(struct io *io, void *user_data)
{
	struct chrc *chrc = user_data;
	uint8_t buf[512];
	int fd = io_get_fd(io);
	ssize_t bytes_read;

	if (io != notify_io.io && !chrc)
		return true;

	bytes_read = read(fd, buf, sizeof(buf));
	if (bytes_read < 0)
		return false;

        if (notify_io.cb != NULL)
            (notify_io.cb)((int)buf[0]);

	return true;
}

static bool pipe_hup(struct io *io, void *user_data)
{
    struct chrc *chrc = user_data;

    if (chrc) 
    {
        fprintf(stderr, "Attribute %s Notify pipe closed\n", chrc->path);

        io_destroy(chrc->notify_io);
        chrc->notify_io = NULL;

        return false;
    }

    fprintf(stderr, "%s closed\n", io == notify_io.io ? "Notify" : "Write");

    notify_io_destroy();

    return false;
}

static struct io *pipe_io_new(int fd, void *user_data)
{
	struct io *io;

	io = io_new(fd);

	io_set_close_on_destroy(io, true);

	io_set_read_handler(io, pipe_read, user_data, NULL);

	io_set_disconnect_handler(io, pipe_hup, user_data, NULL);

	return io;
}

static void acquire_notify_reply(DBusMessage *message, void *user_data)
{
	DBusError error;
	int fd;

	dbus_error_init(&error);

	if (dbus_set_error_from_message(&error, message) == TRUE)
        {
            fprintf(stderr, "Failed to acquire notify: %s\n", error.name);
            dbus_error_free(&error);
            return;
	}

	if (notify_io.io)
        {
            io_destroy(notify_io.io);
            notify_io.io = NULL;
	}

	notify_io.mtu = 0;

	if ((dbus_message_get_args(message, NULL, DBUS_TYPE_UNIX_FD, &fd,
					DBUS_TYPE_UINT16, &notify_io.mtu,
					DBUS_TYPE_INVALID) == false)) {
		fprintf(stderr, "Invalid AcquireNotify response\n");
		return;
	}

	fprintf(stderr, "AcquireNotify success: fd %d MTU %u\n", fd, notify_io.mtu);

	notify_io.io = pipe_io_new(fd, NULL);
}


static void acquire_setup(DBusMessageIter *iter, void *user_data)
{
	DBusMessageIter dict;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
					&dict);

	dbus_message_iter_close_container(iter, &dict);
}

void bluez_acquire_notify(NotificationCallback cb)
{
    GDBusProxy *proxy = &characteristicRd;
    
    if (strcmp(proxy->interface, "org.bluez.GattCharacteristic1"))
    {
        fprintf(stderr, "Unable to acquire notify: %s not a"
                        " characteristic\n", proxy->interface);
        return;
    }

    if (g_dbus_proxy_method_call(proxy, "AcquireNotify", acquire_setup,
                            acquire_notify_reply, NULL, NULL) == FALSE)
    {
        fprintf(stderr, "Failed to AcquireNotify\n");
        return;
    }

    notify_io.proxy = proxy;
    notify_io.cb = cb;
}

static void write_reply(DBusMessage *message, void *user_data)
{
    DBusError error;

    dbus_error_init(&error);

    if (dbus_set_error_from_message(&error, message) == TRUE)
    {
        fprintf(stderr, "Failed to write: %s\n", error.name);
        dbus_error_free(&error);
    }
}

static void write_setup(DBusMessageIter *iter, void *user_data)
{
    struct iovec *iov = user_data;
    DBusMessageIter array, dict;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "y", &array);
    dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
                                            &iov->iov_base, iov->iov_len);
    dbus_message_iter_close_container(iter, &array);

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
                                    DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                                    DBUS_TYPE_STRING_AS_STRING
                                    DBUS_TYPE_VARIANT_AS_STRING
                                    DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
                                    &dict);
    dbus_message_iter_close_container(iter, &dict);
}

// Write a four-byte value to the single GATT attribute supported for writes.
void bluez_write_attribute(uint32_t value)
{
    struct iovec iov;
    uint8_t bytes[4];
    int i;

    for (i = 3; ; i--)
    {
        bytes[i] = (uint8_t)value;
        if (0 == i)  break;
        value >>= 8;
    }
    iov.iov_base = bytes;
    iov.iov_len = 4;

    if (g_dbus_proxy_method_call(&characteristicWr, "WriteValue", write_setup,
                                    write_reply, &iov, NULL) == FALSE)
    {
        fprintf(stderr, "Failed to write\n");
    }
}


// End functions extracted from Bluez module gatt.c.

//-----------------------------------------------------------------------------
// Functions extracted from BlueZ module gdbus/object.c.

// In this application, messages are never added to the list of pending
// messages, so the g_dbus_flush() call serves no purpose.
#define g_dbus_flush( x )

gboolean g_dbus_send_message(DBusConnection *connection, DBusMessage *message)
{
	dbus_bool_t result = FALSE;

	if (!message)
		return FALSE;

        // This function supports only method calls and does not support
        // signal messages.
	if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_CALL)
            dbus_message_set_no_reply(message, TRUE);
	else if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL)
            return FALSE;
        
	// Flush pending signal to guarantee message order.  Note that in
        // this application, messages are never added to the list of pending
        // messages, so the g_dbus_flush() call serves no purpose.
	g_dbus_flush(connection);

	result = dbus_connection_send(connection, message, NULL);

	dbus_message_unref(message);

	return result;
}

gboolean g_dbus_send_message_with_reply(DBusConnection *connection,
					DBusMessage *message,
					DBusPendingCall **call, int timeout)
{
	dbus_bool_t ret;

	// Flush pending signal to guarantee message order.
	g_dbus_flush(connection);

	ret = dbus_connection_send_with_reply(connection, message, call,
								timeout);

	if (ret == TRUE && call != NULL && *call == NULL)
        {
            error("Unable to send message (passing fd blocked?)");
            return FALSE;
	}

	return ret;
}

// End functions extracted from BlueZ module object.c.

//-----------------------------------------------------------------------------
// Functions extracted from Bluez module gdbus/client.c.

static void append_variant(DBusMessageIter *iter, int type, const void *val)
{
	DBusMessageIter value;
	char sig[2] = { type, '\0' };

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, sig, &value);

	dbus_message_iter_append_basic(&value, type, val);

	dbus_message_iter_close_container(iter, &value);
}

static void append_array_variant(DBusMessageIter *iter, int type, void *val,
							int n_elements)
{
	DBusMessageIter variant, array;
	char type_sig[2] = { type, '\0' };
	char array_sig[3] = { DBUS_TYPE_ARRAY, type, '\0' };

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						array_sig, &variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
						type_sig, &array);

	if (dbus_type_is_fixed(type) == TRUE) {
		dbus_message_iter_append_fixed_array(&array, type, val,
							n_elements);
	} else if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
		const char ***str_array = val;
		int i;

		for (i = 0; i < n_elements; i++)
			dbus_message_iter_append_basic(&array, type,
							&((*str_array)[i]));
	}

	dbus_message_iter_close_container(&variant, &array);

	dbus_message_iter_close_container(iter, &variant);
}

void g_dbus_dict_append_basic_array(DBusMessageIter *dict, int key_type,
					const void *key, int type, void *val,
					int n_elements)
{
	DBusMessageIter entry;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, key_type, key);

	append_array_variant(&entry, type, val, n_elements);

	dbus_message_iter_close_container(dict, &entry);
}

void g_dbus_dict_append_array(DBusMessageIter *dict,
					const char *key, int type, void *val,
					int n_elements)
{
	g_dbus_dict_append_basic_array(dict, DBUS_TYPE_STRING, &key, type, val,
								n_elements);
}

static void iter_append_iter(DBusMessageIter *base, DBusMessageIter *iter)
{
	int type;

	type = dbus_message_iter_get_arg_type(iter);

	if (dbus_type_is_basic(type)) {
		const void *value;

		dbus_message_iter_get_basic(iter, &value);
		dbus_message_iter_append_basic(base, type, &value);
	} else if (dbus_type_is_container(type)) {
		DBusMessageIter iter_sub, base_sub;
		char *sig;

		dbus_message_iter_recurse(iter, &iter_sub);

		switch (type) {
		case DBUS_TYPE_ARRAY:
		case DBUS_TYPE_VARIANT:
			sig = dbus_message_iter_get_signature(&iter_sub);
			break;
		default:
			sig = NULL;
			break;
		}

		dbus_message_iter_open_container(base, type, sig, &base_sub);

		if (sig != NULL)
			dbus_free(sig);

		while (dbus_message_iter_get_arg_type(&iter_sub) !=
							DBUS_TYPE_INVALID) {
			iter_append_iter(&base_sub, &iter_sub);
			dbus_message_iter_next(&iter_sub);
		}

		dbus_message_iter_close_container(base, &base_sub);
	}
}

static void prop_entry_update(struct prop_entry *prop, DBusMessageIter *iter)
{
	DBusMessage *msg;
	DBusMessageIter base;

	msg = dbus_message_new(DBUS_MESSAGE_TYPE_METHOD_RETURN);
	if (msg == NULL)
		return;

	dbus_message_iter_init_append(msg, &base);
	iter_append_iter(&base, iter);

	if (prop->msg != NULL)
		dbus_message_unref(prop->msg);

	prop->msg = dbus_message_copy(msg);
	dbus_message_unref(msg);
}

static void update_properties(GDBusProxy *proxy, DBusMessageIter *iter,
							gboolean send_changed)
{
    DBusMessageIter dict;

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
            return;

    dbus_message_iter_recurse(iter, &dict);

    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY)
    {
        DBusMessageIter entry;
        const char *name;

        if (bluez_dbus_msg_recurse(&dict, &entry, DBUS_TYPE_STRING, &name) == FALSE)
            break;

        bluez_add_property(proxy, name, &entry, send_changed);

        dbus_message_iter_next(&dict);
    }
}

static void proxy_added(GDBusClient *client, GDBusProxy *proxy)
{
    if (!proxy->pending)
            return;

    if (client->proxy_added)
            client->proxy_added(proxy, NULL);

    proxy->pending = FALSE;
}

static gboolean properties_changed(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
    GDBusProxy *proxy = user_data;
    GDBusClient *client = proxy->client;
    DBusMessageIter iter, entry;
    const char *interface;

    if (dbus_message_iter_init(msg, &iter) == FALSE)
            return TRUE;

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
            return TRUE;

    dbus_message_iter_get_basic(&iter, &interface);
    dbus_message_iter_next(&iter);

    update_properties(proxy, &iter, TRUE);

    return TRUE;
}

struct method_call_data
{
    GDBusReturnFunction function;
    void *user_data;
    GDBusDestroyFunction destroy;
};

static void method_call_reply(DBusPendingCall *call, void *user_data)
{
    struct method_call_data *data = user_data;
    DBusMessage *reply = dbus_pending_call_steal_reply(call);

    if (data->function)
            data->function(reply, data->user_data);

    if (data->destroy)
            data->destroy(data->user_data);

    dbus_message_unref(reply);
}

// Methods called:
// "org.bluez", "/org/bluez/hci0", "org.bluez.Adapter1", "SetDiscoveryFilter"
// "org.bluez", "/org/bluez/hci0", "org.bluez.Adapter1", "StartDiscovery"
// "org.bluez", "/org/bluez/hci0", "org.bluez.Adapter1", "StopDiscovery"
// "org.bluez", "/org/bluez/hci0/dev_00_A0_50_3E_47_9D", "org.bluez.Device1", "Connect"
// "org.bluez", "/org/bluez/hci0/dev_00_A0_50_3E_47_9D/service000c/char000d", "org.bluez.GattCharacteristic1", "AcquireNotify"
gboolean g_dbus_proxy_method_call(GDBusProxy *proxy, const char *method,
				GDBusSetupFunction setup,
				GDBusReturnFunction function, void *user_data,
				GDBusDestroyFunction destroy)  // destroy = 0
{
    struct method_call_data *data;
    GDBusClient *client;
    DBusMessage *msg;
    DBusPendingCall *call;

    if (proxy == NULL || method == NULL)
            return FALSE;

    client = proxy->client;
    if (client == NULL)
            return FALSE;

    msg = dbus_message_new_method_call(BLUEZ_SERVICE,
                            proxy->obj_path, proxy->interface, method);
    if (msg == NULL)
            return FALSE;

    if (setup) {
            DBusMessageIter iter;

            dbus_message_iter_init_append(msg, &iter);
            setup(&iter, user_data);
    }

    if (!function)
            return g_dbus_send_message(client->dbus_conn, msg);

    data = g_try_new0(struct method_call_data, 1);
    if (data == NULL)
            return FALSE;

    data->function = function;
    data->user_data = user_data;
    data->destroy = destroy;

    if (g_dbus_send_message_with_reply(client->dbus_conn, msg,
                                    &call, METHOD_CALL_TIMEOUT) == FALSE) {
            dbus_message_unref(msg);
            g_free(data);
            return FALSE;
    }

    dbus_pending_call_set_notify(call, method_call_reply, data, g_free);
    dbus_pending_call_unref(call);

    dbus_message_unref(msg);

    return TRUE;
}

static void parse_properties(GDBusClient *client, const char *path,
				const char *interface, DBusMessageIter *iter)
{
    GDBusProxy *proxy;

    if (g_str_equal(interface, DBUS_INTERFACE_INTROSPECTABLE) == TRUE)
            return;

    if (g_str_equal(interface, DBUS_INTERFACE_PROPERTIES) == TRUE)
            return;

    proxy = bluez_screen_interface(client, path, interface, iter);
    if (proxy == NULL)
        return;

    update_properties(proxy, iter, FALSE);

    proxy_added(client, proxy);
}

static void parse_interfaces(GDBusClient *client, const char *path,
						DBusMessageIter *iter)
{
    DBusMessageIter dict;

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
            return;

    dbus_message_iter_recurse(iter, &dict);

    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY)
    {
        DBusMessageIter entry;
        const char *interface;

        if (bluez_dbus_msg_recurse(&dict, &entry, DBUS_TYPE_STRING, &interface) == FALSE)
            break;

        // Will filter and add only properties of interest.
        parse_properties(client, path, interface, &entry);

        dbus_message_iter_next(&dict);
    }
}

// Arrives here from message_filter/signal_filter for "InterfacesAdded" signal
// from Bluez daemon when device is first discovered and it is not already in
// the Bluez daemon's database, then for every one of the device's services
// and characteristics.
static gboolean interfaces_added(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
    GDBusClient *client = user_data;
    DBusMessageIter iter;
    const char *path;

    if (dbus_message_iter_init(msg, &iter) == FALSE)
            return TRUE;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
            return TRUE;

    dbus_message_iter_get_basic(&iter, &path);
    dbus_message_iter_next(&iter);

    parse_interfaces(client, path, &iter);

    return TRUE;
}

static gboolean interfaces_removed(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
    // Since I removed the call to proxy_remove(), this function does no
    // meaningful work.  I am keeping the shell around because it seems
    // like it may be useful in the future.
#ifdef OLD
    GDBusClient *client = user_data;
    DBusMessageIter iter, entry;
    const char *path;

    if (dbus_message_iter_init(msg, &iter) == FALSE)
            return TRUE;

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
            return TRUE;

    dbus_message_iter_get_basic(&iter, &path);
    dbus_message_iter_next(&iter);

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
            return TRUE;

    dbus_message_iter_recurse(&iter, &entry);

    // Instead of proxy_remove() should set flag (possibly 'pending') so if interface
    // reappears it will be reinitialized... (this would be 'Device1')
    while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            const char *interface;

            dbus_message_iter_get_basic(&entry, &interface);
            proxy_remove(client, path, interface);
            dbus_message_iter_next(&entry);
    }

#endif
    return TRUE;
}

static void parse_managed_objects(GDBusClient *client, DBusMessage *msg)
{
    DBusMessageIter iter, dict;

    if (dbus_message_iter_init(msg, &iter) == FALSE)
            return;

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
            return;

    dbus_message_iter_recurse(&iter, &dict);

    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY)
    {
        DBusMessageIter entry;
        const char *path;

        if (bluez_dbus_msg_recurse(&dict, &entry, DBUS_TYPE_OBJECT_PATH, &path) == FALSE)
            break;

        parse_interfaces(client, path, &entry);

        dbus_message_iter_next(&dict);
    }
}

static void get_managed_objects_reply(DBusPendingCall *call, void *user_data)
{
    GDBusClient *client = user_data;
    DBusMessage *reply = dbus_pending_call_steal_reply(call);
    DBusError error;

    dbus_error_init(&error);

    if (dbus_set_error_from_message(&error, reply) == TRUE)
        dbus_error_free(&error);

    // When this calls through to parse_properties(), that function
    // will filter for only objects we are interested in.
    else
        parse_managed_objects(client, reply);

    if (client->ready)
            client->ready(client, NULL);

    dbus_message_unref(reply);

    dbus_pending_call_unref(client->get_objects_call);
    client->get_objects_call = NULL;
}

// Call the "GetManagedObjects" method on the org.bluez root path.  This
// is the only way to determine what objects exist.  The most important
// object is the bluetooth adapter at path /org/bluez/hci0.  The returned
// message includes all the adapter's sub-objects (such as Adapter1,
// GattManager1, etc.  The only object we need to capture is
// /org/bluez/hci0/Adapter1.  If Bluez has never discovered our device,
// the returned message does not include an interface to it and we need
// to monitor the "InterfacesAdded" signal until it shows up during the
// discovery process.  If Bluez has discovered our device in the past,
// the returned message from this call does include interfaces for our
// device and all of its services and characteristics, so we can capture
// them now to the proxies provided for them in this module even though
// we probably aren't connected to the device yet.
static void get_managed_objects(GDBusClient *client)
{
    DBusMessage *msg;

    if (!client->connected)
            return;

    if (client->get_objects_call != NULL)
            return;

    msg = dbus_message_new_method_call(BLUEZ_SERVICE,
                                       ROOT_PATH,
                                       DBUS_INTERFACE_OBJECT_MANAGER,
                                       "GetManagedObjects");
    if (msg == NULL)
            return;

    dbus_message_append_args(msg, DBUS_TYPE_INVALID);

    if (g_dbus_send_message_with_reply(client->dbus_conn, msg,
                            &client->get_objects_call, -1) == FALSE) {
            dbus_message_unref(msg);
            return;
    }

    dbus_pending_call_set_notify(client->get_objects_call,
                                            get_managed_objects_reply,
                                            client, NULL);

    dbus_message_unref(msg);
}

static void service_connect(DBusConnection *conn, void *user_data)
{
    btClient.connected = TRUE;

    get_managed_objects(&btClient);
}

static void service_disconnect(DBusConnection *conn, void *user_data)
{
    btClient.connected = FALSE;
}

// End functions extracted from Bluez module gdbus/client.c.

//-----------------------------------------------------------------------------
// Functions from here to the end of the file are either heavily modified
// versions of a similar function from the Bluez example code (mostly
// gdbus/client.c) or are newly authored convenience functions to minimize
// hair-pulling trying to keep DBus message parsing straight.

static void bluez_add_property(GDBusProxy *proxy, const char *name,
				DBusMessageIter *iter, gboolean send_changed)
{
    GDBusClient *client = proxy->client;
    DBusMessageIter value;
    int i;

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_VARIANT)
            return;

//fprintf(stderr, "add_property() %s, %s\n", proxy->obj_path, name);
// Experiment...
//if (strcmp(name, "Powered")  &&     // adapter 6
//    strcmp(name, "Discovering") &&  // adapter
//    strcmp(name, "UUIDs")  &&       // adapter 1, device 3
//    strcmp(name, "Connected") &&    // device 2
//    strcmp(name, "RSSI") &&         // device 7
//    strcmp(name, "ServicesResolved") && // device 5
//    strcmp(name, "Adapter") &&      // device 4
//    strcmp(name, "UUID") &&         // characteristic 8
//    strcmp(name, "NotifyAcquired")) // characteristic 9
//    return;  // ha ha! Don't really add property...
// end experiment
    dbus_message_iter_recurse(iter, &value);

    for (i = 0; i < MAX_PROPERTIES; i++)
    {
        if (!strcmp(proxy->property[i].name, ""))
        {
//            fprintf(stderr, "Attempting to add unsupported property %s\n", name);
            return;
        }
        if (!strcmp(proxy->property[i].name, name))
        {
            proxy->property[i].type = dbus_message_iter_get_arg_type(&value);
            prop_entry_update(&(proxy->property[i]), &value);
            break;
        }
    }

    if (proxy->prop_func)
            proxy->prop_func(proxy, name, &value, proxy->prop_data);

    if (client == NULL || send_changed == FALSE)
            return;

    if (client->propertyCallback)
    {
        int i = FALSE;
        if (DBUS_TYPE_BOOLEAN == dbus_message_iter_get_arg_type(&value))
        {
            dbus_bool_t b;
            dbus_message_iter_get_basic(&value, &b);
            if (b)  i = TRUE;
        }
        else i = -1;
        client->propertyCallback(proxy->interface, name, i);
    }
}

static void bluez_discovery_filter_setup(DBusMessageIter *iter, void *user_data)
{
    // Sets filter to only one UUID, defined as UUID_DEVICE
    static char *uuid_list[] = { UUID_DEVICE, NULL };
    DBusMessageIter dict;
    char **p_uuid_list = uuid_list;

    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
                            DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
                            DBUS_TYPE_STRING_AS_STRING
                            DBUS_TYPE_VARIANT_AS_STRING
                            DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

    g_dbus_dict_append_array(&dict, "UUIDs", DBUS_TYPE_STRING,
                                                    &p_uuid_list,
                                                    1);


    dbus_message_iter_close_container(iter, &dict);
}


static void bluez_discovery_filter_reply(DBusMessage *message, void *user_data)
{
    DBusError error;

    dbus_error_init(&error);
    if (dbus_set_error_from_message(&error, message) == TRUE)
    {
        fprintf(stderr, "SetDiscoveryFilter failed: %s\n", error.name);
        dbus_error_free(&error);
        return;
    }
}

static void bluez_discovery_filter(void)
{
    static gboolean filterSet = FALSE;
    
    if (filterSet)
	return;

    fprintf(stderr, "Setting discovery filter now...\n");
    
    if (g_dbus_proxy_method_call(&adapter, "SetDiscoveryFilter",
            bluez_discovery_filter_setup, bluez_discovery_filter_reply,
            NULL, NULL) == FALSE)
    {
        fprintf(stderr, "Failed to set discovery filter\n");
        return;
    }

    filterSet = TRUE;
}

static gboolean bluez_proxy_get_property(GDBusProxy *proxy, const char *name,
                                                        DBusMessageIter *iter)
{
    struct prop_entry *prop = NULL;
    int i;

    if (proxy == NULL || name == NULL)
            return FALSE;

    for (i = 0; i < MAX_PROPERTIES; i++)
    {
        if (!strcmp(proxy->property[i].name, ""))
            break;

        if (!strcmp(proxy->property[i].name, name))
        {
            prop = &(proxy->property[i]);
            break;
        }
    }

    if (prop == NULL)
    {
        fprintf(stderr, "Property %s->%s not found.\n", proxy->obj_path, name);
        return FALSE;
    }

    if (prop->msg == NULL)
            return FALSE;

    if (dbus_message_iter_init(prop->msg, iter) == FALSE)
            return FALSE;

    return TRUE;
}


// Screen for relevant objects, ignore everything else.
static GDBusProxy *bluez_screen_interface(GDBusClient *client, const char *path,
        const char *interface, DBusMessageIter *iter)
{
    GDBusProxy *proxy = NULL;
    
    if (!strcmp(interface, "org.bluez.Adapter1"))
        proxy = &adapter;

    else if (!strcmp(interface, "org.bluez.Device1"))
    {
        if (TRUE == bluez_screen_uuid(iter, UUID_DEVICE))
            proxy = &device;
    }

    else if (!strcmp(interface, "org.bluez.GattCharacteristic1"))
    {
        DBusMessageIter *copy = iter;
        if (TRUE == bluez_screen_uuid(iter, UUID_CHARACTERISTIC_RD))
            proxy = &characteristicRd;
        else if (TRUE == bluez_screen_uuid(copy, UUID_CHARACTERISTIC_WR))
            proxy = &characteristicWr;
    }

    if (NULL == proxy)
        return NULL;
    
    proxy->client = client;
    // "/org/bluez/hci0"
    // "/org/bluez/hci0/dev_xx_xx_xx_xx_xx_xx"
    // "/org/bluez/hci0/dev_xx_xx_xx_xx_xx_xx/serviceXXXX/charXXXX"
    strncpy(proxy->obj_path, path, MAX_BLUEZ_PATH);

    // "org.bluez.Adapter1"
    // "org.bluez.Device1"
    // "org.bluez.GattCharacteristic1"
    strncpy(proxy->interface, interface, MAX_BLUEZ_INTERFACE);

    proxy->watch = g_dbus_add_properties_watch(dbus_conn,
                                                    BLUEZ_SERVICE,
                                                    proxy->obj_path,
                                                    proxy->interface,
                                                    properties_changed,
                                                    proxy, NULL);
    proxy->pending = TRUE;
    
    return proxy;
}

// Dive down into a property (dictionary type) looking for the UUID.
// If the UUID matches one we want, return TRUE.
static gboolean bluez_screen_uuid(DBusMessageIter *iter, const char *uuidWanted)
{
    DBusMessageIter entry;

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
            return FALSE;

    dbus_message_iter_recurse(iter, &entry);

    // Dictionary is an array of entry/key/value structures.  Iterate
    // through the entries with 'entry'
    while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_DICT_ENTRY)
    {
        DBusMessageIter key, value;
        const char *name, *uuid;

        // Spelunk into this dictionary entry...
        if (bluez_dbus_msg_recurse(&entry, &key, DBUS_TYPE_STRING, &name) == FALSE)
            break;

//        fprintf(stderr, "Key: %s\n", name);
        if (!strcmp("UUID", name) || !strcmp("UUIDs", name))
        {
            // Value is variant type, recurse into it to extract the UUID string.
            if (dbus_message_iter_get_arg_type(&key) == DBUS_TYPE_VARIANT)
            {
                if (bluez_dbus_msg_recurse(&key, &value, DBUS_TYPE_STRING, &uuid) == TRUE)
                {
                    if (uuid != NULL)
                    {
//                        fprintf(stderr, "String: %s\n", uuid);
                        if (!strcmp(uuid, uuidWanted))
                            return TRUE;
                    }
                }
                else if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_ARRAY)
                {
                    DBusMessageIter child;
                    if (bluez_dbus_msg_recurse(&value, &child, DBUS_TYPE_STRING, &uuid) == FALSE)
                        break;

                    for ( ; ; )
                    {
//                        fprintf(stderr, "String: %s\n", uuid);
                        if (!strcmp(uuid, uuidWanted))
                            return TRUE;

                        dbus_message_iter_next(&child);
                        if (dbus_message_iter_get_arg_type(&child) != DBUS_TYPE_STRING)
                            break;
                        dbus_message_iter_get_basic(&child, &uuid);
                    }
                }
            }
            break;
        }
        dbus_message_iter_next(&entry);
    }
    return FALSE;
}


void bluez_client_init(DBusConnection *connection, const char *service,
        const char *path, GDBusClientFunction ready)
{
    if (!connection || !service)
            return;

    btClient.dbus_conn = dbus_connection_ref(connection);

    // This call increments the connection reference count by 3.
    btClient.watch = g_dbus_add_service_watch(connection, service,
                                            service_connect,
                                            service_disconnect,
                                            &btClient, NULL);

    // This call increments the connection reference count by 1.
    btClient.added_watch = g_dbus_add_signal_watch(connection, service,
                                            ROOT_PATH,
                                            DBUS_INTERFACE_OBJECT_MANAGER,
                                            "InterfacesAdded",
                                            interfaces_added,
                                            &btClient, NULL);
    // This call increments the connection reference count by 1.
    btClient.removed_watch = g_dbus_add_signal_watch(connection, service,
                                            ROOT_PATH,
                                            DBUS_INTERFACE_OBJECT_MANAGER,
                                            "InterfacesRemoved",
                                            interfaces_removed,
                                            &btClient, NULL);
    btClient.ready = ready;
}

void bluez_client_exit(void)
{
    // It is safe to call this if the notification io has already been destroyed.
    notify_io_destroy();

    if (btClient.pending_call != NULL)
    {
        dbus_pending_call_cancel(btClient.pending_call);
        dbus_pending_call_unref(btClient.pending_call);
    }

    if (btClient.get_objects_call != NULL)
    {
        dbus_pending_call_cancel(btClient.get_objects_call);
        dbus_pending_call_unref(btClient.get_objects_call);
    }

    g_dbus_remove_watch(btClient.dbus_conn, btClient.watch);
    g_dbus_remove_watch(btClient.dbus_conn, btClient.added_watch);
    g_dbus_remove_watch(btClient.dbus_conn, btClient.removed_watch);

    dbus_connection_unref(btClient.dbus_conn);
}

void bluez_set_property_change_fn(PropertyCallback fn)
{
    btClient.propertyCallback = fn;
}

// Returns non-zero if error encountered.  Otherwise sets 'yes' to the value of
// the property.
int bluez_read_property_boolean(GDBusProxy *proxy, const char *name, gboolean *yes)
{
    DBusMessageIter iter;
    dbus_bool_t value; 

    if (NULL == yes)
        return 1;

    *yes = FALSE;
    
    if (bluez_proxy_get_property(proxy, name, &iter) == FALSE)
        return 1;

    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_BOOLEAN)
        return 1;

    dbus_message_iter_get_basic(&iter, &value);
    
    if (value) *yes = TRUE;
    return 0;
}

gboolean bluez_dbus_msg_recurse(DBusMessageIter *iter, DBusMessageIter *sub, int type, void *value)
{
    dbus_message_iter_recurse(iter, sub);

    if (dbus_message_iter_get_arg_type(sub) != type)
            return FALSE;

    dbus_message_iter_get_basic(sub, value);
    dbus_message_iter_next(sub);
    
    return TRUE;
}

static void bluez_set_property_reply(DBusPendingCall *call, void *user_data)
{
    DBusMessage *reply = dbus_pending_call_steal_reply(call);
    DBusError error;

    dbus_error_init(&error);

    if (dbus_set_error_from_message(&error, reply) == TRUE)
        fprintf(stderr, "SetProperty failed: %s\n", error.name);

    dbus_error_free(&error);

    dbus_message_unref(reply);
}

gboolean bluez_set_property(GDBusProxy *proxy, const char *name, int type, const void *value)
{
    DBusMessage *msg;
    DBusMessageIter iter;
    DBusPendingCall *call;

    if (proxy == NULL || name == NULL || value == NULL)
            return FALSE;

    if (dbus_type_is_basic(type) == FALSE)
            return FALSE;

    msg = dbus_message_new_method_call(BLUEZ_SERVICE, proxy->obj_path,
        DBUS_INTERFACE_PROPERTIES, "Set");
    if (msg == NULL)
            return FALSE;

    // Field 'interface' is a char array, not a pointer.  For the call
    // to dbus_message_iter_append_basic() we need an additional level of
    // indirection.
    char *iface = proxy->interface;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &name);

    append_variant(&iter, type, value);

    if (g_dbus_send_message_with_reply(btClient.dbus_conn, msg,
                                                    &call, -1) == FALSE)
    {
            dbus_message_unref(msg);
            return FALSE;
    }

    dbus_pending_call_set_notify(call, bluez_set_property_reply, NULL, NULL);
    dbus_pending_call_unref(call);

    dbus_message_unref(msg);

    return TRUE;
}

// Connect to the one specific device we care about.
gboolean bluez_connect(void)
{
    return g_dbus_proxy_method_call(&device, "Connect", NULL, NULL, NULL, NULL);
}

// Power the Bluetooth adapter on.
void bluez_power_on(void)
{
    dbus_bool_t power = TRUE;

    if (bluez_set_property(&adapter, "Powered", DBUS_TYPE_BOOLEAN, &power) == FALSE)
        fprintf(stderr, "Failed to power adapter on.\n");
}

// Start / stop scan.  Searches for specific UUID set in bluez_discovery_filter().
void bluez_scan(gboolean on)
{
    const char *method;
    
    // Before starting scan, set filter to specific UUID
    if (on)
    {
        bluez_discovery_filter();
	method = "StartDiscovery";
    }
    else
        method = "StopDiscovery";

    if (g_dbus_proxy_method_call(&adapter, method,
			NULL, NULL, GUINT_TO_POINTER(on), NULL) == FALSE) 
    {
        fprintf(stderr, "Failed to %s discovery\n", on ? "start" : "stop");
    }
}
