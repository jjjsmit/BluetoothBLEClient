## BluetoothBLEClient
Minimal Bluetooth BLE central client on Linux using BlueZ.

Interacts with known GATT characteristics in a known BLE peripheral.  Runs under Linux.  Based on BlueZ release 5.49.

## Dependencies

BlueZ release 5.49 must be installed on your Linux machine, earlier versions do not support the "AcquireNotify" method for GATT characteristics.  As of this date, this is not the BlueZ version that ships with the current Linux distribution, at least not Raspbian Stretch.  If you have fooled around much with BlueZ, this should come as no surprise.  To check the BlueZ version:
```
bluetoothctl -v
```
If the version is < 5.49, strap in, you will need to build and install BlueZ as explained in (1a) below.

## Setup

1. Obtain BlueZ release 5.49 and extract.  For the Makefile to work without modifications, do this in the same directory where you put this project's source files.

```
wget http://www.kernel.org/pub/linux/bluetooth/bluez-5.49.tar.xz
tar xvf bluez-5.49.tar.xz
```
1a. **(If the installed BlueZ version is < 5.49)**
Install BlueZ 5.49

```
cd bluez-5.49
sudo apt-get update
sudo apt-get install -y libusb-dev libdbus-1-dev libglib2.0-dev libudev-dev libical-dev libreadline-dev
./configure
make
sudo make install
```
Reboot.

1b. **(Installed BlueZ version >= < 5.49)** 
Install the D-Bus library version 1.12 and GNOME GLib library 2.0.

```
sudo apt-get install -y libdbus-1-dev libglib2.0-dev
```

2. Build.
```
make all
```
## Background
I needed to add user input from a custom BLE peripheral, a simple remote pushbutton, to an embedded program running under Linux (Stretch) on a Raspberry Pi.  I found all the BlueZ "examples" to be needlessly complex, poorly documented, and devoid of comments.

In my case I knew exactly the device, service, and GATT characteristics I needed to interact with, and I wanted a minimum overhead task that would do its thing in a low priority thread.

I accomplished my needs using only three unmodified source files from the BlueZ distribution and excerpts from an additional three source files.  See the Makefile for exactly which three unmodified files are used, and see source file bleClient.c for the excerpts from other BlueZ source files.  In addition the build requires a handful of .h files from the BlueZ source tree.  Since the BlueZ source is inextricably dependent on D-Bus and the Gnome GLib main loop, these are included.
