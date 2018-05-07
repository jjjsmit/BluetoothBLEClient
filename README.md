## BluetoothBLEClient
Minimal Bluetooth BLE central client on Linux using BlueZ.

Interacts with known GATT characteristics in a known BLE peripheral.  Runs under Linux.  Based on BlueZ release 5.49.

## Setup

1. Obtain BlueZ release 5.49 and extract.  For the Makefile to work without modifications, do this in the same directory where you put this project's source files.

```
wget http://www.kernel.org/pub/linux/bluetooth/bluez-5.49.tar.xz
tar xvf bluez-5.49.tar.xz
```

2. Install the D-Bus library version 1.12 and GNOME GLib library 2.0.
```
sudo apt-get install -y libdbus-1-dev libglib2.0-dev
```
3. Build.
```
make all
```
## Background
I needed to add user input from a custom BLE peripheral, a simple remote pushbutton, to an embedded program running under Linux (Stretch) on a Raspberry Pi.  I found all the BlueZ "examples" to be needlessly complex, poorly documented, and devoid of comments.

In my case I knew exactly the device, service, and GATT characteristics I needed to interact with, and I wanted a minimum overhead task that would do its thing in a low priority thread.

I accomplished my needs using only three unmodified source files from the BlueZ distribution and excerpts from an additional three source files.  See the Makefile for exactly which three unmodified files are used, and see source file bleClient.c for the excerpts from other BlueZ source files.  Since the BlueZ source is inextricably dependent on D-Bus and the Gnome GLib main loop, these are included.
