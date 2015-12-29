include ../../xia.mk

CC = g++

# special directories
SRCDIR = src
BUILDDIR = build
BINDIR = bin
TESTDIR = test

# source files
COMMON = rid

# other helpful variables
SRCEXT = c
LIBEXT = so
SOURCES = $(shell find $(SRCDIR) -type f -name *.$(SRCEXT))
OBJECTS = $(patsubst $(SRCDIR)/%,$(BUILDDIR)/%,$(SOURCES:.$(SRCEXT)=.o))

# special include dirs to add
INC += -Iinclude -Ilib/libbloom
# search for libs here
LDFLAGS += -Llib/libbloom/build

# debuggin' options
ifdef DEBUG
CFLAGS += -g -ggdb -Wall -DDEBUG
endif

CFLAGS += -fPIC $(INC) $(LDFLAGS)

# add these libs for linking
LIB += -lbloom -lXsocket -ldagaddr

all: $(BUILDDIR)/librid.$(LIBEXT)
	mkdir -p $(BINDIR)
	mv librid.$(LIBEXT) $(BINDIR)
	ln -s $(shell pwd)/lib/libbloom/build/libbloom.so $(BINDIR)

$(BUILDDIR)/librid.$(LIBEXT): $(BUILDDIR)/$(COMMON).o
	$(CC) $(CFLAGS) -shared -o librid.$(LIBEXT) $^ $(LIB)
	rm -f *.o

$(BUILDDIR)/%.o: $(SRCDIR)/%.$(SRCEXT)
	make -C lib/libbloom
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ -c $<

test:
	make -C $(TESTDIR)

clean:
	@echo " Cleaning...";
	make -C lib/libbloom clean
	make -C $(TESTDIR) clean
	$(RM) -r $(BUILDDIR) $(BINDIR) *~

.PHONY: clean
