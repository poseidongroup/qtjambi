TARGET = com_trolltech_qt_test

include(../qtjambi/qtjambi_include.pri)
include($$QTJAMBI_CPP/com_trolltech_qt_test/com_trolltech_qt_test.pri)
include(../../../build/jambi-lib-version.pri)
# This looks like a bug in Qt to me
INCLUDEPATH += $$QMAKE_INCDIR_QT/QtTest
# We appear to reference qwidget.h
INCLUDEPATH += $$QMAKE_INCDIR_QT/QtGui

# libQtTest.so.4.7.4 is only dependant on libQtCore.so.4 (ensures removal of 'Qt -= gui')
# Hmmm no (not anymore?) in 4.7.4 qtestlib references qWaitForWindowShown() which
#  on linux needs qt_x11_wait_for_window_manager(QWidget*) from QtGui
#QT = core qtestlib
QT += qtestlib
CONFIG += qtestlib
