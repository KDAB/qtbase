TARGET = qphantom

PLUGIN_TYPE = platforms
PLUGIN_CLASS_NAME = PhantomIntegrationPlugin
load(qt_plugin)

QT += core-private gui-private platformsupport-private

SOURCES =   main.cpp \
            phantomintegration.cpp \
            phantombackingstore.cpp
HEADERS =   phantomintegration.h \
            phantombackingstore.h

OTHER_FILES += phantom.json

win32: include(../windows/windows.pri)

include(../../../platformsupport/fontdatabases/fontdatabases.pri)
