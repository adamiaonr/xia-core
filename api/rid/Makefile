include ../../xia.mk

CC = g++

# special directories
SRCDIR = src
BUILDDIR = build
BINDIR = bin

# source files
COMMON = rid

# other helpful variables
SRCEXT = c
LIBEXT = so
SOURCES = $(shell find $(SRCDIR) -type f -name *.$(SRCEXT))
OBJECTS = $(patsubst $(SRCDIR)/%,$(BUILDDIR)/%,$(SOURCES:.$(SRCEXT)=.o))

TESTDIR = test

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
	@echo " Moving librid.$(LIBEXT)..."
	mkdir -p $(BINDIR)
	mv librid.$(LIBEXT) $(BINDIR)
	@echo " Creating symlink to libbloom.so in $(BINDIR)..."
	ln -sf $(shell pwd)/lib/libbloom/build/libbloom.so $(BINDIR)
	# @echo " Building test folder..."
	# make -C test

$(BUILDDIR)/librid.$(LIBEXT): $(BUILDDIR)/$(COMMON).o
	@echo " Linking..."
	$(CC) $(CFLAGS) -Wl,--no-undefined -shared -o librid.$(LIBEXT) $^ $(LIB)

$(BUILDDIR)/%.o: $(SRCDIR)/%.$(SRCEXT)
	@echo " Compiling lib/libbloom..."
	make -C lib/libbloom
	@echo " Compiling librid sources..."
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	@echo " Cleaning...";
	make -C lib/libbloom clean
	make -C $(TESTDIR) clean
	$(RM) -r $(BUILDDIR) $(BINDIR) *~

.PHONY: clean
