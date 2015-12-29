include ../../../xia.mk

CC=g++
SO=so

INC+= -I../../click/include/click
LIB+= -lXsocket -lpthread -ldagaddr -lprotobuf
CFLAGS+= -g -Wall -fPIC $(INC)
OBJS=xcache.o ../common/xcache_sock.o ../bin/xcache_cmd.pb.o

all: libxcache.$(SO)

libxcache.$(SO): $(OBJS)
	@echo " Linking..."
	$(CC) $(CFLAGS) -Wl,--no-undefined -shared -o libxcache.$(SO) $(OBJS) $(LIB)
	rm -f *.o

%.o: %.c
	@echo " Compiling..."
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo " Cleaning..."
	rm -f *.o
	rm -f libxcache.$(SO)
