/****************************************************************************
**
** Copyright (C) 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the QtAddOn.JsonDb module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "jsondb-trace.h"

#include <QDebug>
#include <QRegExp>
#include <QJSValue>
#include <QStringList>

#include <QtJsonDbQson/private/qson_p.h>
#include "qsonconversion.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include "jsondbbtreestorage.h"
#include "jsondb-strings.h"
#include "jsondb-error.h"
#include "json.h"

#include "jsondb.h"
#include "jsondb-proxy.h"
#include "objecttable.h"

namespace QtAddOn { namespace JsonDb {

void JsonDb::initMap(const QString &partition)
{
    if (gVerbose) qDebug() << "Initializing views on partition" << partition;
    JsonDbBtreeStorage *storage = findPartition(partition);
    {
        QsonMap getObjectResponse = getObject(JsonDbString::kTypeStr, "Map", partition);
        QsonList mrdList = getObjectResponse.subList("result");
        for (int i = 0; i < mrdList.size(); ++i) {
            QsonMap mrd = mrdList.at<QsonMap>(i);
            createMapDefinition(mrd, false, partition);
            storage->addView(mrd.value<QString>("targetType"));
        }
    }
    {
        QsonMap getObjectResponse = getObject(JsonDbString::kTypeStr, "Reduce", partition);
        QsonList mrdList = getObjectResponse.subList("result");
        for (int i = 0; i < mrdList.size(); ++i) {
            QsonMap mrd = mrdList.at<QsonMap>(i);
            createReduceDefinition(mrd, false, partition);
            storage->addView(mrd.value<QString>("targetType"));
        }
    }
}

void JsonDb::createMapDefinition(QsonMap mapDefinition, bool firstTime, const QString &partition)
{
    QString targetType = mapDefinition.valueString("targetType");
    QString sourceType = mapDefinition.valueString("sourceType");

    if (gVerbose)
        qDebug() << "createMapDefinition" << sourceType << targetType;

    JsonDbBtreeStorage *storage = findPartition(partition);
    storage->addView(targetType);
    storage->addIndex("sourceUuids.*", "string", targetType);
    storage->addIndex("key", "string", targetType);
    storage->addIndex("mapUuid", "string", targetType);

    JsonDbMapDefinition *def = new JsonDbMapDefinition(this, mOwner, partition, mapDefinition, this);
    mMapDefinitionsBySource.insert(sourceType, def);
    mMapDefinitionsByTarget.insert(targetType, def);

    if (firstTime && def->isActive()) {
        QsonMap getObjectResponse = getObject(JsonDbString::kTypeStr, sourceType);
        QsonList objects = getObjectResponse.subList("result");
        for (int i = 0; i < objects.size(); i++)
            def->mapObject(objects.objectAt(i));
    }
}

void JsonDb::removeMapDefinition(QsonMap mapDefinition)
{
    QString targetType = mapDefinition.valueString("targetType");
    QString sourceType = mapDefinition.valueString("sourceType");

    JsonDbMapDefinition *def = 0;
    foreach (JsonDbMapDefinition *d, mMapDefinitionsBySource.values(sourceType)) {
        if (d->uuid() == mapDefinition.valueString(JsonDbString::kUuidStr)) {
            def = d;
            mMapDefinitionsBySource.remove(sourceType, def);
            mMapDefinitionsByTarget.remove(targetType, def);
            break;
        }
    }

    // remove the output objects
    QsonMap getObjectResponse = getObject("mapUuid", mapDefinition.valueString(JsonDbString::kUuidStr), targetType);
    QsonList objects = getObjectResponse.subList("result");
    for (int i = 0; i < objects.size(); i++)
      removeViewObject(mOwner, objects.objectAt(i));
}

void JsonDb::createReduceDefinition(QsonMap reduceDefinition, bool firstTime, const QString &partition)
{
    QString targetType = reduceDefinition.valueString("targetType");
    QString sourceType = reduceDefinition.valueString("sourceType");

    if (gDebug)
        qDebug() << "createReduceDefinition" << targetType << sourceType;

    JsonDbBtreeStorage *storage = findPartition(partition);
    storage->addView(targetType);

    JsonDbReduceDefinition *def = new JsonDbReduceDefinition(this, mOwner, partition, reduceDefinition, this);
    mReduceDefinitionsBySource.insert(sourceType, def);
    mReduceDefinitionsByTarget.insert(targetType, def);

    storage->addIndex("sourceUuids.*", "string", targetType);
    storage->addIndex(def->sourceKeyName(), "string", sourceType);
    storage->addIndex("key", "string", targetType);
    storage->addIndex("reduceUuid", "string", targetType);

    if (firstTime && def->isActive()) {
        QsonMap getObjectResponse = getObject(JsonDbString::kTypeStr, sourceType);
        QsonList objects = getObjectResponse.subList("result");
        for (int i = 0; i < objects.size(); i++)
            def->updateObject(QsonMap(), objects.objectAt(i));
    }
}

void JsonDb::removeReduceDefinition(QsonMap reduceDefinition)
{
    QString targetType = reduceDefinition.valueString("targetType");
    QString sourceType = reduceDefinition.valueString("sourceType");

    if (gVerbose)
        qDebug() << "removeReduceDefinition" << sourceType <<  targetType;

    JsonDbReduceDefinition *def = 0;
    foreach (JsonDbReduceDefinition *d, mReduceDefinitionsBySource.values(sourceType)) {
        if (d->uuid() == reduceDefinition.valueString(JsonDbString::kUuidStr)) {
            def = d;
            mReduceDefinitionsBySource.remove(def->sourceType(), def);
            mReduceDefinitionsByTarget.remove(def->targetType(), def);
            break;
        }
    }
    // remove the output objects
    QsonMap getObjectResponse = getObject("reduceUuid", reduceDefinition.valueString(JsonDbString::kUuidStr), targetType);
    QsonList objects = getObjectResponse.subList("result");
    for (int i = 0; i < objects.size(); i++)
      removeViewObject(mOwner, objects.objectAt(i));
    //TODO: actually remove the table
}

void JsonDb::updateView(const QString &viewType, const QString &partitionName)
{
    if (viewType.isEmpty())
        return;
    JsonDbBtreeStorage *partition = findPartition(partitionName);
    ObjectTable *objectTable = partition->mainObjectTable();
    ObjectTable *targetTable = partition->findObjectTable(viewType);
    if ((objectTable == targetTable)
        || (objectTable->stateNumber() == targetTable->stateNumber()))
        return;

    updateMap(viewType, partitionName);
    updateReduce(viewType, partitionName);
}

void JsonDb::updateMap(const QString &viewType, const QString &partitionName)
{
    JsonDbBtreeStorage *partition = findPartition(partitionName);
    ObjectTable *targetTable = partition->findObjectTable(viewType);
    quint32 targetStateNumber = qMax(1u, targetTable->stateNumber());
    if (0) qDebug() << "JsonDb::updateMap" << viewType << targetStateNumber << mViewsUpdating.contains(viewType) << "targetStateNumber" << targetStateNumber;
    if (mViewsUpdating.contains(viewType))
        return;
    mViewsUpdating.insert(viewType);

    QHash<QString,JsonDbMapDefinition*> mapDefinitions;
    QSet<ObjectTable*>                  sourceTables;
    QMultiMap<ObjectTable*,QString>     objectTableSourceType;
    QMap<QString, QsonMap> addedMapDefinitions;   // uuid -> added definition
    QMap<QString, QsonMap> removedMapDefinitions; // uuid -> removed definition
    quint32 endStateNumber =
        findUpdatedMapReduceDefinitions(partition, JsonDbString::kMapTypeStr,
                                        viewType, targetStateNumber, addedMapDefinitions, removedMapDefinitions);
    for (QMap<QString,JsonDbMapDefinition*>::const_iterator it = mMapDefinitionsByTarget.find(viewType);
         (it != mMapDefinitionsByTarget.end() && it.key() == viewType);
         ++it) {
        JsonDbMapDefinition *def = it.value();
        const QString &sourceType = def->sourceType();
        ObjectTable *sourceTable = partition->findObjectTable(sourceType);

        sourceTables.insert(sourceTable);
        objectTableSourceType.insert(sourceTable, sourceType);
        mapDefinitions.insert(sourceType, def);
    }
    if (sourceTables.isEmpty() && addedMapDefinitions.isEmpty() && removedMapDefinitions.isEmpty()) {
        mViewsUpdating.remove(viewType);
        return;
    }

    for (QMap<ObjectTable*,QString>::const_iterator it = objectTableSourceType.begin();
         it != objectTableSourceType.end();
         ++it) {
        QString sourceType = it.value();
        // make sure the source is updated
        updateView(sourceType);

        if (!endStateNumber) {
            ObjectTable *sourceTable = it.key();
            quint32 sourceStateNumber = sourceTable->stateNumber();
            endStateNumber = sourceStateNumber;
        }
    }

    // this transaction private to targetTable
    targetTable->begin();

    for (QMap<QString,QsonMap>::const_iterator it = removedMapDefinitions.begin();
         it != removedMapDefinitions.end();
         ++it) {
        QsonMap def = it.value();
        removeMapDefinition(def);
    }
    for (QMap<QString,QsonMap>::const_iterator it = addedMapDefinitions.begin();
         it != addedMapDefinitions.end();
         ++it) {
        QsonMap def = it.value();
        createMapDefinition(def, true, partitionName);
    }

    for (QSet<ObjectTable*>::const_iterator it = sourceTables.begin();
         it != sourceTables.end();
         ++it) {
        ObjectTable *sourceTable = *it;
        QStringList sourceTypes = objectTableSourceType.values(sourceTable);
        QsonMap changes = sourceTable->changesSince(targetStateNumber).subObject("result");
        quint32 count = changes.valueInt("count", 0);
        QsonList changeList = changes.subList("changes");
        for (quint32 i = 0; i < count; i++) {
            QsonMap change = changeList.objectAt(i).toMap();
            QsonMap before = change.subObject("before").toMap();
            QsonMap after = change.subObject("after").toMap();
            if (!before.isEmpty()) {
                QString sourceType = before.valueString(JsonDbString::kTypeStr);
                if (sourceTypes.contains(sourceType)) {
                    mapDefinitions.value(sourceType)->unmapObject(before);
                }
            }
            if (!after.isEmpty()) {
                QString sourceType = after.valueString(JsonDbString::kTypeStr);
                if (sourceTypes.contains(sourceType)) {
                    Q_ASSERT(mapDefinitions.value(sourceType));
                    mapDefinitions.value(sourceType)->mapObject(after);
                }
            }
        }
    }

    targetTable->commit(endStateNumber);
    mViewsUpdating.remove(viewType);
}

quint32 JsonDb::findUpdatedMapReduceDefinitions(JsonDbBtreeStorage *partition, const QString &definitionType,
                                                const QString &viewType, quint32 targetStateNumber,
                                                QMap<QString,QsonMap> &addedDefinitions, QMap<QString,QsonMap> &removedDefinitions) const
{
    ObjectTable *objectTable = partition->findObjectTable(definitionType);
    quint32 stateNumber = objectTable->stateNumber();
    if (stateNumber == targetStateNumber)
        return stateNumber;
    QSet<QString> limitTypes;
    limitTypes << definitionType;
    QsonMap changes = objectTable->changesSince(targetStateNumber, limitTypes).subObject("result");
    quint32 count = changes.valueInt("count", 0);
    QsonList changeList = changes.subList("changes");
    for (quint32 i = 0; i < count; i++) {
        QsonMap change = changeList.objectAt(i).toMap();
        if (change.contains("before")) {
            QsonMap before = change.subObject("before").toMap();
            if ((before.valueString(JsonDbString::kTypeStr) == definitionType)
                && (before.valueString("targetType") == viewType))
                removedDefinitions.insert(before.valueString(JsonDbString::kUuidStr), before);
        }
        if (change.contains("after")) {
            QsonMap after = change.subObject("after").toMap();
            if ((after.valueString(JsonDbString::kTypeStr) == definitionType)
                && (after.valueString("targetType") == viewType))
                addedDefinitions.insert(after.valueString(JsonDbString::kUuidStr), after);
        }
    }
    return stateNumber;
}

void JsonDb::updateReduce(const QString &viewType, const QString &partitionName)
{
    //Q_ASSERT(mReduceDefinitionsByTarget.contains(viewType));
    JsonDbBtreeStorage *partition = findPartition(partitionName);
    ObjectTable *targetTable = partition->findObjectTable(viewType);
    quint32 targetStateNumber = qMax(1u, targetTable->stateNumber());
    if (0) qDebug() << "JsonDb::updateReduce" << viewType << targetStateNumber << mViewsUpdating.contains(viewType);
    if (mViewsUpdating.contains(viewType))
        return;
    mViewsUpdating.insert(viewType);
    QHash<QString,JsonDbReduceDefinition*> reduceDefinitions;
    QSet<ObjectTable*>                  sourceTables;
    QMultiMap<ObjectTable*,QString>     objectTableSourceType;
    QMap<QString, QsonMap> addedReduceDefinitions;   // uuid -> added definition
    QMap<QString, QsonMap> removedReduceDefinitions; // uuid -> removed definition
    quint32 endStateNumber =
        findUpdatedMapReduceDefinitions(partition, JsonDbString::kReduceTypeStr,
                                        viewType, targetStateNumber, addedReduceDefinitions, removedReduceDefinitions);
    for (QMap<QString,JsonDbReduceDefinition*>::const_iterator it = mReduceDefinitionsByTarget.find(viewType);
         (it != mReduceDefinitionsByTarget.end() && it.key() == viewType);
         ++it) {
        JsonDbReduceDefinition *def = it.value();
        if (addedReduceDefinitions.contains(def->uuid()) || removedReduceDefinitions.contains(def->uuid()))
            continue;
        const QString &sourceType = def->sourceType();
        ObjectTable *sourceTable = partition->findObjectTable(sourceType);

        sourceTables.insert(sourceTable);
        objectTableSourceType.insert(sourceTable, sourceType);
        reduceDefinitions.insert(sourceType, def);
    }
    if (sourceTables.isEmpty() && addedReduceDefinitions.isEmpty() && removedReduceDefinitions.isEmpty()) {
        mViewsUpdating.remove(viewType);
        return;
    }

    for (QMap<ObjectTable*,QString>::const_iterator it = objectTableSourceType.begin();
         it != objectTableSourceType.end();
         ++it) {
        QString sourceType = it.value();
        // make sure the source is updated
        updateView(sourceType);

        if (!endStateNumber) {
            ObjectTable *sourceTable = it.key();
            quint32 sourceStateNumber = sourceTable->stateNumber();
            endStateNumber = sourceStateNumber;
        }
    }

    // transaction private to this objecttable
    targetTable->begin();

    for (QMap<QString,QsonMap>::const_iterator it = removedReduceDefinitions.begin();
         it != removedReduceDefinitions.end();
         ++it) {
        QsonMap def = it.value();
        removeReduceDefinition(def);
    }
    for (QMap<QString,QsonMap>::const_iterator it = addedReduceDefinitions.begin();
         it != addedReduceDefinitions.end();
         ++it) {
        QsonMap def = it.value();
        createReduceDefinition(def, true, partitionName);
    }

    for (QSet<ObjectTable*>::const_iterator it = sourceTables.begin();
         it != sourceTables.end();
         ++it) {
        ObjectTable *sourceTable = *it;
        QStringList sourceTypes = objectTableSourceType.values(sourceTable);
        QsonMap changes = sourceTable->changesSince(targetStateNumber).subObject("result");
        quint32 count = changes.valueInt("count", 0);
        QsonList changeList = changes.subList("changes");
        for (quint32 i = 0; i < count; i++) {
            QsonMap change = changeList.objectAt(i).toMap();
            QsonMap before = change.subObject("before").toMap();
            QsonMap after = change.subObject("after").toMap();
            QString sourceType = before.valueString(JsonDbString::kTypeStr);
            if (sourceTypes.contains(sourceType))
              reduceDefinitions.value(sourceType)->updateObject(before, after);
        }
    }

    targetTable->commit(endStateNumber);
    mViewsUpdating.remove(viewType);
}

JsonDbMapDefinition::JsonDbMapDefinition(JsonDb *jsonDb, JsonDbOwner *owner, const QString &partition, QsonMap definition, QObject *parent)
    : QObject(parent)
    , mJsonDb(jsonDb)
    , mPartition(partition)
    , mOwner(owner)
    , mDefinition(definition)
    , mScriptEngine(new QJSEngine(this))
    , mUuid(definition.valueString(JsonDbString::kUuidStr))
    , mTargetType(definition.valueString("targetType"))
    , mSourceType(definition.valueString("sourceType"))
    , mTargetTable(jsonDb->findPartition(partition)->findObjectTable(mTargetType))
    , mSourceTable(jsonDb->findPartition(partition)->findObjectTable(mSourceType))
{

    mJsonDbProxy = new JsonDbMapProxy(mOwner, mJsonDb, this);
    connect(mJsonDbProxy, SIGNAL(lookupRequested(QString,QJSValue,QJSValue,QJSValue)),
            this, SLOT(lookupRequested(QString,QJSValue,QJSValue,QJSValue)));
    connect(mJsonDbProxy, SIGNAL(viewObjectEmitted(QString,QJSValue)),
            this, SLOT(viewObjectEmitted(QString,QJSValue)));

    QJSValue globalObject = mScriptEngine->globalObject();
    globalObject.setProperty("jsondb", mScriptEngine->newQObject(mJsonDbProxy));
    globalObject.setProperty("console", mScriptEngine->newQObject(new Console()));

    QString script = mDefinition.valueString("map");
    Q_ASSERT(!script.isEmpty());

    mMapFunction = mScriptEngine->evaluate(QString("var %1 = %2; %1;").arg("map").arg(script));
    if (mMapFunction.isError() || !mMapFunction.isFunction())
        setError( "Unable to parse map function: " + mMapFunction.toString());
}

void JsonDbMapDefinition::mapObject(QsonMap object)
{
    QJSValue globalObject = mScriptEngine->globalObject();

    QJSValue sv = qsonToJSValue(object, mScriptEngine);
    QString uuid = object.valueString(JsonDbString::kUuidStr);
    mSourceUuids.clear();
    mSourceUuids.append(uuid);
    QJSValue mapped;
    mJsonDbProxy->clear();
    Q_ASSERT(mMapFunction.isFunction());

    QJSValueList mapArgs;
    mapArgs << sv;
    mapped = mMapFunction.call(globalObject, mapArgs);

    if (mapped.isError())
        setError("Error executing map function: " + mapped.toString());
}

void JsonDbMapDefinition::unmapObject(const QsonMap &object)
{
    QString uuid = object.valueString(JsonDbString::kUuidStr);
    QsonMap getObjectResponse = mTargetTable->getObject("sourceUuids.*", uuid, mTargetType);
    QsonList dependentObjects = getObjectResponse.subList("result");

    for (int i = 0; i < dependentObjects.size(); i++) {
        QsonMap dependentObject = dependentObjects.objectAt(i).toMap();
        if (dependentObject.valueString(JsonDbString::kTypeStr) != mTargetType)
            continue;
        mJsonDb->removeViewObject(mOwner, dependentObject);
        QStringList sourceUuids = dependentObject.value<QsonList>("sourceUuids").toStringList();
        int pos = sourceUuids.indexOf(uuid);
        if (pos > 0) {
            // TODO: should queue this for later
            QString remapUuid = sourceUuids[0];

            QsonMap getObjectResponse = mJsonDb->getObject(JsonDbString::kUuidStr, remapUuid);
            if (getObjectResponse.valueInt("count", 0) == 1) {
                QsonList objectsToUpdateList = getObjectResponse.subList("result");
                mapObject(objectsToUpdateList.objectAt(0));
            }
        }
    }
}

void JsonDbMapDefinition::lookupRequested(const QString &findKey, const QJSValue &findValue, const QJSValue &objectType, const QJSValue &context)
{
    QsonMap getObjectResponse =
        mSourceTable->getObject(findKey, findValue.toVariant(), objectType.toString());
    QsonList objectList = getObjectResponse.subList("result");
    for (int i = 0; i < objectList.size(); ++i) {
        QsonMap object = objectList.at<QsonMap>(i);
        const QString uuid = object.valueString(JsonDbString::kUuidStr);
        mSourceUuids.append(uuid);
        QJSValueList mapArgs;
        QJSValue sv = qsonToJSValue(object, mScriptEngine);

        mapArgs << sv << context;
        QJSValue globalObject = mScriptEngine->globalObject();
        QJSValue mapped = mMapFunction.call(globalObject, mapArgs);

        if (mapped.isError())
            setError("Error executing map function during lookup: " + mapped.toString());

        mSourceUuids.removeLast();
    }
    if (objectType.isUndefined()) {
        JsonDbBtreeStorage *storage = mJsonDb->findPartition(kDefaultPartitionName);
        for (QHash<QString, QPointer<ObjectTable> >::const_iterator it = storage->mViews.begin();
             it != storage->mViews.end();
             ++it) {
            ObjectTable *objectTable = it.value();
            QsonMap getObjectResponse = objectTable->getObject(findKey, findValue.toVariant(), objectType.toString());
            QsonList objectList = getObjectResponse.subList("result");
            for (int i = 0; i < objectList.size(); ++i) {
                QsonMap object = objectList.at<QsonMap>(i);
                const QString uuid = object.valueString(JsonDbString::kUuidStr);
                mSourceUuids.append(uuid);
                QJSValueList mapArgs;
                QJSValue sv = qsonToJSValue(object, mScriptEngine);

                mapArgs << sv << context;
                QJSValue globalObject = mScriptEngine->globalObject();
                QJSValue mapped = mMapFunction.call(globalObject, mapArgs);

                if (mapped.isError())
                    setError("Error executing map function during lookup: " + mapped.toString());

                mSourceUuids.removeLast();
            }
        }
    }
}


void JsonDbMapDefinition::viewObjectEmitted(const QString &key, const QJSValue &value)
{
    if (key.isEmpty()) {
        setError("Empty key provided to emitViewObject");
        return;
    }

    QsonMap newItem;
    newItem.insert(JsonDbString::kTypeStr, mTargetType);
    newItem.insert("key", key);
    newItem.insert("value", variantToQson(value.toVariant()));
    QsonList sourceUuids;
    foreach (const QString &str, mSourceUuids)
        sourceUuids.append(str);
    newItem.insert("sourceUuids", sourceUuids);
    newItem.insert("mapUuid", mUuid);

    QsonMap res = mJsonDb->createViewObject(mOwner, newItem);
    if (JsonDb::responseIsError(res))
        setError("Error executing map function during emitViewObject: " +
                 res.subObject(JsonDbString::kErrorStr).valueString(JsonDbString::kMessageStr));
}

bool JsonDbMapDefinition::isActive() const
{
    return mDefinition.isNull(JsonDbString::kActiveStr) || mDefinition.valueBool(JsonDbString::kActiveStr);
}

void JsonDbMapDefinition::setError(const QString &errorMsg)
{
    mDefinition.insert(JsonDbString::kActiveStr, false);
    mDefinition.insert(JsonDbString::kErrorStr, errorMsg);
    if (JsonDbBtreeStorage *storage = mJsonDb->findPartition(mPartition)) {
        WithTransaction transaction(storage, "JsonDbMapDefinition::setError");
        ObjectTable *objectTable = storage->findObjectTable(JsonDbString::kMapTypeStr);
        transaction.addObjectTable(objectTable);
        storage->updatePersistentObject(mDefinition);
        transaction.commit();
    }
}

JsonDbReduceDefinition::JsonDbReduceDefinition(JsonDb *jsonDb, JsonDbOwner *owner, const QString &partition,
                                               QsonMap definition, QObject *parent)
    : QObject(parent)
    , mJsonDb(jsonDb)
    , mOwner(owner)
    , mPartition(partition)
    , mDefinition(definition)
    , mScriptEngine(new QJSEngine(this))
    , mUuid(mDefinition.valueString(JsonDbString::kUuidStr))
    , mTargetType(mDefinition.valueString("targetType"))
    , mSourceType(mDefinition.valueString("sourceType"))
    , mSourceKeyName(mDefinition.contains("sourceKeyName") ? mDefinition.valueString("sourceKeyName") : QString("key"))
{
    Q_ASSERT(!mDefinition.valueString("add").isEmpty());
    Q_ASSERT(!mDefinition.valueString("subtract").isEmpty());

    QJSValue globalObject = mScriptEngine->globalObject();
    globalObject.setProperty("console", mScriptEngine->newQObject(new Console()));

    QString script = mDefinition.valueString("add");
    mAddFunction = mScriptEngine->evaluate(QString("var %1 = %2; %1;").arg("add").arg(script));

    if (mAddFunction.isError() || !mAddFunction.isFunction()) {
        setError("Unable to parse add function: " + mAddFunction.toString());
        return;
    }

    script = mDefinition.valueString("subtract");
    mSubtractFunction = mScriptEngine->evaluate(QString("var %1 = %2; %1;").arg("subtract").arg(script));

    if (mSubtractFunction.isError() || !mSubtractFunction.isFunction())
        setError("Unable to parse subtract function: " + mSubtractFunction.toString());
}

void JsonDbReduceDefinition::updateObject(QsonMap before, QsonMap after)
{
    Q_ASSERT(mAddFunction.isFunction());

    QString beforeKeyValue = mSourceKeyName.contains(".") ? JsonDb::propertyLookup(before, mSourceKeyName).toString()
        : before.valueString(mSourceKeyName);
    QString afterKeyValue = mSourceKeyName.contains(".") ? JsonDb::propertyLookup(after, mSourceKeyName).toString()
        : after.valueString(mSourceKeyName);

    if (!after.isEmpty() && (beforeKeyValue != afterKeyValue)) {
        // do a subtract only on the before key
        //qDebug() << "beforeKeyValue" << beforeKeyValue << "afterKeyValue" << afterKeyValue;
        //qDebug() << "before" << before << endl << "after" << after << endl;
        if (!beforeKeyValue.isEmpty())
            updateObject(before, QsonMap());

        // and then continue here with the add with the after key
        before = QsonMap();
    }

    const QString keyValue(after.isEmpty() ? beforeKeyValue : afterKeyValue);
    if (keyValue.isEmpty())
        return;

    QsonMap getObjectResponse = mJsonDb->getObject("key", keyValue, mTargetType);
    QsonMap previousResult;
    QsonObject previousValue;

    QsonList previousResults = getObjectResponse.subList("result");
    for (int k = 0; k < previousResults.size(); ++k) {
        QsonMap previous = previousResults.at<QsonMap>(k);
        if (previous.valueString("reduceUuid") == mUuid) {
            previousResult = previous;

            if (!previousResult.subObject("value").isEmpty())
                previousValue = previousResult.subObject("value");
            break;
        }
    }

    QsonObject value = previousValue;
    if (!before.isEmpty())
        value = subtractObject(keyValue, value, before);
    if (!after.isEmpty())
        value = addObject(keyValue, value, after);

    QsonMap res;
    if (!previousResult.valueString(JsonDbString::kUuidStr).isEmpty()) {
        if (value.isEmpty()) {
            res = mJsonDb->removeViewObject(mOwner, previousResult);
        } else {
            previousResult.insert("value", value);
            res = mJsonDb->updateViewObject(mOwner, previousResult);
        }
    } else {
        previousResult.insert(JsonDbString::kTypeStr, mTargetType);
        previousResult.insert("key", keyValue);
        previousResult.insert("value", value);
        previousResult.insert("reduceUuid", mUuid);
        res = mJsonDb->createViewObject(mOwner, previousResult);
    }

    if (JsonDb::responseIsError(res))
        setError("Error executing add function: " +
                 res.subObject(JsonDbString::kErrorStr).valueString(JsonDbString::kMessageStr));
}

QsonObject JsonDbReduceDefinition::addObject(const QString &keyValue, const QsonObject &previousValue, QsonMap object)
{
    QJSValue globalObject = mScriptEngine->globalObject();
    QJSValue svKeyValue = mScriptEngine->toScriptValue(keyValue);
    QJSValue svPreviousValue = mScriptEngine->toScriptValue(qsonToVariant(previousValue));
    QJSValue svObject = qsonToJSValue(object, mScriptEngine);

    QJSValueList reduceArgs;
    reduceArgs << svKeyValue << svPreviousValue << svObject;
    QJSValue reduced = mAddFunction.call(globalObject, reduceArgs);

    if (reduced.isObject() && !reduced.isError()) {
        QVariantMap vReduced = mScriptEngine->fromScriptValue<QVariantMap>(reduced);
        return variantToQson(vReduced);
    } else {

        if (reduced.isError())
            setError("Error executing add function: " + reduced.toString());

        return QsonObject();
    }
}

QsonObject JsonDbReduceDefinition::subtractObject(const QString &keyValue, const QsonObject &previousValue, QsonMap object)
{
    Q_ASSERT(mSubtractFunction.isFunction());

    QJSValue globalObject = mScriptEngine->globalObject();

    QJSValue svKeyValue = mScriptEngine->toScriptValue(keyValue);
    QJSValue svPreviousValue = mScriptEngine->toScriptValue(qsonToVariant(previousValue));
    QJSValue sv = qsonToJSValue(object, mScriptEngine);

    QJSValueList reduceArgs;
    reduceArgs << svKeyValue << svPreviousValue << sv;
    QJSValue reduced = mSubtractFunction.call(globalObject, reduceArgs);

    if (reduced.isObject() && !reduced.isError()) {
        QVariantMap vReduced = mScriptEngine->fromScriptValue<QVariantMap>(reduced);
        return variantToQson(vReduced);
    } else {
        if (reduced.isError())
            setError("Error executing subtract function: " + reduced.toString());
        return QsonObject();
    }
}

bool JsonDbReduceDefinition::isActive() const
{
    return mDefinition.isNull(JsonDbString::kActiveStr) || mDefinition.valueBool(JsonDbString::kActiveStr);
}

void JsonDbReduceDefinition::setError(const QString &errorMsg)
{
    mDefinition.insert(JsonDbString::kActiveStr, false);
    mDefinition.insert(JsonDbString::kErrorStr, errorMsg);
    if (JsonDbBtreeStorage *storage = mJsonDb->findPartition(mPartition)) {
        WithTransaction transaction(storage, "JsonDbReduceDefinition::setError");
        ObjectTable *objectTable = storage->findObjectTable(JsonDbString::kMapTypeStr);
        transaction.addObjectTable(objectTable);
        storage->updatePersistentObject(mDefinition);
        transaction.commit();
    }
}

} } // end namespace QtAddOn::JsonDb