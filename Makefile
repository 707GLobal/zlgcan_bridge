CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lusbcan-4e -lpthread
OBJS     = main.o can_side.o zlg_side.o

all: zlg_can_bridge

zlg_can_bridge: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c zlg_bridge.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f zlg_can_bridge *.o
