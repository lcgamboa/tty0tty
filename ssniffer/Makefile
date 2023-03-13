prefix = /usr/local
CP= cp
RM= rm -f
INSTALL= install -m755 -D
TARGET=ssniffer

CFLAGS += -Wall -O2

all: $(TARGET)

$(TARGET): $(TARGET).c 
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDCFLAGS) -o $@ $^

install: $(TARGET)
	$(INSTALL) $(TARGET) $(DESTDIR)$(prefix)/bin/$(TARGET)

clean:
	$(RM) $(TARGET)

distclean: clean

uninstall:
	$(RM) $(DESTDIR)$(prefix)/bin/$(TARGET)

.PHONY: all install clean distclean uninstall
