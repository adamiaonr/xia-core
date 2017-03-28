all: 
	make -C dagaddr
	make -C xsocket
	make -C xcache
	make -C wrapper
	make -C xssl
	make -C rid

static:
	make -C dagaddr static
	make -C xsocket static
	make -C xssl static

clean:
	make -C dagaddr clean
	make -C xsocket clean
	make -C xcache clean
	make -C wrapper clean
	make -C xssl clean
	make -C rid clean

test:
	make -C dagaddr test
	make -C xsocket test
	make -C wrapper test
	make -C rid test

