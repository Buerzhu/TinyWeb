QT += core
QT -= gui

TARGET = server
CONFIG += console
CONFIG -= app_bundle
QMAKE_CXXFLAGS += -std=c++0x

TEMPLATE = app

SOURCES += main.cpp

HEADERS += \
    http_conn.h \
    web_function.h \
    web_thread.h

