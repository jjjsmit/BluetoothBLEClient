# Makefile for example Bluetooth BLE client based on BlueZ release 5.49.
# Tested on a Raspberry Pi running Raspbian 4.9.59 (Stretch).
#
# Created 05/07/2018
# Copyright (C) 2018 Stellar LLC
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
#
# Dependencies:
#
# 1. BlueZ release 5.49
#    wget http://www.kernel.org/pub/linux/bluetooth/bluez-5.49.tar.xz
#    tar xvf bluez-5.49.tar.xz
#
#   Actually requires only the following files from the BlueZ source:
#      bluez-5.49/gdbus/mainloop.c
#      bluez-5.49/gdbus/watch.c
#      bluez-5.49/src/shared/io-glib.c
#
# 2. D-Bus library 1.12
#    sudo apt-get install -y libdbus-1-dev
#
# 3. GLib library 2.0 from GNOME.org
#    sudo apt-get install -y libglib2.0-dev
#
# That should do it.
 
#- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Environment 

BLUEZ_BASE  := bluez-5.49
OBJDIR	    := obj
SYS_INC	    := /usr/include
SYS_LIB	    := /usr/lib/arm-linux-gnueabihf

INC_DIRS    := $(BLUEZ_BASE) $(BLUEZ_BASE)/gdbus \
               $(SYS_INC)/dbus-1.0 $(SYS_LIB)/dbus-1.0/include \
               $(SYS_INC)/glib-2.0 $(SYS_LIB)/glib-2.0/include

LIBS	    := dbus-1 glib-2.0	

#- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Tools
COMPILE_FLAGS := \
	-c \
	-g \
	$(INC_DIRS:%=-I%)

CFLAGS	:= \
	$(COMPILE_FLAGS)

LINK_LIBS := \
	$(LIBS:%=-l%)

CC	:= gcc
LD	:= gcc
MKDIR	:= mkdir
RMDIR	:= rm -r

#- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Implicit Rules

$(OBJDIR)/%.o : %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ $<


#- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Targets

EXE := bleexample
	
_APP_OBJS   := ble.o bleClient.o mainloop.o watch.o io-glib.o
APP_OBJS    := $(addprefix $(OBJDIR)/, $(_APP_OBJS))
	
$(EXE):	$(APP_OBJS)
	$(LD) -o $@ $(APP_OBJS) $(LINK_LIBS)
	
$(OBJDIR):
	$(MKDIR) $(OBJDIR)

$(OBJDIR)/mainloop.o : $(BLUEZ_BASE)/gdbus/mainloop.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OBJDIR)/watch.o : $(BLUEZ_BASE)/gdbus/watch.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OBJDIR)/io-glib.o : $(BLUEZ_BASE)/src/shared/io-glib.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ $<


.PHONY: clean
clean:
	$(RMDIR) $(OBJDIR)

.PHONY: all
all: $(EXE)
