TEMPLATE = lib
TARGET = ConfigSupport

QT += gui \
    network \
    xml \
    svg \
    opengl \
    declarative

DEFINES += QTCREATOR_UTILS_LIB

include(../../openpilotgcslibrary.pri)
include(config_support.pri)

SOURCES += calibration.cpp
HEADERS += calibration.h