include ../../xia.mk

CC = g++

# special directories
SRCDIR = src
BUILDDIR = build
BINDIR = bin

# different targets: RID requester and producer applications
RID_PRO_TARGET = rid_producer
RID_REQ_TARGET = rid_requester

# other helpful variables
SRCEXT = c
SOURCES = $(shell find $(SRCDIR) -type f -name *.$(SRCEXT))
OBJECTS = $(patsubst $(SRCDIR)/%,$(BUILDDIR)/%,$(SOURCES:.$(SRCEXT)=.o))

# special include dirs to add
INC += -Iinclude
# search for libs here
LDFLAGS += $(LIBS)

# debuggin' options
ifdef DEBUG
CFLAGS += -g -ggdb -Wall -DDEBUG
endif

CFLAGS += $(INC)

# add these libs for linking
LIB += -lrid -lXsocket -ldagaddr -lxcache

# EXTRA: required due to the secondary linking (librid.so -> libbloom.so)
LIB += -Wl,-rpath=$(RIDAPIDIR)/bin

all: $(RID_PRO_TARGET) $(RID_REQ_TARGET)
	mkdir -p $(BINDIR)
	mv $(RID_PRO_TARGET) $(BINDIR)
	mv $(RID_REQ_TARGET) $(BINDIR)

$(RID_PRO_TARGET): $(BUILDDIR)/$(RID_PRO_TARGET).o
	@echo " Linking..."
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(RID_PRO_TARGET) $^ $(LIB)

$(RID_REQ_TARGET): $(BUILDDIR)/$(RID_REQ_TARGET).o
	@echo " Linking..."
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(RID_REQ_TARGET) $^ $(LIB)

$(BUILDDIR)/%.o: $(SRCDIR)/%.$(SRCEXT)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	@echo " Cleaning...";
	@echo " $(RM) -r $(BUILDDIR) $(BINDIR)"; $(RM) -r $(BUILDDIR) $(BINDIR) *~

.PHONY: clean
