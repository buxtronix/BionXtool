#
# Makefile for BionXtool (SocketCAN version)
# (a fork of BigXionFlasher)
#

CC = gcc
CFLAGS = -Wall -pthread -g0 -O2
LDFLAGS = -lncurses

OBJS = BionXtool.o

all: BionXtool

BionXtool: $(OBJS)
	$(CC) -o BionXtool $(OBJS) $(LDFLAGS)

BionXtool.o: BionXtool.c registers.h
	$(CC) $(CFLAGS) -c BionXtool.c

clean:	
	rm -f *.o *~ ./BionXtool ./BionXtool.exe
