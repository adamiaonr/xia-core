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

# debuggin' options
ifdef DEBUG
CFLAGS += -g -ggdb -Wall -DDEBUG
else
CFLAGS +=-O2
endif

# search for libs here
LDFLAGS += -Llib/libbloom/build
# add these libs for linking
LIB += -lbloom ../../daemons/xcache/api/libxcache.a -lpthread -lssl -lcrypto -lprotobuf $(LIBS)
# special include dirs to add
INC += -I include -I lib/libbloom

all: $(RID_PRO_TARGET) $(RID_REQ_TARGET) testrid
	mkdir -p $(BINDIR)
	mv $(RID_PRO_TARGET) $(BINDIR)
	mv $(RID_REQ_TARGET) $(BINDIR)

$(RID_PRO_TARGET): $(BUILDDIR)/$(RID_PRO_TARGET).o $(BUILDDIR)/$(COMMON).o
	@echo " Linking..."
	@echo " $(CC) $^ -o $(RID_PRO_TARGET) $(LIB)"; $(CC) $^ -o $(RID_PRO_TARGET) $(LDFLAGS) $(LIB)

$(RID_REQ_TARGET): $(BUILDDIR)/$(RID_REQ_TARGET).o $(BUILDDIR)/$(COMMON).o
	@echo " Linking..."
	@echo " $(CC) $^ -o $(RID_REQ_TARGET) $(LIB)"; $(CC) $^ -o $(RID_REQ_TARGET) $(LDFLAGS) $(LIB)

$(BUILDDIR)/%.o: $(SRCDIR)/%.$(SRCEXT)
	make -C lib/libbloom
	@mkdir -p $(BUILDDIR)
	@echo " $(CC) $(CFLAGS) $(INC) -c -o $@ $<"; $(CC) $(CFLAGS) $(INC) -c -o $@ $<

testrid:
	make -C test

clean:
	@echo " Cleaning...";
	make -C lib/libbloom clean
	make -C test clean
	@echo " $(RM) -r $(BUILDDIR) $(BINDIR)"; $(RM) -r $(BUILDDIR) $(BINDIR) *~

.PHONY: clean
