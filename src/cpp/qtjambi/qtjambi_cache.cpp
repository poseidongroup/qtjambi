/****************************************************************************
**
** Copyright (C) 1992-2009 Nokia. All rights reserved.
**
** This file is part of Qt Jambi.
**
** ** $BEGIN_LICENSE$
** Commercial Usage
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
** 
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
** 
** In addition, as a special exception, Nokia gives you certain
** additional rights. These rights are described in the Nokia Qt LGPL
** Exception version 1.0, included in the file LGPL_EXCEPTION.txt in this
** package.
** 
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
** 
** If you are unsure which license is appropriate for your use, please
** contact the sales department at qt-sales@nokia.com.
** $END_LICENSE$

**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#include "qtjambi_cache.h"
#include "qtjambifunctiontable.h"
#include "qtjambitypemanager_p.h"
#include "qtjambilink.h"
#include "qtjambi_core.h"

#include <QtCore/QHash>
#include <QtCore/QReadWriteLock>
#include <QtCore/QByteArray>

#include <QtCore/QDebug>

// #define QTJAMBI_NOCACHE
// #define QTJAMBI_COUNTCACHEMISSES(T) cacheMisses(T)

#ifdef QTJAMBI_COUNTCACHEMISSES
static void cacheMisses(const char *s)
{
    static int count = 0;

    qDebug("Total # of cache misses: %d : '%s'", ++count, s);
}
#else
# define QTJAMBI_COUNTCACHEMISSES(T)
#endif

Q_GLOBAL_STATIC(QReadWriteLock, gStaticLock);

uint qHash(const char *p)
{
    uint h = 0;
    uint g;

    while (*p != 0) {
        h = (h << 4) + *p++;
        if ((g = (h & 0xf0000000)) != 0)
            h ^= g >> 23;
        h &= ~g;
    }
    return h;
}
#define FUNC ^


typedef QHash<QString, QString> NameHash;
Q_GLOBAL_STATIC(QReadWriteLock, gJavaNameHashLock);
Q_GLOBAL_STATIC(NameHash, gJavaNameHash);
void registerQtToJava(const QString &qt_name, const QString &java_name)
{
    QWriteLocker locker(gJavaNameHashLock());
    gJavaNameHash()->insert(qt_name, java_name);
}
QString getJavaName(const QString &qt_name)
{
    QReadLocker locker(gJavaNameHashLock());
    return gJavaNameHash()->value(qt_name, QString());
}

Q_GLOBAL_STATIC(QReadWriteLock, gQtNameHashLock);
Q_GLOBAL_STATIC(NameHash, gQtNameHash);
void registerJavaToQt(const QString &java_name, const QString &qt_name)
{
    QWriteLocker locker(gQtNameHashLock());
    gQtNameHash()->insert(java_name, qt_name);
}

QString getQtName(const QString &java_name)
{
    QReadLocker locker(gQtNameHashLock());
    return gQtNameHash()->value(java_name, QString());
}

Q_GLOBAL_STATIC(QReadWriteLock, gJavaSignatureHashLock);
Q_GLOBAL_STATIC(NameHash, gJavaSignatureHash);
void registerJavaSignature(const QString &qt_name, const QString &java_signature)
{
    QWriteLocker locker(gJavaSignatureHashLock());
    gJavaSignatureHash()->insert(qt_name, java_signature);
}

QString getJavaSignature(const QString &qt_name)
{
    QReadLocker locker(gJavaSignatureHashLock());
    return gJavaSignatureHash()->value(qt_name, QString());
}

typedef QHash<QString, PtrDestructorFunction> DestructorHash;
Q_GLOBAL_STATIC(QReadWriteLock, gDestructorHashLock);
Q_GLOBAL_STATIC(DestructorHash, gDestructorHash);
void registerDestructor(const QString &java_name, PtrDestructorFunction destructor)
{
    QWriteLocker locker(gDestructorHashLock());
    gDestructorHash()->insert(java_name, destructor);
}

PtrDestructorFunction destructor(const QString &java_name)
{
    QReadLocker locker(gDestructorHashLock());
    return gDestructorHash()->value(java_name, 0);
}

typedef QHash<QString, ObjectDeletionPolicy> ObjectDeletionPolicyHash;
Q_GLOBAL_STATIC(QReadWriteLock, gObjectDeletionPolicyHashLock);
Q_GLOBAL_STATIC(ObjectDeletionPolicyHash, gObjectDeletionPolicyHash);
void registerObjectDeletionPolicy(const QString &java_name, ObjectDeletionPolicy policy)
{
    QWriteLocker locker(gObjectDeletionPolicyHashLock());
    gObjectDeletionPolicyHash()->insert(java_name, policy);
}

ObjectDeletionPolicy objectDeletionPolicy(const QString &java_name)
{
    QReadLocker locker(gObjectDeletionPolicyHashLock());
    return gObjectDeletionPolicyHash()->value(java_name, ObjectDeletionPolicyNormal);
}

/*******************************************************************************
 * Class Cache
 */
struct class_id
{
    const char *className;
    const char *package;
};

typedef QHash<class_id, jclass> ClassIdHash ;
Q_GLOBAL_STATIC(ClassIdHash, gClassHash);

inline bool operator==(const class_id &id1, const class_id &id2)
{
    return (!strcmp(id1.className, id2.className)
            && !strcmp(id1.package, id2.package));
}
uint qHash(const class_id &id) { return qHash(id.className) FUNC qHash(id.package); }

// The returned reference is owned by the caller, this means for returning GlobalRef
//  the caller needs to destroy it.
// Previous implementations of this method could expose the GlobalRef the cache itself
//  owned which caused this API to be tricky for consumers to handle (as you could never
//  be sure if the caller is responsible for destroying the reference or not, and
//  depending upon if the it was a cache-hit or cache-miss the type of reference
//  actually returned might be a LocalRef or a GlobalRef.
// Most use cases the caller just wants a LocalRef alias to the same instance from
//  the cache to temporarly use and then discard.
// I suspect there are very few users of this method that actually want a GlobalRef
//  but none the less API is provided by setting wantGlobalRef=true and always remember
//  to destory the any non-zero returned reference you obtained.
jclass resolveClass(JNIEnv *env, const char *className, const char *package, bool wantGlobalRef)
{
    Q_ASSERT(env);
    Q_ASSERT(className);
    Q_ASSERT(package);

    jclass returned;
#ifndef QTJAMBI_NOCACHE
    class_id key = { className, package };

    {
        QReadLocker locker(gStaticLock());
        returned = gClassHash()->value(key, 0);  // GlobalRef
        Q_ASSERT(REFTYPE_GLOBAL_SAFE(env, returned));
    }

    if (returned == 0) {
#endif // !QTJAMBI_NOCACHE

        QByteArray ba(package);
        ba += className;

        returned = qtjambi_find_class(env, ba.constData());  // LocalRef
        Q_ASSERT(REFTYPE_LOCAL_SAFE(env, returned));

#ifndef QTJAMBI_NOCACHE
        if (returned) {
            QWriteLocker locker(gStaticLock());

            // Read again this time with write lock held
            jclass confirm = gClassHash()->value(key, 0);
            if (confirm) {
                Q_ASSERT(REFTYPE_GLOBAL(env, confirm));
                env->DeleteLocalRef(returned);
                returned = confirm; // discard and use value from cache
            } else {
                QTJAMBI_COUNTCACHEMISSES(className);

                jobject global_ref = env->NewGlobalRef(returned);
                Q_ASSERT(REFTYPE_GLOBAL(env, global_ref));

                char *tmp = new char[strlen(className) + 1];
                strcpy(tmp, className);
                key.className = tmp;

                tmp = new char[strlen(package) + 1];
                strcpy(tmp, package);
                key.package = tmp;

                gClassHash()->insert(key, (jclass) global_ref);

                env->DeleteLocalRef(returned);
                returned = (jclass) global_ref;  // LocalRef => GlobalRef
            }
        }
    }
#endif // !QTJAMBI_NOCACHE

#ifndef QTJAMBI_NOCACHE
    if (returned) { // GlobalRef
        Q_ASSERT(REFTYPE_GLOBAL(env, returned));
        if (wantGlobalRef)
            returned = (jclass) env->NewGlobalRef(returned); // don't share make new
        else
            returned = (jclass) env->NewLocalRef(returned);  // GlobalRef => LocalRef
    }
#else
    if (returned) { // LocalRef
        Q_ASSERT(REFTYPE_LOCAL(env, returned));
        if (wantGlobalRef) {
            jclass global_ref = (jclass) env->NewGlobalRef(returned); // LocalRef => GlobalRef
            env->DeleteLocalRef(returned);
            returned = global_ref;
        }
    }
#endif // !QTJAMBI_NOCACHE

    if (wantGlobalRef)
        Q_ASSERT(REFTYPE_GLOBAL_SAFE(env, returned));
    if (!wantGlobalRef)
        Q_ASSERT(REFTYPE_LOCAL_SAFE(env, returned));

    return returned;
}

static int qtjambi_cache_prune_class(JNIEnv *env)
{
    QWriteLocker locker(gStaticLock());
    int count = 0;
    ClassIdHash::iterator i = gClassHash()->begin();
    while(i != gClassHash()->end()) {
        class_id key = i.key();

        jclass clazz = i.value();
        if(clazz) {
            Q_ASSERT(REFTYPE_GLOBAL(env, clazz));
            env->DeleteGlobalRef(clazz);
        }

        delete[] key.className;
        delete[] key.package;
        count++;

        i = gClassHash()->erase(i);
    }
    return count;
}


/*******************************************************************************
 * Field Cache
 */
struct field_id
{
    const char *fieldName;
    const char *className;
    const char *package;
    bool isStatic;
    JNIEnv *env;  // why is this here ?
};

typedef QHash<field_id, jfieldID> FieldIdHash ;
Q_GLOBAL_STATIC(FieldIdHash, gFieldHash);

inline bool operator==(const field_id &id1, const field_id &id2)
{
    return (!strcmp(id1.fieldName, id2.fieldName)
            && !strcmp(id1.className, id2.className)
            && !strcmp(id1.package, id2.package)
            && (id1.env == id2.env)
            && (id1.isStatic == id2.isStatic));
}
uint qHash(const field_id &id)
{
    return qHash(id.fieldName) FUNC qHash(id.className) FUNC qHash(id.package)
           FUNC qHash(id.env) FUNC int(id.isStatic);
}

jfieldID resolveField(JNIEnv *env, const char *fieldName, const char *signature,
                      const char *className, const char *package, bool isStatic)
{
    Q_ASSERT(env);
    Q_ASSERT(fieldName);
    Q_ASSERT(signature);
    Q_ASSERT(className);
    Q_ASSERT(package);

    jfieldID returned;
#ifndef QTJAMBI_NOCACHE
    field_id key = { fieldName, className, package, isStatic, env };

    {
        QReadLocker locker(gStaticLock());
        returned = gFieldHash()->value(key, 0);
    }

    if (returned == 0) {
#endif // !QTJAMBI_NOCACHE
        jclass clazz = resolveClass(env, className, package);
        Q_ASSERT(REFTYPE_LOCAL_SAFE(env, clazz));

#ifndef QTJAMBI_NOCACHE
        {
#endif // !QTJAMBI_NOCACHE

            if (clazz) {
                // Hmm the fieldID is surely only valid while we pin the 'clazz' ?
                // The JVM could theoretically unload classes
                if (!isStatic)
                    returned = env->GetFieldID(clazz, fieldName, signature);
                else
                    returned = env->GetStaticFieldID(clazz, fieldName, signature);
            }

#ifndef QTJAMBI_NOCACHE

            QWriteLocker locker(gStaticLock());

            // Read again this time with write lock held
            jfieldID confirm = gFieldHash()->value(key, 0);
            if (confirm) {
                returned = confirm; // discard and use value from cache
            } else {
                // Should we really record misses for potentially non-existant fields?
                QTJAMBI_COUNTCACHEMISSES(fieldName);

                if (returned) {
                    char *tmp = new char[strlen(fieldName) + 1];
                    strcpy(tmp, fieldName);
                    key.fieldName = tmp;

                    tmp = new char[strlen(className) + 1];
                    strcpy(tmp, className);
                    key.className = tmp;

                    tmp = new char[strlen(package) + 1];
                    strcpy(tmp, package);
                    key.package = tmp;

                    gFieldHash()->insert(key, returned);
                }
            }
        }
#endif // !QTJAMBI_NOCACHE

        if(clazz)
            env->DeleteLocalRef(clazz);

#ifndef QTJAMBI_NOCACHE
    }
#endif // !QTJAMBI_NOCACHE

    return returned;
}

static int qtjambi_cache_prune_field()
{
    QWriteLocker locker(gStaticLock());
    int count = 0;
    FieldIdHash::iterator i = gFieldHash()->begin();
    while(i != gFieldHash()->end()) {
        field_id key = i.key();

        // jfieldID in i.value() does not need any dealloc

        delete[] key.fieldName;
        delete[] key.className;
        delete[] key.package;
        count++;

        i = gFieldHash()->erase(i);
    }
    return count;
}

jfieldID resolveField(JNIEnv *env, const char *fieldName, const char *signature,
                      jclass clazz, bool isStatic)
{
    QString qualifiedName = QtJambiLink::nameForClass(env, clazz).replace(QLatin1Char('.'), QLatin1Char('/'));
    QByteArray className = QtJambiTypeManager::className(qualifiedName).toUtf8();
    QByteArray package = QtJambiTypeManager::package(qualifiedName).toUtf8();

    return resolveField(env, fieldName, signature, className.constData(), package.constData(), isStatic);
}

/*******************************************************************************
 * Method Cache
 */
struct method_id
{
    const char *methodName;
    const char *signature;
    const char *className;
    const char *package;
    bool isStatic;
    JNIEnv *env;
};

typedef QHash<method_id, jmethodID> MethodIdHash ;
Q_GLOBAL_STATIC(MethodIdHash, gMethodHash);

inline bool operator==(const method_id &id1, const method_id &id2)
{
    return (!strcmp(id1.methodName, id2.methodName)
            && !strcmp(id1.signature, id2.signature)
            && !strcmp(id1.className, id2.className)
            && !strcmp(id1.package, id2.package)
            && (id1.isStatic == id2.isStatic)
            && (id1.env == id2.env));
}
uint qHash(const method_id &id)
{
    return qHash(id.methodName)
           FUNC qHash(id.signature)
           FUNC qHash(id.className)
           FUNC qHash(id.package)
           FUNC qHash(id.env)
           FUNC int(id.isStatic);
}

jmethodID resolveMethod(JNIEnv *env, const char *methodName, const char *signature, const char *className,
                        const char *package, bool isStatic)
{
    Q_ASSERT(env);
    Q_ASSERT(methodName);
    Q_ASSERT(signature);
    Q_ASSERT(className);
    Q_ASSERT(package);

    jmethodID returned;
#ifndef QTJAMBI_NOCACHE
    method_id key = { methodName, signature, className, package, isStatic, env };

    {
        QReadLocker locker(gStaticLock());
        returned = gMethodHash()->value(key, 0);
    }

    if (returned == 0) {
#endif // !QTJAMBI_NOCACHE
        jclass clazz = resolveClass(env, className, package);
        Q_ASSERT(REFTYPE_LOCAL_SAFE(env, clazz));
#ifndef QTJAMBI_NOCACHE

#endif // !QTJAMBI_NOCACHE

        if (clazz) {
            // Hmm the methodID is surely only valid while we pin the 'clazz' ?
            // The JVM could theoretically unload classes
            if (!isStatic)
                returned = env->GetMethodID(clazz, methodName, signature);
            else
                returned = env->GetStaticMethodID(clazz, methodName, signature);
        }

#ifndef QTJAMBI_NOCACHE
        {
            QWriteLocker locker(gStaticLock());

            // Read again this time with write lock held
            jmethodID confirm = gMethodHash()->value(key, 0);
            if (confirm) {
                returned = confirm; // discard and use value from cache
            } else {
                // Should we really record misses for potentially non-existant fields?
                QTJAMBI_COUNTCACHEMISSES(methodName);

                if (returned) {
                    char *tmp = new char[strlen(methodName) + 1];
                    strcpy(tmp, methodName);
                    key.methodName = tmp;

                    tmp = new char[strlen(signature) + 1];
                    strcpy(tmp, signature);
                    key.signature = tmp;

                    tmp = new char[strlen(className) + 1];
                    strcpy(tmp, className);
                    key.className = tmp;

                    tmp = new char[strlen(package) + 1];
                    strcpy(tmp, package);
                    key.package = tmp;

                    gMethodHash()->insert(key, returned);
                }
            }
        }
#endif // !QTJAMBI_NOCACHE

        env->DeleteLocalRef(clazz);

#ifndef QTJAMBI_NOCACHE
    }
#endif // !QTJAMBI_NOCACHE

    return returned;
}

static int qtjambi_cache_prune_method()
{
    QWriteLocker locker(gStaticLock());
    int count = 0;
    MethodIdHash::iterator i = gMethodHash()->begin();
    while(i != gMethodHash()->end()) {
        method_id key = i.key();

        // jmethodID in i.value() does not need any dealloc

        delete[] key.methodName;
        delete[] key.signature;
        delete[] key.className;
        delete[] key.package;
        count++;

        i = gMethodHash()->erase(i);
    }
    return count;
}

jmethodID resolveMethod(JNIEnv *env, const char *methodName, const char *signature, jclass clazz,
                        bool isStatic)
{
    QString qualifiedName = QtJambiLink::nameForClass(env, clazz).replace(QLatin1Char('.'), QLatin1Char('/'));
    QByteArray className = QtJambiTypeManager::className(qualifiedName).toUtf8();
    QByteArray package = QtJambiTypeManager::package(qualifiedName).toUtf8();

    return resolveMethod(env, methodName, signature, className, package, isStatic);
}

/*******************************************************************************
 * Closest superclass in Qt Cache
 */

struct closestsuperclass_id
{
    const char *className;
    const char *package;
};

inline bool operator==(const closestsuperclass_id &id1, const closestsuperclass_id &id2)
{
    return (!strcmp(id1.className, id2.className)
            && !strcmp(id1.package, id2.package));
}
uint qHash(const closestsuperclass_id &id) { return qHash(id.className) FUNC qHash(id.package); }

jclass resolveClosestQtSuperclass(JNIEnv *env, jclass clazz, bool wantGlobalRef)
{
    Q_ASSERT(env);
    Q_ASSERT(clazz);
    Q_ASSERT(REFTYPE_LOCAL(env, clazz));

    QString qualifiedName = QtJambiLink::nameForClass(env, clazz).replace(QLatin1Char('.'), QLatin1Char('/'));
    QByteArray className = QtJambiTypeManager::className(qualifiedName).toUtf8();
    QByteArray package = QtJambiTypeManager::package(qualifiedName).toUtf8();

    return resolveClosestQtSuperclass(env, className.constData(), package.constData(), wantGlobalRef);  // recursive
}

typedef QHash<closestsuperclass_id, jclass> ClassHash;
Q_GLOBAL_STATIC(ClassHash, gQtSuperclassHash);
jclass resolveClosestQtSuperclass(JNIEnv *env, const char *className, const char *package, bool wantGlobalRef)
{
    Q_ASSERT(env);
    Q_ASSERT(className);
    Q_ASSERT(package);

    closestsuperclass_id key = { className, package };

    jclass returned;
    {
        QReadLocker locker(gStaticLock());
        returned = gQtSuperclassHash()->value(key, 0); // GlobalRef
        Q_ASSERT(REFTYPE_GLOBAL_SAFE(env, returned));
    }

    if (returned == 0) {
        jclass clazz = resolveClass(env, className, package, false);  // LocalRef
        Q_ASSERT(REFTYPE_LOCAL_SAFE(env, clazz));

        // Check if key is a Qt class
        if (clazz) {

            StaticCache *sc = StaticCache::instance();
            sc->resolveQtJambiInternal();

            if (env->CallStaticBooleanMethod(sc->QtJambiInternal.class_ref, sc->QtJambiInternal.isGeneratedClass, clazz)) {
                returned = clazz;  // already LocalRef
                clazz = 0;  // to stop DeleteLocalRef
            }

            // If not, try the superclass recursively
            if (returned == 0) {
                jclass superKey = env->GetSuperclass(clazz);
                Q_ASSERT(REFTYPE_LOCAL_SAFE(env, superKey));
                if (superKey) {
                    returned = resolveClosestQtSuperclass(env, superKey);  // LocalRef
                    Q_ASSERT(REFTYPE_LOCAL_SAFE(env, returned));
                    env->DeleteLocalRef(superKey);
                }
                env->DeleteLocalRef(clazz);
            }
        }

        if (returned) {  // LocalRef
            QWriteLocker locker(gStaticLock());

            // Read again this time with write lock held
            jclass confirm = gQtSuperclassHash()->value(key, 0);
            if (confirm) {
                Q_ASSERT(REFTYPE_GLOBAL(env, confirm));
                env->DeleteLocalRef(returned);
                returned = confirm; // discard and use value from cache
            } else {
                QTJAMBI_COUNTCACHEMISSES(className);

                jobject global_ref = env->NewGlobalRef(returned);
                Q_ASSERT(REFTYPE_GLOBAL(env, global_ref));

                char *tmp = new char[strlen(className) + 1];
                strcpy(tmp, className);
                key.className = tmp;

                tmp = new char[strlen(package) + 1];
                strcpy(tmp, package);
                key.package = tmp;

                gQtSuperclassHash()->insert(key, (jclass) global_ref);

                env->DeleteLocalRef(returned);
                returned = (jclass) global_ref;  // LocalRef => GlobalRef
            }
        }
    }

    if (returned) { // GlobalRef
        Q_ASSERT(REFTYPE_GLOBAL(env, returned));
        if (wantGlobalRef)
            returned = (jclass) env->NewGlobalRef(returned); // don't share make new
        else
            returned = (jclass) env->NewLocalRef(returned);  // GlobalRef => LocalRef
    }

    if (wantGlobalRef)
        Q_ASSERT(REFTYPE_GLOBAL_SAFE(env, returned));
    if (!wantGlobalRef)
        Q_ASSERT(REFTYPE_LOCAL_SAFE(env, returned));

    return returned;
}

static int qtjambi_cache_prune_superclass(JNIEnv *env)
{
    QWriteLocker locker(gStaticLock());
    int count = 0;
    ClassHash::iterator i = gQtSuperclassHash()->begin();
    while(i != gQtSuperclassHash()->end()) {
        closestsuperclass_id key = i.key();

        jclass clazz = i.value();
        if(clazz)
            env->DeleteGlobalRef(clazz);

        delete[] key.className;
        delete[] key.package;
        count++;

        i = gQtSuperclassHash()->erase(i);
    }
    return count;
}

/*******************************************************************************
 * Function Table Cache
 */
typedef QHash<QString, QtJambiFunctionTable *> FunctionTableHash;
Q_GLOBAL_STATIC(FunctionTableHash, functionTableCache);

QtJambiFunctionTable *findFunctionTable(const QString &className)
{
    QReadLocker locker(gStaticLock());
    Q_ASSERT(functionTableCache());
    QtJambiFunctionTable *table = functionTableCache()->value(className);
    return table;
}

void storeFunctionTable(const QString &className, QtJambiFunctionTable *table)
{
    QWriteLocker locker(gStaticLock());
    Q_ASSERT(functionTableCache());
    functionTableCache()->insert(className, table);
}

void removeFunctionTable(QtJambiFunctionTable *table)
{
    QWriteLocker locker(gStaticLock());
    if (functionTableCache())
        functionTableCache()->remove(table->className());
}

static int qtjambi_cache_prune_functiontable()
{
    QWriteLocker locker(gStaticLock());
    int count = 0;
    FunctionTableHash::iterator i = functionTableCache()->begin();
    while(i != functionTableCache()->end()) {
        QString key = i.key();

        QtJambiFunctionTable *table = i.value();
        table->dtorInhibitRemoveFunctionTable();
        delete table;

        count++;

        i = functionTableCache()->erase(i);
    }
    return count;
}

StaticCache::~StaticCache() {
    delete d;
}


Q_GLOBAL_STATIC(QReadWriteLock, staticcache_instance_lock);

StaticCache *StaticCache::the_cache;

StaticCache *StaticCache::instance()
{
    {
        QReadLocker read(staticcache_instance_lock());
        if (the_cache)
            return the_cache;
    }

    {
        QWriteLocker write(staticcache_instance_lock());
        if (the_cache)
            return the_cache;

        the_cache = new StaticCache;
        memset(the_cache, 0, sizeof(StaticCache));
        the_cache->d = new StaticCachePrivate();
        return the_cache;
    }
}

#define unref_class(env, x) do { if(cache->x != 0) { (env)->DeleteGlobalRef(cache->x); cache->x = 0; } } while(0)

void StaticCache::shutdown(JNIEnv *env)
{
    QWriteLocker write(staticcache_instance_lock());

    StaticCache *cache = the_cache;
    if(cache == 0)
        return;

    cache->d->lock.lock();
    // unreference everything
    unref_class(env, HashSet.class_ref);
    unref_class(env, ArrayList.class_ref);
    unref_class(env, Stack.class_ref);
    unref_class(env, LinkedList.class_ref);
    unref_class(env, Map.class_ref);
    unref_class(env, MapEntry.class_ref);
    unref_class(env, HashMap.class_ref);
    unref_class(env, TreeMap.class_ref);
    unref_class(env, NullPointerException.class_ref);
    unref_class(env, RuntimeException.class_ref);
    unref_class(env, Collection.class_ref);

    unref_class(env, Pair.class_ref);
    unref_class(env, Integer.class_ref);
    unref_class(env, Double.class_ref);
    unref_class(env, Method.class_ref);
    unref_class(env, Modifier.class_ref);
    unref_class(env, Object.class_ref);
    unref_class(env, NativePointer.class_ref);
    unref_class(env, QtJambiObject.class_ref);
    unref_class(env, Boolean.class_ref);
    unref_class(env, Long.class_ref);

    unref_class(env, Float.class_ref);
    unref_class(env, Short.class_ref);
    unref_class(env, Byte.class_ref);
    unref_class(env, Character.class_ref);
    unref_class(env, Class.class_ref);
    unref_class(env, System.class_ref);
    unref_class(env, URL.class_ref);
    unref_class(env, URLClassLoader.class_ref);
    unref_class(env, ClassLoader.class_ref);
    unref_class(env, QSignalEmitter.class_ref);

    unref_class(env, String.class_ref);
    unref_class(env, AbstractSignal.class_ref);
    unref_class(env, QObject.class_ref);
    unref_class(env, QtJambiInternal.class_ref);
    unref_class(env, MetaObjectTools.class_ref);
    unref_class(env, MetaData.class_ref);
    unref_class(env, QtJambiGuiInternal.class_ref);
    unref_class(env, Thread.class_ref);
    unref_class(env, QModelIndex.class_ref);
    unref_class(env, QtEnumerator.class_ref);

    unref_class(env, ValidationData.class_ref);
    unref_class(env, QTableArea.class_ref);
    unref_class(env, CellAtIndex.class_ref);
    unref_class(env, Qt.class_ref);
    unref_class(env, QFlags.class_ref);
    unref_class(env, QtProperty.class_ref);
    unref_class(env, QtConcurrent_MapFunctor.class_ref);
    unref_class(env, QtConcurrent_MappedFunctor.class_ref);
    unref_class(env, QtConcurrent_ReducedFunctor.class_ref);
    unref_class(env, QtConcurrent_FilteredFunctor.class_ref);

    unref_class(env, QClassPathEngine.class_ref);
    unref_class(env, QItemEditorCreatorBase.class_ref);
    unref_class(env, ResolvedEntity.class_ref);
#ifdef QTJAMBI_RETRO_JAVA
    unref_class(env, RetroTranslatorHelper.class_ref);
#else
    unref_class(env, Enum.class_ref);
#endif

    cache->d->lock.unlock();

    delete StaticCache::the_cache;
    StaticCache::the_cache = 0;
}


#define ref_class(x) (jclass) env->NewGlobalRef((jobject) x);

#define IMPLEMENT_RESOLVE_DEFAULT_FUNCTION(structName, qualifiedClassName, constructorSignature) \
    void StaticCache::resolve##structName##_internal() {                                        \
        JNIEnv *env = qtjambi_current_environment();                                          \
        Q_ASSERT(!structName.class_ref);                                                      \
        structName.class_ref = ref_class(qtjambi_find_class(env, qualifiedClassName));        \
        Q_ASSERT(structName.class_ref);                                                       \
        structName.constructor =                                                              \
            env->GetMethodID(structName.class_ref, "<init>", constructorSignature);           \
        Q_ASSERT(structName.constructor);                                                     \
}


IMPLEMENT_RESOLVE_DEFAULT_FUNCTION(HashSet, "java/util/HashSet", "()V");

void StaticCache::resolveArrayList_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!ArrayList.class_ref);

    ArrayList.class_ref = ref_class(qtjambi_find_class(env, "java/util/ArrayList"));
    Q_ASSERT(ArrayList.class_ref);

    ArrayList.constructor = env->GetMethodID(ArrayList.class_ref, "<init>", "(I)V");
    Q_ASSERT(ArrayList.constructor);
}

void StaticCache::resolveStack_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Stack.class_ref);

    Stack.class_ref = ref_class(qtjambi_find_class(env, "java/util/Stack"));
    Q_ASSERT(Stack.class_ref);

    Stack.constructor = env->GetMethodID(Stack.class_ref, "<init>", "()V");
    Q_ASSERT(Stack.constructor);
}

void StaticCache::resolveLinkedList_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!LinkedList.class_ref);

    LinkedList.class_ref = ref_class(qtjambi_find_class(env, "java/util/LinkedList"));
    Q_ASSERT(LinkedList.class_ref);

    LinkedList.constructor = env->GetMethodID(LinkedList.class_ref, "<init>", "()V");
    Q_ASSERT(LinkedList.constructor);
}

void StaticCache::resolveMap_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Map.class_ref);

    Map.class_ref = ref_class(qtjambi_find_class(env, "java/util/Map"));
    Q_ASSERT(Map.class_ref);

    Map.put = env->GetMethodID(Map.class_ref, "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    Q_ASSERT(Map.put);

    Map.size = env->GetMethodID(Map.class_ref, "size", "()I");
    Q_ASSERT(Map.size);

    Map.entrySet = env->GetMethodID(Map.class_ref, "entrySet", "()Ljava/util/Set;");
    Q_ASSERT(Map.entrySet);
}

void StaticCache::resolveMapEntry_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!MapEntry.class_ref);

    MapEntry.class_ref = ref_class(qtjambi_find_class(env, "java/util/Map$Entry"));
    Q_ASSERT(MapEntry.class_ref);

    MapEntry.getKey = env->GetMethodID(MapEntry.class_ref, "getKey", "()Ljava/lang/Object;");
    Q_ASSERT(MapEntry.getKey);

    MapEntry.getValue = env->GetMethodID(MapEntry.class_ref, "getValue", "()Ljava/lang/Object;");
    Q_ASSERT(MapEntry.getValue);
}

void StaticCache::resolveHashMap_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!HashMap.class_ref);

    HashMap.class_ref = ref_class(qtjambi_find_class(env, "java/util/HashMap"));
    Q_ASSERT(HashMap.class_ref);

    HashMap.constructor = env->GetMethodID(HashMap.class_ref, "<init>", "(I)V");
    Q_ASSERT(HashMap.constructor);
}

void StaticCache::resolveTreeMap_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!TreeMap.class_ref);

    TreeMap.class_ref = ref_class(qtjambi_find_class(env, "java/util/TreeMap"));
    Q_ASSERT(TreeMap.class_ref);

    TreeMap.constructor = env->GetMethodID(TreeMap.class_ref, "<init>", "()V");
    Q_ASSERT(TreeMap.constructor);
}

void StaticCache::resolveNullPointerException_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!NullPointerException.class_ref);

    NullPointerException.class_ref = ref_class(qtjambi_find_class(env, "java/lang/NullPointerException"));
    Q_ASSERT(NullPointerException.class_ref);
}

void StaticCache::resolveRuntimeException_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!RuntimeException.class_ref);

    RuntimeException.class_ref = ref_class(qtjambi_find_class(env, "java/lang/RuntimeException"));
    Q_ASSERT(RuntimeException.class_ref);
}

void StaticCache::resolveCollection_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Collection.class_ref);

    Collection.class_ref = ref_class(qtjambi_find_class(env, "java/util/Collection"));
    Q_ASSERT(Collection.class_ref);

    Collection.add = env->GetMethodID(Collection.class_ref, "add", "(Ljava/lang/Object;)Z");
    Collection.size = env->GetMethodID(Collection.class_ref, "size", "()I");
    Collection.toArray = env->GetMethodID(Collection.class_ref, "toArray", "()[Ljava/lang/Object;");
    Collection.clear = env->GetMethodID(Collection.class_ref, "clear", "()V");

    Q_ASSERT(Collection.add);
    Q_ASSERT(Collection.size);
    Q_ASSERT(Collection.toArray);
    Q_ASSERT(Collection.clear);
}


void StaticCache::resolvePair_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Pair.class_ref);

    Pair.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/QPair"));
    Q_ASSERT(Pair.class_ref);

    Pair.constructor = env->GetMethodID(Pair.class_ref, "<init>", "(Ljava/lang/Object;Ljava/lang/Object;)V");
    Pair.first = env->GetFieldID(Pair.class_ref, "first", "Ljava/lang/Object;");
    Pair.second = env->GetFieldID(Pair.class_ref, "second", "Ljava/lang/Object;");

    Q_ASSERT(Pair.constructor);
    Q_ASSERT(Pair.first);
    Q_ASSERT(Pair.second);
}


void StaticCache::resolveInteger_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Integer.class_ref);

    Integer.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Integer"));
    Q_ASSERT(Integer.class_ref);

    Integer.constructor = env->GetMethodID(Integer.class_ref, "<init>", "(I)V");
    Integer.intValue = env->GetMethodID(Integer.class_ref, "intValue", "()I");

    Q_ASSERT(Integer.constructor);
    Q_ASSERT(Integer.intValue);
}


void StaticCache::resolveDouble_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Double.class_ref);

    Double.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Double"));
    Q_ASSERT(Double.class_ref);

    Double.constructor = env->GetMethodID(Double.class_ref, "<init>", "(D)V");
    Double.doubleValue = env->GetMethodID(Double.class_ref, "doubleValue", "()D");

    Q_ASSERT(Double.constructor);
    Q_ASSERT(Double.doubleValue);
}


void StaticCache::resolveMethod_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Method.class_ref);

    Method.class_ref = ref_class(qtjambi_find_class(env, "java/lang/reflect/Method"));
    Q_ASSERT(Method.class_ref);

    Method.getDeclaringClass = env->GetMethodID(Method.class_ref, "getDeclaringClass",
                                                "()Ljava/lang/Class;");
    Method.getModifiers = env->GetMethodID(Method.class_ref, "getModifiers", "()I");
    Method.getName = env->GetMethodID(Method.class_ref, "getName", "()Ljava/lang/String;");

    Q_ASSERT(Method.getModifiers);
    Q_ASSERT(Method.getDeclaringClass);
    Q_ASSERT(Method.getName);
}


void StaticCache::resolveModifier_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Modifier.class_ref);

    Modifier.class_ref = ref_class(qtjambi_find_class(env, "java/lang/reflect/Modifier"));
    Q_ASSERT(Modifier.class_ref);

    Modifier.isNative = env->GetStaticMethodID(Modifier.class_ref, "isNative", "(I)Z");

    Q_ASSERT(Modifier.isNative);
}

void StaticCache::resolveObject_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Object.class_ref);

    Object.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Object"));
    Q_ASSERT(Object.class_ref);

    Object.equals = env->GetMethodID(Object.class_ref, "equals", "(Ljava/lang/Object;)Z");
    Object.toString = env->GetMethodID(Object.class_ref, "toString", "()Ljava/lang/String;");
    Object.hashCode = env->GetMethodID(Object.class_ref, "hashCode", "()I");
    Q_ASSERT(Object.equals);
    Q_ASSERT(Object.toString);
    Q_ASSERT(Object.hashCode);
}


void StaticCache::resolveNativePointer_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!NativePointer.class_ref);

    NativePointer.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/QNativePointer"));
    Q_ASSERT(NativePointer.class_ref);

    NativePointer.fromNative = env->GetStaticMethodID(NativePointer.class_ref,
                                                      "fromNative",
                                                      "(JII)Lcom/trolltech/qt/QNativePointer;");
    NativePointer.constructor = env->GetMethodID(NativePointer.class_ref, "<init>", "(III)V");
    NativePointer.indirections = env->GetFieldID(NativePointer.class_ref, "m_indirections", "I");
    NativePointer.ptr = env->GetFieldID(NativePointer.class_ref, "m_ptr", "J");

    Q_ASSERT(NativePointer.fromNative);
    Q_ASSERT(NativePointer.indirections);
    Q_ASSERT(NativePointer.ptr);
}


void StaticCache::resolveQtJambiObject_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QtJambiObject.class_ref);

    QtJambiObject.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/internal/QtJambiObject"));
    Q_ASSERT(QtJambiObject.class_ref);

    QtJambiObject.native_id = env->GetFieldID(QtJambiObject.class_ref, "native__id", "J");
    Q_ASSERT(QtJambiObject.native_id);

    QtJambiObject.disposed = env->GetMethodID(QtJambiObject.class_ref, "disposed", "()V");
}

void StaticCache::resolveBoolean_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Boolean.class_ref);

    Boolean.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Boolean"));
    Q_ASSERT(Boolean.class_ref);

    Boolean.constructor = env->GetMethodID(Boolean.class_ref, "<init>", "(Z)V");
    Q_ASSERT(Boolean.constructor);

    Boolean.booleanValue = env->GetMethodID(Boolean.class_ref, "booleanValue", "()Z");
    Q_ASSERT(Boolean.booleanValue);

    Boolean.field_FALSE = env->GetStaticFieldID(Boolean.class_ref, "FALSE", "Ljava/lang/Boolean;");
    Q_ASSERT(Boolean.field_FALSE);

    Boolean.field_TRUE = env->GetStaticFieldID(Boolean.class_ref, "TRUE", "Ljava/lang/Boolean;");
    Q_ASSERT(Boolean.field_TRUE);
}

void StaticCache::resolveLong_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Long.class_ref);

    Long.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Long"));
    Q_ASSERT(Long.class_ref);

    Long.longValue = env->GetMethodID(Long.class_ref, "longValue", "()J");
    Q_ASSERT(Long.longValue);

    Long.constructor = env->GetMethodID(Long.class_ref, "<init>", "(J)V");
    Q_ASSERT(Long.constructor);
}

void StaticCache::resolveFloat_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Float.class_ref);

    Float.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Float"));
    Q_ASSERT(Float.class_ref);

    Float.constructor = env->GetMethodID(Float.class_ref, "<init>", "(F)V");
    Q_ASSERT(Float.constructor);

    Float.floatValue = env->GetMethodID(Float.class_ref, "floatValue", "()F");
    Q_ASSERT(Float.floatValue);
}

void StaticCache::resolveShort_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Short.class_ref);

    Short.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Short"));
    Q_ASSERT(Short.class_ref);

    Short.constructor = env->GetMethodID(Short.class_ref, "<init>", "(S)V");
    Q_ASSERT(Short.constructor);

    Short.shortValue = env->GetMethodID(Short.class_ref, "shortValue", "()S");
    Q_ASSERT(Short.shortValue);
}

void StaticCache::resolveByte_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Byte.class_ref);

    Byte.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Byte"));
    Q_ASSERT(Byte.class_ref);

    Byte.constructor = env->GetMethodID(Byte.class_ref, "<init>", "(B)V");
    Q_ASSERT(Byte.constructor);

    Byte.byteValue = env->GetMethodID(Byte.class_ref, "byteValue", "()B");
    Q_ASSERT(Byte.byteValue);
}

void StaticCache::resolveCharacter_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Character.class_ref);

    Character.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Character"));
    Q_ASSERT(Character.class_ref);

    Character.charValue = env->GetMethodID(Character.class_ref, "charValue", "()C");
    Q_ASSERT(Character.charValue);

    Character.constructor = env->GetMethodID(Character.class_ref, "<init>", "(C)V");
    Q_ASSERT(Character.constructor);
}

void StaticCache::resolveClass_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Class.class_ref);

    Class.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Class"));
    Q_ASSERT(Class.class_ref);

    Class.getName = env->GetMethodID(Class.class_ref, "getName", "()Ljava/lang/String;");
    Q_ASSERT(Class.getName);

    Class.getDeclaredMethods = env->GetMethodID(Class.class_ref, "getDeclaredMethods",
        "()[Ljava/lang/reflect/Method;");
    Q_ASSERT(Class.getDeclaredMethods);

#if !defined(QTJAMBI_RETRO_JAVA)
    Class.getEnumConstants = env->GetMethodID(Class.class_ref, "getEnumConstants", "()[Ljava/lang/Object;");
    Q_ASSERT(Class.getEnumConstants);
#endif
}

void StaticCache::resolveSystem_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!System.class_ref);

    System.class_ref = ref_class(qtjambi_find_class(env, "java/lang/System"));
    Q_ASSERT(System.class_ref);

    System.gc = env->GetStaticMethodID(System.class_ref, "gc", "()V");
    Q_ASSERT(System.gc);

    System.getProperty = env->GetStaticMethodID(System.class_ref, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    Q_ASSERT(System.getProperty);
}

void StaticCache::resolveURL_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!URL.class_ref);

    URL.class_ref = ref_class(qtjambi_find_class(env, "java/net/URL"));
    Q_ASSERT(URL.class_ref);

    URL.constructor = env->GetMethodID(URL.class_ref, "<init>", "(Ljava/lang/String;)V");
    Q_ASSERT(URL.constructor);
}

void StaticCache::resolveURLClassLoader_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!URLClassLoader.class_ref);

    URLClassLoader.class_ref = ref_class(qtjambi_find_class(env, "java/net/URLClassLoader"));
    Q_ASSERT(URLClassLoader.class_ref);

    URLClassLoader.newInstance = env->GetStaticMethodID(URLClassLoader.class_ref, "newInstance", "([Ljava/net/URL;Ljava/lang/ClassLoader;)Ljava/net/URLClassLoader;");
    Q_ASSERT(URLClassLoader.newInstance);

    URLClassLoader.addURL = env->GetMethodID(URLClassLoader.class_ref, "addURL", "(Ljava/net/URL;)V");
    Q_ASSERT(URLClassLoader.addURL);
}

void StaticCache::resolveClassLoader_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!ClassLoader.class_ref);

    ClassLoader.class_ref = ref_class(qtjambi_find_class(env, "java/lang/ClassLoader"));
    Q_ASSERT(ClassLoader.class_ref);

    ClassLoader.loadClass = env->GetMethodID(ClassLoader.class_ref, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    Q_ASSERT(ClassLoader.loadClass);
}

void StaticCache::resolveQSignalEmitter_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QSignalEmitter.class_ref);

    QSignalEmitter.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/QSignalEmitter"));
    Q_ASSERT(QSignalEmitter.class_ref);

    QSignalEmitter.disconnect = env->GetMethodID(QSignalEmitter.class_ref, "disconnect",
        "(Ljava/lang/Object;)V");
}

void StaticCache::resolveQObject_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QObject.class_ref);

    QObject.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/core/QObject"));
    Q_ASSERT(QObject.class_ref);
}

void StaticCache::resolveAbstractSignal_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!AbstractSignal.class_ref);

    AbstractSignal.class_ref =
        ref_class(qtjambi_find_class(env, "com/trolltech/qt/QSignalEmitter$AbstractSignal"));
    Q_ASSERT(AbstractSignal.class_ref);

    AbstractSignal.inCppEmission = env->GetFieldID(AbstractSignal.class_ref,
                                                   "inCppEmission", "Z");
    Q_ASSERT(AbstractSignal.inCppEmission);

    AbstractSignal.inJavaEmission = env->GetFieldID(AbstractSignal.class_ref, "inJavaEmission", "Z");
    Q_ASSERT(AbstractSignal.inJavaEmission);

    AbstractSignal.connect = env->GetMethodID(AbstractSignal.class_ref,
                                              "connect",
                                              "(Ljava/lang/Object;"
                                               "Ljava/lang/String;"
                                               "Lcom/trolltech/qt/core/Qt$ConnectionType;)V");
    Q_ASSERT(AbstractSignal.connect);

    AbstractSignal.connectSignalMethod = env->GetMethodID(AbstractSignal.class_ref,
                                                          "connectSignalMethod",
                                                          "(Ljava/lang/reflect/Method;Ljava/lang/Object;I)V");
    Q_ASSERT(AbstractSignal.connectSignalMethod);

    AbstractSignal.removeConnection = env->GetMethodID(AbstractSignal.class_ref,
                                                       "removeConnection",
                                                       "(Ljava/lang/Object;Ljava/lang/reflect/Method;)Z");
    Q_ASSERT(AbstractSignal.removeConnection);
}

void StaticCache::resolveQtJambiInternal_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    QtJambiInternal.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/internal/QtJambiInternal"));
    Q_ASSERT(QtJambiInternal.class_ref);

    QtJambiInternal.lookupSignal = env->GetStaticMethodID(QtJambiInternal.class_ref,
                                                          "lookupSignal",
                                                          "(Lcom/trolltech/qt/internal/QSignalEmitterInternal;"
                                                           "Ljava/lang/String;"
                                                          ")Lcom/trolltech/qt/internal/QSignalEmitterInternal$AbstractSignalInternal;");
    Q_ASSERT(QtJambiInternal.lookupSignal);

    QtJambiInternal.lookupSlot = env->GetStaticMethodID(QtJambiInternal.class_ref,
                                                        "lookupSlot",
                                                        "(Ljava/lang/Object;"
                                                         "Ljava/lang/String;"
                                                         "Z"
                                                        ")Ljava/lang/reflect/Method;");
    Q_ASSERT(QtJambiInternal.lookupSlot);

     QtJambiInternal.findEmitMethod = env->GetStaticMethodID(QtJambiInternal.class_ref,
                                                           "findEmitMethod",
                                                           "(Lcom/trolltech/qt/internal/QSignalEmitterInternal$AbstractSignalInternal;)Ljava/lang/reflect/Method;");
    Q_ASSERT(QtJambiInternal.findEmitMethod);

    QtJambiInternal.isImplementedInJava =
        env->GetStaticMethodID(QtJambiInternal.class_ref, "isImplementedInJava",
                               "(Ljava/lang/reflect/Method;)Z");
     Q_ASSERT(QtJambiInternal.isImplementedInJava);

    QtJambiInternal.findGeneratedSuperclass =
        env->GetStaticMethodID(QtJambiInternal.class_ref, "findGeneratedSuperclass",
                               "(Ljava/lang/Object;)Ljava/lang/Class;");
    Q_ASSERT(QtJambiInternal.findGeneratedSuperclass);


    QtJambiInternal.writeSerializableJavaObject = env->GetStaticMethodID(QtJambiInternal.class_ref,
                                     "writeSerializableJavaObject",
                                     "(Lcom/trolltech/qt/core/QDataStream;"
                                     "Ljava/lang/Object;"
                                     ")V");
    Q_ASSERT(QtJambiInternal.writeSerializableJavaObject);


    QtJambiInternal.readSerializableJavaObject = env->GetStaticMethodID(QtJambiInternal.class_ref,
                                     "readSerializableJavaObject",
                                     "(Lcom/trolltech/qt/core/QDataStream;)"
                                     "Ljava/lang/Object;");
    Q_ASSERT(QtJambiInternal.readSerializableJavaObject);

    QtJambiInternal.isGeneratedClass = env->GetStaticMethodID(QtJambiInternal.class_ref,
                                                              "isGeneratedClass",
                                                              "(Ljava/lang/Class;)Z");
    Q_ASSERT(QtJambiInternal.isGeneratedClass);


    QtJambiInternal.signalParameters = env->GetStaticMethodID(QtJambiInternal.class_ref, "signalParameters",
                                                              "(Lcom/trolltech/qt/internal/QSignalEmitterInternal$AbstractSignalInternal;)Ljava/lang/String;");
    Q_ASSERT(QtJambiInternal.signalParameters);


    QtJambiInternal.signalMatchesSlot = env->GetStaticMethodID(QtJambiInternal.class_ref, "signalMatchesSlot",
                                                               "(Ljava/lang/String;Ljava/lang/String;)Z");
    Q_ASSERT(QtJambiInternal.signalMatchesSlot);
}

void StaticCache::resolveMetaObjectTools_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    MetaObjectTools.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/internal/MetaObjectTools"));
    Q_ASSERT(MetaObjectTools.class_ref);

    MetaObjectTools.buildMetaData = env->GetStaticMethodID(MetaObjectTools.class_ref,
                                                           "buildMetaData",
                                                           "(Ljava/lang/Class;)"
                                                           "Lcom/trolltech/qt/internal/MetaObjectTools$MetaData;");
    Q_ASSERT(MetaObjectTools.buildMetaData);

    MetaObjectTools.methodSignature = env->GetStaticMethodID(MetaObjectTools.class_ref, "methodSignature",
                                                             "(Ljava/lang/reflect/Method;)Ljava/lang/String;");
    Q_ASSERT(MetaObjectTools.methodSignature);

    MetaObjectTools.methodSignature2 = env->GetStaticMethodID(MetaObjectTools.class_ref, "methodSignature",
                                                              "(Ljava/lang/reflect/Method;Z)Ljava/lang/String;");
    Q_ASSERT(MetaObjectTools.methodSignature2);

    MetaObjectTools.getEnumForQFlags = env->GetStaticMethodID(MetaObjectTools.class_ref, "getEnumForQFlags",
                                                              "(Ljava/lang/Class;)Ljava/lang/Class;");
    Q_ASSERT(MetaObjectTools.getEnumForQFlags);

}

void StaticCache::resolveMetaData_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    MetaData.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/internal/MetaObjectTools$MetaData"));
    Q_ASSERT(MetaData.class_ref);

    MetaData.metaData = env->GetFieldID(MetaData.class_ref, "metaData", "[I");
    Q_ASSERT(MetaData.metaData);

    MetaData.stringData = env->GetFieldID(MetaData.class_ref, "stringData", "[B");
    Q_ASSERT(MetaData.stringData);

    MetaData.signalsArray = env->GetFieldID(MetaData.class_ref, "signalsArray", "[Ljava/lang/reflect/Field;");
    Q_ASSERT(MetaData.signalsArray);

    MetaData.slotsArray = env->GetFieldID(MetaData.class_ref, "slotsArray", "[Ljava/lang/reflect/Method;");
    Q_ASSERT(MetaData.slotsArray);

    MetaData.propertyReadersArray = env->GetFieldID(MetaData.class_ref, "propertyReadersArray", "[Ljava/lang/reflect/Method;");
    Q_ASSERT(MetaData.propertyReadersArray);

    MetaData.propertyWritersArray = env->GetFieldID(MetaData.class_ref, "propertyWritersArray", "[Ljava/lang/reflect/Method;");
    Q_ASSERT(MetaData.propertyWritersArray);

    MetaData.propertyResettersArray = env->GetFieldID(MetaData.class_ref, "propertyResettersArray", "[Ljava/lang/reflect/Method;");
    Q_ASSERT(MetaData.propertyResettersArray);

    MetaData.propertyDesignablesArray = env->GetFieldID(MetaData.class_ref, "propertyDesignablesArray", "[Ljava/lang/reflect/Method;");
    Q_ASSERT(MetaData.propertyDesignablesArray);

    MetaData.extraDataArray = env->GetFieldID(MetaData.class_ref, "extraDataArray", "[Ljava/lang/Class;");
    Q_ASSERT(MetaData.extraDataArray);

    MetaData.originalSignatures = env->GetFieldID(MetaData.class_ref, "originalSignatures", "[Ljava/lang/String;");
    Q_ASSERT(MetaData.originalSignatures);
}

void StaticCache::resolveQtJambiGuiInternal_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    QtJambiGuiInternal.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/QtJambiGuiInternal"));
    Q_ASSERT(QtJambiGuiInternal.class_ref);

    QtJambiGuiInternal.endPaint = env->GetStaticMethodID(QtJambiGuiInternal.class_ref,
                                                      "endPaint",
                                                      "(Lcom/trolltech/qt/gui/QWidget;)V");
    Q_ASSERT(QtJambiGuiInternal.endPaint);
}

void StaticCache::resolveString_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!String.class_ref);

    String.class_ref = ref_class(qtjambi_find_class(env, "java/lang/String"));
    Q_ASSERT(String.class_ref);
}

void StaticCache::resolveThread_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Thread.class_ref);

    Thread.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Thread"));
    Q_ASSERT(Thread.class_ref);

    Thread.currentThread = env->GetStaticMethodID(Thread.class_ref, "currentThread", "()Ljava/lang/Thread;");
    Q_ASSERT(Thread.currentThread);

    Thread.getContextClassLoader = env->GetMethodID(Thread.class_ref, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    Q_ASSERT(Thread.getContextClassLoader);

    Thread.setContextClassLoader = env->GetMethodID(Thread.class_ref, "setContextClassLoader", "(Ljava/lang/ClassLoader;)V");
    Q_ASSERT(Thread.setContextClassLoader);
}

void StaticCache::resolveQModelIndex_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QModelIndex.class_ref);

    QModelIndex.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/core/QModelIndex"));
    Q_ASSERT(QModelIndex.class_ref);

    QModelIndex.constructor = env->GetMethodID(QModelIndex.class_ref,
                                               "<init>",
                                               "(IIJLcom/trolltech/qt/core/QAbstractItemModel;)V");
    Q_ASSERT(QModelIndex.constructor);

    QModelIndex.field_row = env->GetFieldID(QModelIndex.class_ref, "row", "I");
    QModelIndex.field_column = env->GetFieldID(QModelIndex.class_ref, "column", "I");
    QModelIndex.field_internalId = env->GetFieldID(QModelIndex.class_ref, "internalId", "J");
    QModelIndex.field_model = env->GetFieldID(QModelIndex.class_ref, "model",
                                              "Lcom/trolltech/qt/core/QAbstractItemModel;");
    Q_ASSERT(QModelIndex.field_row);
    Q_ASSERT(QModelIndex.field_column);
    Q_ASSERT(QModelIndex.field_internalId);
    Q_ASSERT(QModelIndex.field_model);
}

void StaticCache::resolveQtEnumerator_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QtEnumerator.class_ref);

    QtEnumerator.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/QtEnumerator"));
    Q_ASSERT(QtEnumerator.class_ref);

    QtEnumerator.value = env->GetMethodID(QtEnumerator.class_ref, "value", "()I");
    Q_ASSERT(QtEnumerator.value);

}

void StaticCache::resolveValidationData_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!ValidationData.class_ref);

    ValidationData.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/gui/QValidator$QValidationData"));
    Q_ASSERT(ValidationData.class_ref);

    ValidationData.constructor = env->GetMethodID(ValidationData.class_ref, "<init>", "(Ljava/lang/String;I)V");
    Q_ASSERT(ValidationData.constructor);

    ValidationData.string = env->GetFieldID(ValidationData.class_ref, "string", "Ljava/lang/String;");
    Q_ASSERT(ValidationData.string);

    ValidationData.position = env->GetFieldID(ValidationData.class_ref, "position", "I");
    Q_ASSERT(ValidationData.position);
}

void StaticCache::resolveQTableArea_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QTableArea.class_ref);

    QTableArea.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/gui/QTableArea"));
    Q_ASSERT(QTableArea.class_ref);

    QTableArea.constructor = env->GetMethodID(QTableArea.class_ref, "<init>", "(IIII)V");
    Q_ASSERT(QTableArea.constructor);

    QTableArea.row = env->GetFieldID(QTableArea.class_ref, "row", "I");
    Q_ASSERT(QTableArea.row);

    QTableArea.column = env->GetFieldID(QTableArea.class_ref, "column", "I");
    Q_ASSERT(QTableArea.column);

    QTableArea.rowCount = env->GetFieldID(QTableArea.class_ref, "rowCount", "I");
    Q_ASSERT(QTableArea.rowCount);

    QTableArea.columnCount = env->GetFieldID(QTableArea.class_ref, "columnCount", "I");
    Q_ASSERT(QTableArea.columnCount);
}

void StaticCache::resolveCellAtIndex_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!CellAtIndex.class_ref);

    CellAtIndex.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/gui/QAccessibleTableInterface$CellAtIndex"));
    Q_ASSERT(CellAtIndex.class_ref);

    CellAtIndex.constructor = env->GetMethodID(CellAtIndex.class_ref, "<init>", "(IIIIZ)V");
    Q_ASSERT(CellAtIndex.constructor);

    CellAtIndex.isSelected = env->GetFieldID(CellAtIndex.class_ref, "isSelected", "Z");
    Q_ASSERT(CellAtIndex.isSelected);
}

void StaticCache::resolveQt_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Qt.class_ref);

    Qt.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/core/Qt"));
    Q_ASSERT(Qt.class_ref);
}

void StaticCache::resolveQFlags_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QFlags.class_ref);

    QFlags.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/QFlags"));
    Q_ASSERT(QFlags.class_ref);
}

void StaticCache::resolveQtProperty_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QtProperty.class_ref);

    QtProperty.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/QtProperty"));
    Q_ASSERT(QtProperty.class_ref);

    QtProperty.constructor = env->GetMethodID(QtProperty.class_ref, "<init>", "(ZZZZLjava/lang/String;)V");
    Q_ASSERT(QtProperty.constructor);
}

void StaticCache::resolveQtConcurrent_MapFunctor_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QtConcurrent_MapFunctor.class_ref);

    QtConcurrent_MapFunctor.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/core/QtConcurrent$MapFunctor"));
    Q_ASSERT(QtConcurrent_MapFunctor.class_ref);

    QtConcurrent_MapFunctor.map = env->GetMethodID(QtConcurrent_MapFunctor.class_ref, "map", "(Ljava/lang/Object;)V");
    Q_ASSERT(QtConcurrent_MapFunctor.map);
}

void StaticCache::resolveQtConcurrent_MappedFunctor_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QtConcurrent_MappedFunctor.class_ref);

    QtConcurrent_MappedFunctor.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/core/QtConcurrent$MappedFunctor"));
    Q_ASSERT(QtConcurrent_MappedFunctor.class_ref);

    QtConcurrent_MappedFunctor.map = env->GetMethodID(QtConcurrent_MappedFunctor.class_ref, "map", "(Ljava/lang/Object;)Ljava/lang/Object;");
    Q_ASSERT(QtConcurrent_MappedFunctor.map);
}

void StaticCache::resolveQtConcurrent_ReducedFunctor_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QtConcurrent_ReducedFunctor.class_ref);

    QtConcurrent_ReducedFunctor.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/core/QtConcurrent$ReducedFunctor"));
    Q_ASSERT(QtConcurrent_ReducedFunctor.class_ref);

    QtConcurrent_ReducedFunctor.reduce = env->GetMethodID(QtConcurrent_ReducedFunctor.class_ref, "reduce", "(Ljava/lang/Object;Ljava/lang/Object;)V");
    Q_ASSERT(QtConcurrent_ReducedFunctor.reduce);

    QtConcurrent_ReducedFunctor.defaultResult = env->GetMethodID(QtConcurrent_ReducedFunctor.class_ref, "defaultResult", "()Ljava/lang/Object;");
    Q_ASSERT(QtConcurrent_ReducedFunctor.defaultResult);
}

void StaticCache::resolveQtConcurrent_FilteredFunctor_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QtConcurrent_FilteredFunctor.class_ref);

    QtConcurrent_FilteredFunctor.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/core/QtConcurrent$FilteredFunctor"));
    Q_ASSERT(QtConcurrent_FilteredFunctor.class_ref);

    QtConcurrent_FilteredFunctor.filter = env->GetMethodID(QtConcurrent_FilteredFunctor.class_ref, "filter", "(Ljava/lang/Object;)Z");
    Q_ASSERT(QtConcurrent_FilteredFunctor.filter);
}

void StaticCache::resolveQClassPathEngine_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QClassPathEngine.class_ref);

    QClassPathEngine.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/internal/fileengine/QClassPathEngine"));
    Q_ASSERT(QClassPathEngine.class_ref);

    QClassPathEngine.constructor = env->GetMethodID(QClassPathEngine.class_ref, "<init>", "(Ljava/lang/String;)V");
    Q_ASSERT(QClassPathEngine.constructor);
}

void StaticCache::resolveQItemEditorCreatorBase_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!QItemEditorCreatorBase.class_ref);

    QItemEditorCreatorBase.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/gui/QItemEditorCreatorBase"));
    Q_ASSERT(QItemEditorCreatorBase.class_ref);

    QItemEditorCreatorBase.createWidget = env->GetMethodID(QItemEditorCreatorBase.class_ref, "createWidget",
        "(Lcom/trolltech/qt/gui/QWidget;)Lcom/trolltech/qt/gui/QWidget;");
    Q_ASSERT(QItemEditorCreatorBase.createWidget);

    QItemEditorCreatorBase.valuePropertyName = env->GetMethodID(QItemEditorCreatorBase.class_ref, "valuePropertyName",
        "()Lcom/trolltech/qt/core/QByteArray;");
    Q_ASSERT(QItemEditorCreatorBase.valuePropertyName);
}

void StaticCache::resolveResolvedEntity_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!ResolvedEntity.class_ref);

    ResolvedEntity.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/xml/QXmlEntityResolver$ResolvedEntity"));
    Q_ASSERT(ResolvedEntity.class_ref);

    ResolvedEntity.constructor = env->GetMethodID(ResolvedEntity.class_ref, "<init>", "(ZLcom/trolltech/qt/xml/QXmlInputSource;)V");
    Q_ASSERT(ResolvedEntity.constructor);

    ResolvedEntity.error = env->GetFieldID(ResolvedEntity.class_ref, "error", "Z");
    Q_ASSERT(ResolvedEntity.error);

    ResolvedEntity.inputSource = env->GetFieldID(ResolvedEntity.class_ref, "inputSource", "Lcom/trolltech/qt/xml/QXmlInputSource;");
    Q_ASSERT(ResolvedEntity.inputSource);
}

#ifdef QTJAMBI_RETRO_JAVA
void StaticCache::resolveRetroTranslatorHelper_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!RetroTranslatorHelper.class_ref);

    RetroTranslatorHelper.class_ref = ref_class(qtjambi_find_class(env, "com/trolltech/qt/internal/RetroTranslatorHelper"));
    Q_ASSERT(RetroTranslatorHelper.class_ref);

    RetroTranslatorHelper.getEnumConstants = env->GetStaticMethodID(RetroTranslatorHelper.class_ref, "getEnumConstants", "(Ljava/lang/Class;)[Ljava/lang/Object;");
    Q_ASSERT(RetroTranslatorHelper.getEnumConstants);

    RetroTranslatorHelper.isEnumType = env->GetStaticMethodID(RetroTranslatorHelper.class_ref, "isEnumType", "(Ljava/lang/Class;)Z");
    Q_ASSERT(RetroTranslatorHelper.isEnumType);

    RetroTranslatorHelper.enumOrdinal = env->GetStaticMethodID(RetroTranslatorHelper.class_ref, "enumOrdinal", "(Ljava/lang/Object;)I");
    Q_ASSERT(RetroTranslatorHelper.enumOrdinal);
}
#else
void StaticCache::resolveEnum_internal()
{
    JNIEnv *env = qtjambi_current_environment();

    Q_ASSERT(!Enum.class_ref);

    Enum.class_ref = ref_class(qtjambi_find_class(env, "java/lang/Enum"));
    Q_ASSERT(Enum.class_ref);

    Enum.ordinal = env->GetMethodID(Enum.class_ref, "ordinal", "()I");
    Q_ASSERT(Enum.ordinal);

    Enum.name = env->GetMethodID(Enum.class_ref, "name", "()Ljava/lang/String;");
    Q_ASSERT(Enum.name);
}
#endif // QTJAMBI_RETRO_JAVA

void qtjambi_cache_prune(JNIEnv *env)
{
    int i;

    i = qtjambi_object_cache_operation_count();
#if defined(QTJAMBI_DEBUG_TOOLS)
    fprintf(stderr, "qtjambi_object_cache_operation_count()=%d\n", i);
#endif

    i = QtJambiLink::qtjambi_object_cache_prune();
#if defined(QTJAMBI_DEBUG_TOOLS)
    fprintf(stderr, "QtJambiLink::qtjambi_object_cache_operation_prune()=%d\n", i);
#endif

#if defined(QTJAMBI_DEBUG_TOOLS)
    i = QtJambiLink::QtJambiLinkList_count();
    fprintf(stderr, "QtJambiLink::QtJambiLinkList_count()=%d\n", i);
#endif

    i = qtjambi_metaobject_prune(env);
#if defined(QTJAMBI_DEBUG_TOOLS)
    fprintf(stderr, "qtjambi_metaobject_prune()=%d\n", i);
#endif

    i = qtjambi_cache_prune_functiontable();
#if defined(QTJAMBI_DEBUG_TOOLS)
    fprintf(stderr, "qtjambi_cache_prune_functiontable()=%d\n", i);
#endif

    i = qtjambi_cache_prune_superclass(env);
#if defined(QTJAMBI_DEBUG_TOOLS)
    fprintf(stderr, "qtjambi_cache_prune_superclass()=%d\n", i);
#endif

    i = qtjambi_cache_prune_field();
#if defined(QTJAMBI_DEBUG_TOOLS)
    fprintf(stderr, "qtjambi_cache_prune_field()=%d\n", i);
#endif

    i = qtjambi_cache_prune_method();
#if defined(QTJAMBI_DEBUG_TOOLS)
    fprintf(stderr, "qtjambi_cache_prune_method()=%d\n", i);
#endif

    i = qtjambi_cache_prune_class(env);
#if defined(QTJAMBI_DEBUG_TOOLS)
    fprintf(stderr, "qtjambi_cache_prune_class()=%d\n", i);
#endif

    StaticCache::shutdown(env);
}
