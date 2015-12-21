CC=g++
SO=so

INC+= -I../../click/include/click
CFLAGS+= -g -Wall -fPIC $(INC)
OBJS=xcache.o ../common/xcache_sock.o ../bin/xcache_cmd.pb.o

all: libxcache.$(SO)

libxcache.$(SO): $(OBJS)
	$(CC) $(OBJS) -shared -o libxcache.$(SO)
	rm -f *.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o
	rm -f libxcache.$(SO)
