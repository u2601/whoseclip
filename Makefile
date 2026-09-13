# Builds WhoseClip for Linux.  Usage:  make            (build)
#                                     make deps       (print the apt line)
#                                     make install    (into ~/.local)
#                                     make STRICT=1   (warnings are errors)
#
# The tray icon needs Ayatana AppIndicator.  It is optional: without it the
# binary still builds and the strip becomes the whole interface.  On Ubuntu
# Desktop the library and the GNOME extension that shows it are both there by
# default, so the tray normally just works.

BIN      := whoseclip
BUILDDIR := build/linux
SRCDIR   := src/linux
TESTDIR  := src/linux/test

SRCS := $(SRCDIR)/main_gtk.cpp $(SRCDIR)/clip_x11.cpp $(SRCDIR)/config_linux.cpp
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))

PKGS := gtk+-3.0 x11 xfixes xres

# Deprecation warnings are not suppressed to hide bugs; GTK 3 marks calls
# deprecated across point releases and a build that breaks on a distro upgrade
# is worse than a warning nobody can act on.
WARN := -Wall -Wextra -Wno-deprecated-declarations
ifdef STRICT
WARN += -Werror
endif

CXXFLAGS ?= -O2
# gnu++11, not c++11: strict ANSI mode sets __STRICT_ANSI__, which stops glibc
# declaring explicit_bzero, and wiping the preview buffer is not optional here.
CXXFLAGS += -std=gnu++11 $(WARN) -fno-rtti
CXXFLAGS += -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS  += -pie -Wl,-z,relro,-z,now

# NO_TRAY=1 builds without the tray icon, leaving the strip as the whole
# interface. Useful on a box without the library, and for telling a tray problem
# apart from a strip problem.
ifdef NO_TRAY
HAVE_AI :=
else
HAVE_AI := $(shell pkg-config --exists ayatana-appindicator3-0.1 && echo yes)
endif
ifeq ($(HAVE_AI),yes)
PKGS     += ayatana-appindicator3-0.1
CXXFLAGS += -DHAVE_APPINDICATOR
endif

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)

PREFIX ?= $(HOME)/.local

.PHONY: all clean deps install uninstall check holdsel test

all: $(BUILDDIR)/$(BIN)

# A selection owner that does not fork, so the tests can check that the owning
# process is reported correctly. xclip and xsel both daemonise, which leaves X
# holding a pid that has already exited. It lives one level below the product
# source because it carries its own main(), and a second main() sitting next to
# the real sources is a trap for anyone who later globs this directory.
holdsel: $(BUILDDIR)/holdsel

$(BUILDDIR)/holdsel: $(TESTDIR)/holdsel.c | $(BUILDDIR)
	$(CC) -O2 -Wall -Wextra -o $@ $< -lX11

test: all holdsel
	$(TESTDIR)/cliptest.sh

$(BUILDDIR)/$(BIN): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(PKG_LIBS)
	@echo
	@echo "  $@   $$(stat -c %s $@) bytes"
ifneq ($(HAVE_AI),yes)
	@echo "  note: built without a tray icon (ayatana-appindicator3 not found)"
	@echo "        run 'make deps' for the package list"
endif

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp $(SRCDIR)/whoseclip_linux.h | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -c $< -o $@

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

check:
	@pkg-config --exists gtk+-3.0 || echo "missing: libgtk-3-dev"
	@pkg-config --exists x11      || echo "missing: libx11-dev"
	@pkg-config --exists xfixes   || echo "missing: libxfixes-dev"
	@pkg-config --exists xres     || echo "missing: libxres-dev"
	@pkg-config --exists ayatana-appindicator3-0.1 || echo "missing (optional): libayatana-appindicator3-dev"
	@echo "check done"

deps:
	@echo "sudo apt install build-essential pkg-config libgtk-3-dev libx11-dev libxfixes-dev libxres-dev libayatana-appindicator3-dev"

install: $(BUILDDIR)/$(BIN)
	install -Dm755 $(BUILDDIR)/$(BIN) $(PREFIX)/bin/$(BIN)
	@echo "installed to $(PREFIX)/bin/$(BIN)"

uninstall:
	rm -f $(PREFIX)/bin/$(BIN)

clean:
	rm -rf $(BUILDDIR)
