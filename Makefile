# /**************************************
#  EventMusicPlayerClient daemon (empcd)
#  by Jeroen Massar <jeroen@massar.ch>
# ***************************************
# $Author: $
# $Id: $
# $Date: $
# **************************************/
#
# Source Makefile for empcd
#

CROSS_COMPILE ?= arm-none-linux-gnueabi-
PREFIX  = $(CROSS_COMPILE)
CC      = $(PREFIX)gcc
CXX     = $(PREFIX)g++
CPP     = $(PREFIX)cpp
LD      = $(PREFIX)gcc
AR      = $(PREFIX)ar
AS      = $(PREFIX)as
RANLIB  = $(PREFIX)ranlib
PWD     = $(shell pwd)

SYSROOT ?= /usr/local/arago/arm-2009q1/arm-none-linux-gnueabi
RPATH_ARG ?= $(SYSROOT)/usr/lib
INCFLAG = -I. -I./support/mpc-0.12.2/src/ -I$(SYSROOT)/usr/include

BINS    = empcd
SRCS    = empcd.c keyeventtable.c support/mpc-0.12.2/src/libmpdclient.c
INCS    = empcd.h
DEPS    = Makefile
OBJS    = empcd.o keyeventtable.o support/mpc-0.12.2/src/libmpdclient.o
WARNS   = -W -Wall -pedantic -Wno-format -Wno-unused -Wno-long-long
EXTRA   = -g -O2 -pipe
MYCFLAGS  = $(WARNS) $(EXTRA) -D_GNU_SOURCE $(INCFLAG)
MYLDFLAGS = -L$(SYSROOT)/lib -L$(SYSROOT)/usr/lib

CFLAGS += $(MYCFLAGS)
LDFLAGS += $(MYLDFLAGS)

dirsbin = /usr/sbin/
dirdoc  = /usr/share/doc/empcd/
diretc = /etc

# Export some things
export dirsbin
export dirdoc
export diretc

# Make Targets
all:	$(BINS)

empcd:	$(OBJS) ${INCS} ${DEPS}
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

clean:
	$(RM) -rf $(OBJS) $(BINS) build-stamp configure-stamp debian/*.debhelper debian/empcd.substvars debian/files debian/dirs debian/empcd

distclean: clean

# Install the program into ${DESTDIR}
# RPM's don't want the docs so it won't get it ;)
install: all
	@echo "Installing into ${DESTDIR}..."
	@echo "Binaries..."
	@mkdir -p ${DESTDIR}${dirsbin}
	@cp empcd ${DESTDIR}${dirsbin}
	@mkdir -p ${DESTDIR}${dirdoc}
	@cp README.md ${DESTDIR}${dirdoc}
	@echo "Configuration..."
	@mkdir -p ${DESTDIR}${diretc}
	@echo "Installation into ${DESTDIR}/ completed"

deb:
	@debian/rules binary

# Mark targets as phony
.PHONY : all clean deb

