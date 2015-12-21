include ../../../xia.mk

CC=g++
SO=so

INC+= -I../../click/include/click
LIB += -lXsocket -lpthread -lssl -lcrypto -ldagaddr -lprotobuf
CFLAGS+= -g -Wall -fPIC $(INC)
OBJS=xcache.o ../common/xcache_sock.o ../bin/xcache_cmd.pb.o

all: libxcache.$(SO)

libxcache.$(SO): $(OBJS)
	$(CC) $(CFLAGS) -shared -o libxcache.$(SO) $(OBJS) $(LIB)
	rm -f *.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o
	rm -f libxcache.$(SO)
