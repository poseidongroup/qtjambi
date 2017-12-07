include(qtjambi_base.pri)
include(../../../build/jambi-major-version.pri)
QTJAMBI_LIB_NAME = qtjambi
#!isEmpty(QTJAMBI_CONFIG) {
#    contains(QTJAMBI_CONFIG, debug) {
#        QTJAMBI_LIB_NAME = $$member(QTJAMBI_LIB_NAME, 0)_debuglib
#    }
#} else {
    CONFIG(debug, debug|release) {
        QTJAMBI_LIB_NAME = $$member(QTJAMBI_LIB_NAME, 0)_debuglib$${QTJAMBI_MAJOR_VERSION}
    }
    else {
        QTJAMBI_LIB_NAME = $$member(QTJAMBI_LIB_NAME, 0)$${QTJAMBI_MAJOR_VERSION}
    }
#}

macx:{
    LIBS += $$PWD/../../../build/qmake-qtjambi/lib/lib$$member(QTJAMBI_LIB_NAME, 0).jnilib
} else {
    LIBS += -L$$PWD/../../../build/qmake-qtjambi/lib -l$$QTJAMBI_LIB_NAME
}

QTJAMBI_CPP = ../../../build/generator/out/cpp/

# These option changes are recommended since at least: win32-msvc2005
win32-msvc* {
    CONFIG += embed_manifest_dll force_embed_manifest
}
