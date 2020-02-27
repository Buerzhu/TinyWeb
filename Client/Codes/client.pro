QT += core
QT -= gui

TARGET = client
CONFIG += console
QMAKE_CXXFLAGS += -std=c++11
CONFIG -= app_bundle

TEMPLATE = app

SOURCES += main.cpp

