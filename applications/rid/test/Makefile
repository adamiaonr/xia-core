#include ../../../xia.mk

CC = g++

# special directories
SRCDIR = src
BUILDDIR = build
BINDIR = bin

# target: rid_fwd
TARGET = rid_fwd
# common source files
COMMON = lookup_stats lsht pt rid_utils

# other helpful variables
SRCEXT = c
SOURCES = $(shell find $(SRCDIR) -type f -name *.$(SRCEXT))
OBJECTS = $(patsubst $(SRCDIR)/%,$(BUILDDIR)/%,$(SOURCES:.$(SRCEXT)=.o))

# debuggin' options
ifdef DEBUG
CFLAGS += -g
#else
#CFLAGS +=-O2
endif

# search for libs here
LDFLAGS += -L../lib/libbloom/build -Llib/uthash
# add these libs for linking
LIB += -lbloom $(LIBS)
# special include dirs to add
INC += -I include -I ../lib/libbloom -I lib/uthash/src

all: $(TARGET)
	mkdir -p $(BINDIR)
	mv $(TARGET) $(BINDIR)

$(TARGET): $(OBJECTS)
	@echo " Linking..."
	@echo " $(CC) -o $@ $^ $(LDFLAGS) $(LIB)"; $(CC) -o $@ $^ $(LDFLAGS) $(LIB)

$(BUILDDIR)/%.o: $(SRCDIR)/%.$(SRCEXT)
	make -C ../lib/libbloom
	@mkdir -p $(BUILDDIR)
	@echo "$(CC) $(CFLAGS) $(INC) -c $< -o $@"; $(CC) $(CFLAGS) $(INC) -c $< -o $@
	
clean:
	@echo " Cleaning...";
	make -C ../lib/libbloom clean
	@echo " $(RM) -r $(BUILDDIR) $(BINDIR)"; $(RM) -r $(BUILDDIR) $(BINDIR) *~

.PHONY: clean
