# ZOYI ZT703S PC GUI - Windows build: static Qt 6, plain make + g++.
#
# Needs MSYS2 with the UCRT64 packages listed in setup_msys2.sh. From a
# UCRT64 shell:
#     make                 builds build/win/zt703s.exe
#     make BUILD=<dir>     puts objects and the exe somewhere else
# qmake is not used: the static kit's qmake6 -query reports wrong paths,
# so the real ones are set below.

QT_PREFIX := /ucrt64/qt6-static
QT_LIBDIR := $(QT_PREFIX)/lib
QT_PLUGINS := $(QT_PREFIX)/share/qt6/plugins
MOC       := $(QT_PREFIX)/share/qt6/bin/moc.exe

QT_INCDIR := $(QT_PREFIX)/include/qt6
QT_INCS   := -I$(QT_INCDIR) -I$(QT_INCDIR)/QtCore -I$(QT_INCDIR)/QtGui \
             -I$(QT_INCDIR)/QtWidgets -I$(QT_INCDIR)/QtSerialPort
INCS      := $(QT_INCS) -I.
# Compiler only: Qt as -isystem, so -MMD leaves Qt headers out of the .d
# files. g++ writes them as C:/msys64/... and make reads "C:" as a target.
# moc keeps INCS.
CXX_INCS  := $(patsubst -I%,-isystem %,$(QT_INCS)) -I.

BUILD  ?= build/win
TARGET := $(BUILD)/zt703s.exe

CXX      := g++
# -g and no -s strip: Windows crash offsets can then be symbolized with addr2line.
# The kit's CMake targets (lib/cmake/Qt6Core/*.cmake) say release consumers
# compile with QT_NO_DEBUG + the module QT_*_LIB defines; missing them
# against static Qt headers caused a heap-corruption crash (any QSS on
# -> deterministic death). -DQT_STATIC: belt and braces for static kits.
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra \
            -DQT_NO_DEBUG -DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB \
            -DQT_SERIALPORT_LIB
LDFLAGS  := -static -mwindows

SRCS := $(wildcard *.cpp) $(wildcard core/*.cpp) $(wildcard ui/*.cpp)
OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS))

# Headers containing Q_OBJECT.
# core/x.h -> $(BUILD)/moc_core/x.cpp (mirrored under BUILD, keeps names unique).
MOC_HEADERS := core/scpiclient.h core/framepump.h ui/mainwindow.h ui/waveview.h ui/overview.h \
               ui/controldock.h ui/measbar.h ui/dmmpanel.h ui/genpanel.h
MOC_OBJS    := $(patsubst %.h,$(BUILD)/moc_%.o,$(MOC_HEADERS))

vpath %.h core ui .

# Static link line: plugin first (main.o's Q_IMPORT_PLUGIN references it),
# top-level Qt libs explicit (a .prl lists only dependencies), then the
# .prl closure, minus libraries this kit lists but does not need.
PRL_LIBS := $(shell sh prl-libs.sh $(QT_PREFIX) $(QT_LIBDIR) $(QT_PLUGINS) \
    $(QT_PLUGINS)/platforms/qwindows.prl \
    $(QT_LIBDIR)/Qt6Widgets.prl $(QT_LIBDIR)/Qt6SerialPort.prl \
    | tr ' ' '\n' \
    | grep -vE '^(-lssl|-lglib-2\.0|-lintl|-lpcre2-8|%/|.*libQt6OpenGL\.a)$$' \
    | tr '\n' ' ')
QT_LIBS := $(QT_PLUGINS)/platforms/libqwindows.a -L$(QT_LIBDIR) \
    -lQt6Widgets -lQt6SerialPort $(PRL_LIBS) -ldbghelp -lbcrypt
# -lbcrypt: MSYS2's static libcrypto calls BCryptGenRandom and its .prl
# does not name bcrypt.

all: $(TARGET)

$(TARGET): $(OBJS) $(MOC_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(MOC_OBJS) $(QT_LIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	MSYS2_ARG_CONV_EXCL=-MT $(CXX) $(CXXFLAGS) $(CXX_INCS) -MMD -MP -MT$@ -c $< -o $@

$(BUILD)/moc_%.cpp: %.h
	@mkdir -p $(dir $@)
	$(MOC) $(INCS) $< -o $@

$(BUILD)/moc_%.o: $(BUILD)/moc_%.cpp
	@mkdir -p $(dir $@)
	MSYS2_ARG_CONV_EXCL=-MT $(CXX) $(CXXFLAGS) $(CXX_INCS) -MMD -MP -MT$@ -c $< -o $@

clean:
	rm -rf $(BUILD)

.SECONDARY:   # keep moc intermediates across links (make would delete them)

# Header dependencies: -MMD writes a .d next to each object. -MT$@ (kept out
# of MSYS path conversion) names the target in make's /c/... spelling; g++
# itself would write C:/..., which never matches. Moc objects need a .d too:
# a class with no explicit destructor gets its vtable in moc_<class>.o, so a
# header it only includes can change its layout. Flag changes need make -B.
-include $(OBJS:.o=.d) $(MOC_OBJS:.o=.d)

.PHONY: all clean
