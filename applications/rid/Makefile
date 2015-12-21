include ../../xia.mk

CC = g++

# special directories
SRCDIR = src
BUILDDIR = build
BINDIR = bin

# different targets: RID requester and producer applications
RID_PRO_TARGET = rid_producer
RID_REQ_TARGET = rid_requester
# common source files
COMMON = rid

# other helpful variables
SRCEXT = c
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

CFLAGS += $(INC) $(LDFLAGS)

# add these libs for linking
LIB += -lbloom -lXsocket -lpthread -lssl -lcrypto -ldagaddr -lxcache -lprotobuf

all: $(RID_PRO_TARGET) $(RID_REQ_TARGET) testrid
	mkdir -p $(BINDIR)
	mv $(RID_PRO_TARGET) $(BINDIR)
	mv $(RID_REQ_TARGET) $(BINDIR)

$(RID_PRO_TARGET): $(BUILDDIR)/$(RID_PRO_TARGET).o $(BUILDDIR)/$(COMMON).o
	@echo " Linking..."
	$(CC) $(CFLAGS) -o $(RID_PRO_TARGET) $^ $(LIB)

$(RID_REQ_TARGET): $(BUILDDIR)/$(RID_REQ_TARGET).o $(BUILDDIR)/$(COMMON).o
	@echo " Linking..."
	$(CC) $(CFLAGS) -o $(RID_REQ_TARGET) $^ $(LIB)

$(BUILDDIR)/%.o: $(SRCDIR)/%.$(SRCEXT)
	make -C lib/libbloom
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ -c $<

testrid:
	make -C test

clean:
	@echo " Cleaning...";
	make -C lib/libbloom clean
	make -C test clean
	@echo " $(RM) -r $(BUILDDIR) $(BINDIR)"; $(RM) -r $(BUILDDIR) $(BINDIR) *~

.PHONY: clean
