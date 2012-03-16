/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
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
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QObject>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegExp>
#include <QString>
#include <QElapsedTimer>
#include <QUuid>
#include <QtAlgorithms>
#include <QtEndian>
#include <QStringBuilder>
#include <QTimerEvent>
#include <QMap>

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include "json.h"
#include "jsondb-strings.h"
#include "jsondb-error.h"
#include "jsondbpartition.h"
#include "jsondbindex.h"
#include "jsondbobjecttable.h"
#include "qbtreetxn.h"
#include "jsondbmanagedbtree.h"
#include "jsondbsettings.h"
#include "jsondbview.h"
#include "jsondbschemamanager_impl_p.h"
#include "jsondbobjecttypes_impl_p.h"

QT_BEGIN_NAMESPACE_JSONDB

const QString gDatabaseSchemaVersion = "0.2";

JsonDbPartition::JsonDbPartition(const QString &filename, const QString &name, JsonDbOwner *owner, QObject *parent)
    : QObject(parent)
    , mObjectTable(0)
    , mPartitionName(name)
    , mFilename(filename)
    , mTransactionDepth(0)
    , mWildCardPrefixRegExp("([^*?\\[\\]\\\\]+).*")
    , mMainSyncTimerId(-1)
    , mIndexSyncTimerId(-1)
    , mDefaultOwner(owner)
{
    if (!mFilename.endsWith(QLatin1String(".db")))
        mFilename += QLatin1String(".db");

    mMainSyncInterval = jsondbSettings->syncInterval();
    if (mMainSyncInterval < 1000)
        mMainSyncInterval = 5000;
    mIndexSyncInterval = jsondbSettings->indexSyncInterval();
    if (mIndexSyncInterval < 1000)
        mIndexSyncInterval = 12000;
}

JsonDbPartition::~JsonDbPartition()
{
    if (mTransactionDepth) {
        qCritical() << "JsonDbBtreePartition::~JsonDbBtreePartition"
                    << "closing while transaction open" << "mTransactionDepth" << mTransactionDepth;
    }
    close();
}

bool JsonDbPartition::close()
{
    foreach (JsonDbView *view, mViews.values()) {
        view->close();
        delete view;
    }
    mViews.clear();

    delete mObjectTable;
    mObjectTable = 0;

    return true;
}

bool JsonDbPartition::open()
{
    if (jsondbSettings->debug())
        qDebug() << "JsonDbBtree::open" << mPartitionName << mFilename;

    mObjectTable = new JsonDbObjectTable(this);
    mObjectTable->open(mFilename, QBtree::NoSync | QBtree::UseSyncMarker);

    if (!checkStateConsistency()) {
        qCritical() << "JsonDbBtreePartition::open()" << "Unable to recover database";
        return false;
    }

    bool rebuildingDatabaseMetadata = false;

    QString partitionId;
    GetObjectsResult getObjectsResult = mObjectTable->getObjects(JsonDbString::kTypeStr, QJsonValue(JsonDbString::kDbidTypeStr),
                                                                 JsonDbString::kDbidTypeStr);
    if (getObjectsResult.data.size()) {
        QJsonObject object = getObjectsResult.data.at(0);
        partitionId = object.value("id").toString();
        QString partitionName = object.value("name").toString();
        if (partitionName != mPartitionName || !object.contains(JsonDbString::kDatabaseSchemaVersionStr)
            || object.value(JsonDbString::kDatabaseSchemaVersionStr).toString() != gDatabaseSchemaVersion) {
            if (jsondbSettings->verbose())
                qDebug() << "Rebuilding database metadata";
            rebuildingDatabaseMetadata = true;
        }
    }

    if (partitionId.isEmpty() || rebuildingDatabaseMetadata) {
        if (partitionId.isEmpty())
            partitionId = QUuid::createUuid().toString();

        QByteArray baKey(4, 0);
        qToBigEndian(0, (uchar *)baKey.data());

        JsonDbObject object;
        object.insert(JsonDbString::kTypeStr, JsonDbString::kDbidTypeStr);
        object.insert(QLatin1String("id"), partitionId);
        object.insert(QLatin1String("name"), mPartitionName);
        object.insert(JsonDbString::kDatabaseSchemaVersionStr, gDatabaseSchemaVersion);
        JsonDbWriteResult result = updateObject(mDefaultOwner, object, ForcedWrite);
        Q_ASSERT(result.code == JsonDbError::NoError);
    }
    if (jsondbSettings->verbose())
        qDebug() << "partition" << mPartitionName << "id" << partitionId;

    initSchemas();
    initIndexes();
    JsonDbView::initViews(this);

    return true;
}

bool JsonDbPartition::clear()
{
    if (mObjectTable->bdb()) {
        qCritical() << "Cannot clear database while it is open.";
        return false;
    }
    QStringList filters;
    QFileInfo fi(mFilename);
    filters << QString::fromLatin1("%1*.db").arg(fi.baseName());
    QDir dir(fi.absolutePath());
    QStringList lst = dir.entryList(filters);
    foreach (const QString &fileName, lst) {
        if (jsondbSettings->verbose())
            qDebug() << "removing" << fileName;
        if (!dir.remove(fileName)) {
            qCritical() << "Failed to remove" << fileName;
            return false;
        }
    }
    return true;
}

inline quint16 fieldValueSize(QJsonValue::Type vt, const QJsonValue &fieldValue)
{
    switch (vt) {
    case QJsonValue::Undefined:
    case QJsonValue::Null:
    case QJsonValue::Array:
    case QJsonValue::Object:
        return 0;
    case QJsonValue::Bool:
        return 4;
    case QJsonValue::Double:
        return 8;
    case QJsonValue::String:
        return 2*fieldValue.toString().count();
    }
    return 0;
}

void memcpyFieldValue(char *data, QJsonValue::Type vt, const QJsonValue &fieldValue)
{
    switch (vt) {
    case QJsonValue::Undefined:
    case QJsonValue::Null:
    case QJsonValue::Array:
    case QJsonValue::Object:
        break;
    case QJsonValue::Bool: {
        quint32 value = fieldValue.toBool() ? 1 : 0;
        qToBigEndian(value, (uchar *)data);
    } break;
    case QJsonValue::Double: {
        union {
            double d;
            quint64 ui;
        };
        d = fieldValue.toDouble();
        qToBigEndian<quint64>(ui, (uchar *)data);
    } break;
    case QJsonValue::String: {
        QString str = fieldValue.toString();
        memcpy(data, (const char *)str.constData(), 2*str.count());
    }
    }
}

void memcpyFieldValue(QJsonValue::Type vt, QJsonValue &fieldValue, const char *data, quint16 size)
{
    switch (vt) {
    case QJsonValue::Undefined:
    case QJsonValue::Array:
    case QJsonValue::Object:
        break;
    case QJsonValue::Null:
        fieldValue = QJsonValue();
        break;
    case QJsonValue::Bool: {
        fieldValue = qFromBigEndian<qint32>((const uchar *)data) == 1 ? true : false;
    } break;
    case QJsonValue::Double: {
        union {
            double d;
            quint64 ui;
        };
        ui = qFromBigEndian<quint64>((const uchar *)data);
        fieldValue = d;
    } break;
    case QJsonValue::String: {
        fieldValue = QString((const QChar *)data, size/2);
    }
    }
}

int objectKeyCmp(const char *aptr, size_t alen, const char *bptr, size_t blen, void *)
{
    Q_UNUSED(alen);
    Q_UNUSED(blen);
    quint32 a = qFromBigEndian<quint32>((const uchar *)aptr);
    quint32 b = qFromBigEndian<quint32>((const uchar *)bptr);
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

int intcmp(const uchar *aptr, const uchar *bptr)
{
    qint32 a = qFromBigEndian<qint32>((const uchar *)aptr);
    qint32 b = qFromBigEndian<qint32>((const uchar *)bptr);
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}
int doublecmp(const uchar *aptr, const uchar *bptr)
{
    union {
        double d;
        quint64 ui;
    } a, b;
    a.ui = qFromBigEndian<quint64>((const uchar *)aptr);
    b.ui = qFromBigEndian<quint64>((const uchar *)bptr);
    if (a.d < b.d)
        return -1;
    if (a.d > b.d)
        return 1;
    return 0;
}
int qstringcmp(const quint16 *achar, quint32 acount, const quint16 *bchar, quint32 bcount)
{
    int rv = 0;
    quint32 minCount = qMin(acount, bcount);
    for (quint32 i = 0; i < minCount; i++) {
        if ((rv = (achar[i] - bchar[i])) != 0)
            return rv;
    }
    return acount-bcount;
}

QJsonValue makeFieldValue(const QJsonValue &value, const QString &type)
{
    if (type.isEmpty() || type == QLatin1String("string")) {
        switch (value.type()) {
        case QJsonValue::Null: return QLatin1String("null");
        case QJsonValue::Bool: return QLatin1String(value.toBool() ? "true" : "false");
        case QJsonValue::Double: return QString::number(value.toDouble());
        case QJsonValue::String: return value.toString();
        case QJsonValue::Array: {
            QJsonArray array = value.toArray();
            if (array.size() == 1)
                return makeFieldValue(array.at(0), type);
            return QJsonValue(QJsonValue::Undefined);
        }
        case QJsonValue::Object: break;
        case QJsonValue::Undefined: break;
        }
    } else if ((type == QLatin1String("number"))
               || (type == QLatin1String("integer"))) {
        switch (value.type()) {
        case QJsonValue::Null: return 0;
        case QJsonValue::Bool: return value.toBool() ? 1 : 0;
        case QJsonValue::Double: return value.toDouble();
        case QJsonValue::String: {
            QString str = value.toString();
            bool ok = false;
            double dval = str.toDouble(&ok);
            if (ok)
                return dval;
            int ival = str.toInt(&ok);
            if (ok)
                return ival;
            break;
        }
        case QJsonValue::Array: {
            QJsonArray array = value.toArray();
            if (array.size() == 1)
                return makeFieldValue(array.at(0), type);
            return QJsonValue(QJsonValue::Undefined);
        }
        case QJsonValue::Object: break;
        case QJsonValue::Undefined: break;
        }
    } else {
        qWarning() << "qtjsondb: makeFieldValue: unsupported index type" << type;
    }
    return QJsonValue(QJsonValue::Undefined);
}

static const int VtLastKind = QJsonValue::Undefined;
QByteArray makeForwardKey(const QJsonValue &fieldValue, const ObjectKey &objectKey)
{
    QJsonValue::Type vt = fieldValue.type();
    Q_ASSERT(vt <= VtLastKind);
    quint32 size = fieldValueSize(vt, fieldValue);

    QByteArray forwardKey(4+size+16, 0);
    char *data = forwardKey.data();
    qToBigEndian<quint32>(vt, (uchar *)&data[0]);
    memcpyFieldValue(data+4, vt, fieldValue);
    qToBigEndian(objectKey, (uchar *)&data[4+size]);

    return forwardKey;
}

void forwardKeySplit(const QByteArray &forwardKey, QJsonValue &fieldValue)
{
    const char *data = forwardKey.constData();
    QJsonValue::Type vt = (QJsonValue::Type)qFromBigEndian<quint32>((const uchar *)&data[0]);
    Q_ASSERT(vt <= VtLastKind);
    quint32 fvSize = forwardKey.size()-4-16;
    memcpyFieldValue(vt, fieldValue, data+4, fvSize);
}
void forwardKeySplit(const QByteArray &forwardKey, QJsonValue &fieldValue, ObjectKey &objectKey)
{
    const char *data = forwardKey.constData();
    QJsonValue::Type vt = (QJsonValue::Type)qFromBigEndian<quint32>((const uchar *)&data[0]);
    Q_ASSERT(vt <= VtLastKind);
    quint32 fvSize = forwardKey.size()-4-16;
    memcpyFieldValue(vt, fieldValue, data+4, fvSize);
    objectKey = qFromBigEndian<ObjectKey>((const uchar *)&data[4+fvSize]);
}
int forwardKeyCmp(const char *aptr, size_t asiz, const char *bptr, size_t bsiz, void *)
{
    int rv = 0;
    QJsonValue::Type avt = (QJsonValue::Type)qFromBigEndian<quint32>((const uchar *)&aptr[0]);
    QJsonValue::Type bvt = (QJsonValue::Type)qFromBigEndian<quint32>((const uchar *)&bptr[0]);
    Q_ASSERT(avt <= VtLastKind);
    Q_ASSERT(bvt <= VtLastKind);
    quint32 asize = asiz - 4 - 16;
    quint32 bsize = bsiz - 4 - 16;
    if (avt != bvt)
        return avt - bvt;

    const char *aData = aptr + 4;
    const char *bData = bptr + 4;
    switch (avt) {
    case QJsonValue::Bool:
        rv = intcmp((const uchar *)aData, (const uchar *)bData);
        break;
    case QJsonValue::Double:
        rv = doublecmp((const uchar *)aData, (const uchar *)bData);
        break;
    case QJsonValue::String:
        rv = qstringcmp((const quint16 *)aData, asize/2, (const quint16 *)bData, bsize/2);
        break;
    case QJsonValue::Undefined:
    case QJsonValue::Null:
    case QJsonValue::Array:
    case QJsonValue::Object:
        rv = 0;
        break;
    }
    if (rv != 0)
        return rv;
    ObjectKey aObjectKey = qFromBigEndian<ObjectKey>((const uchar *)aptr+4+asize);
    ObjectKey bObjectKey = qFromBigEndian<ObjectKey>((const uchar *)bptr+4+bsize);
    if (aObjectKey == bObjectKey)
        return 0;
    return aObjectKey < bObjectKey ? -1 : 1;
}

QByteArray makeForwardValue(const ObjectKey &objectKey)
{
    QByteArray forwardValue(16, 0);
    char *data = forwardValue.data();
    qToBigEndian(objectKey,  (uchar *)&data[0]);
    return forwardValue;
}
void forwardValueSplit(const QByteArray &forwardValue, ObjectKey &objectKey)
{
    const uchar *data = (const uchar *)forwardValue.constData();
    objectKey = qFromBigEndian<ObjectKey>(&data[0]);
}

QJsonObject JsonDbPartition::createPersistentObject(JsonDbObject &object)
{
    QJsonObject resultmap, errormap;

    if (!object.contains(JsonDbString::kUuidStr)) {
        object.generateUuid();
        object.computeVersion();
    }

    QString uuid = object.value(JsonDbString::kUuidStr).toString();
    QString version = object.value(JsonDbString::kVersionStr).toString();
    QString objectType = object.value(JsonDbString::kTypeStr).toString();

    ObjectKey objectKey(object.value(JsonDbString::kUuidStr).toString());
    JsonDbObjectTable *table = findObjectTable(objectType);

    bool ok = table->put(objectKey, object);
    if (!ok)
        return makeErrorResponse(resultmap, JsonDbError::DatabaseError,
                                 table->errorMessage());

    quint32 stateNumber = table->storeStateChange(objectKey, ObjectChange::Created);
    table->indexObject(objectKey, object, table->stateNumber());

    resultmap.insert(JsonDbString::kUuidStr, uuid);
    resultmap.insert(JsonDbString::kVersionStr, version);
    resultmap.insert(JsonDbString::kCountStr, 1);
    resultmap.insert(JsonDbString::kStateNumberStr, (int)stateNumber);

    return makeResponse(resultmap, errormap);
}

JsonDbView *JsonDbPartition::addView(const QString &viewType)
{
    JsonDbView *view = mViews.value(viewType);
    if (view)
        return view;

    view = new JsonDbView(this, viewType, this);
    view->open();
    mViews.insert(viewType, view);
    return view;
}

void JsonDbPartition::removeView(const QString &viewType)
{
    JsonDbView *view = mViews.value(viewType);
    view->close();
    view->deleteLater();
    mViews.remove(viewType);
}

void JsonDbPartition::updateView(const QString &objectType)
{
    if (!mViews.contains(objectType))
        return;
    mViews[objectType]->updateView();
}

bool JsonDbPartition::checkCanAddSchema(const JsonDbObject &schema, const JsonDbObject &oldSchema, QString &errorMsg)
{
    if (!schema.contains("name") || !schema.contains("schema")) {
        errorMsg = QLatin1String("_schemaType objects must specify both name and schema properties");
        return false;
    }

    QString schemaName = schema.value("name").toString();

    if (schemaName.isEmpty()) {
        errorMsg = QLatin1String("name property of _schemaType object must be specified");
        return false;
    } else if (mSchemas.contains(schemaName) && oldSchema.value("name").toString() != schemaName) {
        errorMsg = QString("A schema with name %1 already exists").arg(schemaName);
        return false;
    }

    return true;
}

JsonDbView *JsonDbPartition::findView(const QString &objectType) const
{
    if (mViews.contains(objectType))
        return mViews.value(objectType);
    else
        return 0;
}

JsonDbObjectTable *JsonDbPartition::findObjectTable(const QString &objectType) const
{
        if (mViews.contains(objectType))
            return mViews.value(objectType)->objectTable();
        else
            return mObjectTable;
}

bool JsonDbPartition::beginTransaction()
{
    if (mTransactionDepth++ == 0) {
        Q_ASSERT(mTableTransactions.isEmpty());
    }
    return true;
}

bool JsonDbPartition::commitTransaction(quint32 stateNumber)
{
    if (--mTransactionDepth == 0) {
        bool ret = true;
        quint32 nextStateNumber = stateNumber ? stateNumber : (mObjectTable->stateNumber() + 1);

        if (jsondbSettings->debug())
            qDebug() << "commitTransaction" << stateNumber;

        if (!stateNumber && (mTableTransactions.size() == 1))
            nextStateNumber = mTableTransactions.at(0)->stateNumber() + 1;

        for (int i = 0; i < mTableTransactions.size(); i++) {
            JsonDbObjectTable *table = mTableTransactions.at(i);
            if (!table->commit(nextStateNumber)) {
                qCritical() << __FILE__ << __LINE__ << "Failed to commit transaction on object table";
                ret = false;
            }
        }
        mTableTransactions.clear();

        if (mMainSyncTimerId == -1)
            mMainSyncTimerId = startTimer(mMainSyncInterval, Qt::VeryCoarseTimer);
        if (mIndexSyncTimerId == -1)
            mIndexSyncTimerId = startTimer(mIndexSyncInterval, Qt::VeryCoarseTimer);

        return ret;
    }
    return true;
}

bool JsonDbPartition::abortTransaction()
{
    if (--mTransactionDepth == 0) {
        if (jsondbSettings->verbose())
            qDebug() << "JsonDbBtreePartition::abortTransaction()";
        bool ret = true;

        for (int i = 0; i < mTableTransactions.size(); i++) {
            JsonDbObjectTable *table = mTableTransactions.at(i);
            if (!table->abort()) {
                qCritical() << __FILE__ << __LINE__ << "Failed to abort transaction";
                ret = false;
            }
        }
        mTableTransactions.clear();
        return ret;
    }
    return true;
}

QJsonObject JsonDbPartition::flush()
{
    mObjectTable->sync(JsonDbObjectTable::SyncObjectTable);

    QJsonObject resultmap, errormap;
    resultmap.insert(JsonDbString::kStateNumberStr, (int)mObjectTable->stateNumber());
    return makeResponse(resultmap, errormap);
}


void JsonDbPartition::timerEvent(QTimerEvent *event)
{
    if (mTransactionDepth)
        return;

    if (event->timerId() == mMainSyncTimerId) {
        if (jsondbSettings->debug())
            qDebug() << "Syncing main object table";

        mObjectTable->sync(JsonDbObjectTable::SyncObjectTable);
        killTimer(mMainSyncTimerId);
        mMainSyncTimerId = -1;
    } else if (event->timerId() == mIndexSyncTimerId) {

        if (jsondbSettings->debug())
            qDebug() << "Syncing indexes and views";

        // sync the main object table's indexes
        mObjectTable->sync(JsonDbObjectTable::SyncIndexes);

        // sync the views
        foreach (JsonDbView *view, mViews)
            view->objectTable()->sync(JsonDbObjectTable::SyncObjectTable | JsonDbObjectTable::SyncIndexes);

        killTimer(mIndexSyncTimerId);
        mIndexSyncTimerId = -1;
    }
}

QJsonObject JsonDbPartition::updatePersistentObject(const JsonDbObject &oldObject, const JsonDbObject &object)
{
    QJsonObject resultmap, errormap;

    QString uuid = object.value(JsonDbString::kUuidStr).toString();
    QString version = object.value(JsonDbString::kVersionStr).toString();
    QString objectType = object.value(JsonDbString::kTypeStr).toString();
    JsonDbObjectTable *table = findObjectTable(objectType);
    ObjectKey objectKey(uuid);

    objectKey = ObjectKey(object.value(JsonDbString::kUuidStr).toString());

    if (!oldObject.isEmpty())
        table->deindexObject(objectKey, oldObject, table->stateNumber());

    if (!table->put(objectKey, object))
        return makeErrorResponse(resultmap, JsonDbError::DatabaseError,
                                 table->errorMessage());

    if (jsondbSettings->debug())
        qDebug() << "updateObject" << objectKey << endl << object << endl << oldObject;

    quint32 stateNumber;
    if (!oldObject.isEmpty()) {
        stateNumber = table->storeStateChange(objectKey, ObjectChange::Updated, oldObject);
    } else {
        stateNumber = table->storeStateChange(objectKey, ObjectChange::Created);
    }

    table->indexObject(objectKey, object, table->stateNumber());

    resultmap.insert( JsonDbString::kCountStr, 1 );
    resultmap.insert( JsonDbString::kUuidStr, uuid );
    resultmap.insert( JsonDbString::kVersionStr, version );
    resultmap.insert( JsonDbString::kStateNumberStr, (int)stateNumber );

    return makeResponse(resultmap, errormap);
}

QJsonObject JsonDbPartition::removePersistentObject(const JsonDbObject &oldObject, const JsonDbObject &ts)
{
    if (jsondbSettings->debug())
        qDebug() << "removePersistentObject" << endl << oldObject << endl << "    tombstone" << ts;

    QJsonObject resultmap, errormap;
    QString uuid = oldObject.value(JsonDbString::kUuidStr).toString();
    QString version = oldObject.value(JsonDbString::kVersionStr).toString();
    QString objectType = oldObject.value(JsonDbString::kTypeStr).toString();

    JsonDbObjectTable *table = findObjectTable(objectType);

    ObjectKey objectKey(uuid);

    Q_ASSERT(ts.contains(JsonDbString::kUuidStr) && (oldObject.value(JsonDbString::kUuidStr) == ts.value(JsonDbString::kUuidStr)));
    Q_ASSERT(ts.contains(JsonDbString::kVersionStr));
    Q_ASSERT(ts.contains(JsonDbString::kDeletedStr) && ts.value(JsonDbString::kDeletedStr).toBool());

    bool ok = table->put(objectKey, ts);
    Q_ASSERT(ok);
    if (!ok) {
        return makeErrorResponse(resultmap,
                                 JsonDbError::DatabaseError,
                                 table->errorMessage());
    }
    JsonDbObject testing;
    ok = table->get(objectKey, &testing, true);
    Q_ASSERT(ok);
    Q_ASSERT(testing == ts);

    quint32 stateNumber = table->storeStateChange(objectKey, ObjectChange::Deleted, oldObject);
    table->deindexObject(objectKey, oldObject, table->stateNumber());

    QJsonObject item;
    item.insert(JsonDbString::kUuidStr, uuid);
    item.insert(JsonDbString::kVersionStr, version);

    QJsonArray data;
    data.append(item);
    resultmap.insert(JsonDbString::kCountStr, 1);
    resultmap.insert(JsonDbString::kDataStr, data);
    resultmap.insert(JsonDbString::kErrorStr, QJsonValue());
    resultmap.insert(JsonDbString::kStateNumberStr, (int)stateNumber);
    QJsonObject result = makeResponse(resultmap, errormap);
    return result;
}

bool JsonDbPartition::getObject(const QString &uuid, JsonDbObject &object, const QString &objectType) const
{
    ObjectKey objectKey(uuid);
    return getObject(objectKey, object, objectType);
}

bool JsonDbPartition::getObject(const ObjectKey &objectKey, JsonDbObject &object, const QString &objectType) const
{
    JsonDbObjectTable *table = findObjectTable(objectType);

    bool ok = table->get(objectKey, &object);
    if (ok)
        return ok;
    QHash<QString,QPointer<JsonDbView> >::const_iterator it = mViews.begin();
    for (; it != mViews.end(); ++it) {
        JsonDbView *view = it.value();
        if (!view)
            qDebug() << "no view";
        if (!view->objectTable())
            qDebug() << "no object table for view";
        bool ok = view->objectTable()->get(objectKey, &object);
        if (ok)
            return ok;
    }
    return false;
}

GetObjectsResult JsonDbPartition::getObjects(const QString &keyName, const QJsonValue &keyValue, const QString &_objectType, bool updateViews)
{
    const QString &objectType = (keyName == JsonDbString::kTypeStr) ? keyValue.toString() : _objectType;
    JsonDbObjectTable *table = findObjectTable(objectType);

    if (updateViews && (table != mObjectTable))
        updateView(objectType);
    return table->getObjects(keyName, keyValue, objectType);
}

QJsonObject JsonDbPartition::changesSince(quint32 stateNumber, const QSet<QString> &limitTypes)
{
    JsonDbObjectTable *objectTable = 0;
    if (!limitTypes.size())
        objectTable = mObjectTable;
    else
        foreach (const QString &limitType, limitTypes) {
            JsonDbObjectTable *ot = findObjectTable(limitType);
            qDebug() << "changesSince" << limitType << QString::number((long long)ot, 16);
            if (!objectTable)
                objectTable = ot;
            else if (ot == objectTable)
                continue;
            else
                return makeError(JsonDbError::InvalidRequest, "limit types must be from the same object table");
        }
    Q_ASSERT(objectTable);
    return objectTable->changesSince(stateNumber, limitTypes);
}

void JsonDbPartition::flushCaches()
{
    mObjectTable->flushCaches();
    for (QHash<QString,QPointer<JsonDbView> >::const_iterator it = mViews.begin();
         it != mViews.end();
         ++it)
        it.value()->reduceMemoryUsage();
}

void JsonDbPartition::initIndexes()
{
    QByteArray baPropertyName;
    QByteArray baIndexObject;

    mObjectTable->addIndexOnProperty(JsonDbString::kUuidStr, "string");
    mObjectTable->addIndexOnProperty(JsonDbString::kTypeStr, "string");

    GetObjectsResult getObjectsResult = mObjectTable->getObjects(JsonDbString::kTypeStr, JsonDbString::kIndexTypeStr, JsonDbString::kIndexTypeStr);
    foreach (const QJsonObject indexObject, getObjectsResult.data) {
        if (jsondbSettings->verbose())
            qDebug() << "initIndexes" << "index" << indexObject;
        QString indexObjectType = indexObject.value(JsonDbString::kTypeStr).toString();
        if (indexObjectType == JsonDbString::kIndexTypeStr) {
            QString indexName = indexObject.value(JsonDbString::kNameStr).toString();
            QString propertyName = indexObject.value(JsonDbString::kPropertyNameStr).toString();
            QString propertyType = indexObject.value(JsonDbString::kPropertyTypeStr).toString();
            QString objectType = indexObject.value(JsonDbString::kObjectTypeStr).toString();
            QString propertyFunction = indexObject.value(JsonDbString::kPropertyFunctionStr).toString();
            QString locale = indexObject.value(JsonDbString::kLocaleStr).toString();
            QString collation = indexObject.value(JsonDbString::kCollationStr).toString();

            JsonDbObjectTable *table = findObjectTable(objectType);
            table->addIndex(indexName, propertyName, propertyType, objectType, propertyFunction, locale, collation);
        }
    }
}

bool JsonDbPartition::addIndex(const QString &indexName, const QString &propertyName,
                                  const QString &propertyType, const QString &objectType, const QString &propertyFunction,
                                  const QString &locale, const QString &collation)
{
    Q_ASSERT(!indexName.isEmpty());
    //qDebug() << "JsonDbBtreePartition::addIndex" << propertyName << objectType;
    JsonDbObjectTable *table = findObjectTable(objectType);
    const IndexSpec *indexSpec = table->indexSpec(indexName);
    if (indexSpec)
        return true;
    //if (gVerbose) qDebug() << "JsonDbBtreePartition::addIndex" << propertyName << objectType;
    return table->addIndex(indexName, propertyName, propertyType, objectType, propertyFunction, locale, collation);
}

bool JsonDbPartition::removeIndex(const QString &indexName, const QString &objectType)
{
    JsonDbObjectTable *table = findObjectTable(objectType);
    const IndexSpec *indexSpec = table->indexSpec(indexName);
    if (!indexSpec)
        return false;
    return table->removeIndex(indexName);
}

bool JsonDbPartition::checkStateConsistency()
{
    return true;
}

void JsonDbPartition::checkIndexConsistency(JsonDbObjectTable *objectTable, JsonDbIndex *index)
{
    quint32 indexStateNumber = index->bdb()->tag();
    quint32 objectStateNumber = objectTable->stateNumber();
    if (indexStateNumber > objectTable->stateNumber()) {
        qCritical() << "reverting index" << index->propertyName() << indexStateNumber << objectStateNumber;
        while (indexStateNumber > objectTable->stateNumber()) {
            int rc = index->bdb()->rollback();
            quint32 newIndexStateNumber = index->bdb()->tag();
            if (newIndexStateNumber == indexStateNumber) {
                qDebug() << "failed to revert. clearing" << rc;
                index->bdb()->clearData();
                break;
            }
            qCritical() << "   reverted index to state" << indexStateNumber;
        }
    }
}

QHash<QString, qint64> JsonDbPartition::fileSizes() const
{
    QList<QFileInfo> fileInfo;
    fileInfo << mObjectTable->bdb()->fileName();

    foreach (const IndexSpec &spec, mObjectTable->indexSpecs().values()) {
        if (spec.index->bdb())
            fileInfo << spec.index->bdb()->fileName();
    }

    foreach (JsonDbView *view, mViews) {
        JsonDbObjectTable *objectTable = view->objectTable();
        fileInfo << objectTable->bdb()->fileName();
        foreach (const IndexSpec &spec, objectTable->indexSpecs().values()) {
            if (spec.index->bdb())
                fileInfo << spec.index->bdb()->fileName();
        }
    }

    QHash<QString, qint64> result;
    foreach (const QFileInfo &info, fileInfo)
        result.insert(info.fileName(), info.size());
    return result;
}

static bool sDebugQuery = jsondbSettings->debugQuery();

IndexQuery *IndexQuery::indexQuery(JsonDbPartition *partition, JsonDbObjectTable *table,
                                   const QString &propertyName, const QString &propertyType,
                                   const JsonDbOwner *owner, bool ascending)
{
    if (propertyName == JsonDbString::kUuidStr)
        return new UuidQuery(partition, table, propertyName, owner, ascending);
    else
        return new IndexQuery(partition, table, propertyName, propertyType, owner, ascending);
}

UuidQuery::UuidQuery(JsonDbPartition *partition, JsonDbObjectTable *table, const QString &propertyName, const JsonDbOwner *owner, bool ascending)
    : IndexQuery(partition, table, propertyName, QString(), owner, ascending)
{
}

IndexQuery::IndexQuery(JsonDbPartition *partition, JsonDbObjectTable *table,
                       const QString &propertyName, const QString &propertyType,
                       const JsonDbOwner *owner, bool ascending)
    : mPartition(partition)
    , mObjectTable(table)
    , mBdbIndex(0)
    , mCursor(0)
    , mOwner(owner)
    , mMin(QJsonValue::Undefined)
    , mMax(QJsonValue::Undefined)
    , mAscending(ascending)
    , mPropertyName(propertyName)
    , mPropertyType(propertyType)
    , mSparseMatchPossible(false)
    , mResidualQuery(0)
{
    if (propertyName != JsonDbString::kUuidStr) {
        mBdbIndex = table->indexSpec(propertyName)->index->bdb();
        mTxn = mBdbIndex->btree()->beginRead();
        mCursor = new QBtreeCursor(mTxn);
    } else {
        mTxn = table->bdb()->btree()->beginRead();
        mCursor = new QBtreeCursor(mTxn);
    }
}
IndexQuery::~IndexQuery()
{
    if (mResidualQuery) {
        delete mResidualQuery;
        mResidualQuery = 0;
    }
    if (mTxn && mCursor) {
        mTxn->abort();
        delete mCursor;
        mCursor = 0;
    }
    for (int i = 0; i < mQueryConstraints.size(); i++) {
        delete mQueryConstraints[i];
    }
}

QString IndexQuery::partition() const
{
    return mPartition->name();
}

quint32 IndexQuery::stateNumber() const
{
    return mBdbIndex->tag();
}

bool IndexQuery::matches(const QJsonValue &fieldValue)
{
    for (int i = 0; i < mQueryConstraints.size(); i++) {
        if (!mQueryConstraints[i]->matches(fieldValue))
            return false;
    }
    return true;
}

void IndexQuery::setMin(const QJsonValue &value)
{
    mMin = makeFieldValue(value, mPropertyType);
}

void IndexQuery::setMax(const QJsonValue &value)
{
    mMax = makeFieldValue(value, mPropertyType);
}

bool IndexQuery::seekToStart(QJsonValue &fieldValue)
{
    QByteArray forwardKey;
    if (mAscending) {
        forwardKey = makeForwardKey(mMin, ObjectKey());
        if (sDebugQuery) qDebug() << __FUNCTION__ << __LINE__ << "mMin" << mMin << "key" << forwardKey.toHex();
    } else {
        forwardKey = makeForwardKey(mMax, ObjectKey());
        if (sDebugQuery) qDebug() << __FUNCTION__ << __LINE__ << "mMax" << mMin << "key" << forwardKey.toHex();
    }

    bool ok = false;
    if (mAscending) {
        if (!mMin.isUndefined()) {
            ok = mCursor->seekRange(forwardKey);
            if (sDebugQuery) qDebug() << "IndexQuery::first" << __LINE__ << "ok after seekRange" << ok;
        }
        if (!ok) {
            ok = mCursor->first();
        }
    } else {
        // need a seekDescending
        ok = mCursor->last();
    }
    if (ok) {
        QByteArray baKey;
        mCursor->current(&baKey, 0);
        forwardKeySplit(baKey, fieldValue);
    }
    //qDebug() << "IndexQuery::seekToStart" << (mAscending ? mMin : mMax) << "ok" << ok << fieldValue;
    return ok;
}

bool IndexQuery::seekToNext(QJsonValue &fieldValue)
{
    bool ok = mAscending ? mCursor->next() : mCursor->prev();
    if (ok) {
        QByteArray baKey;
        mCursor->current(&baKey, 0);
        forwardKeySplit(baKey, fieldValue);
    }
    //qDebug() << "IndexQuery::seekToNext" << "ok" << ok << fieldValue;
    return ok;
}

JsonDbObject IndexQuery::currentObjectAndTypeNumber(ObjectKey &objectKey)
{
    QByteArray baValue;
    mCursor->current(0, &baValue);
    forwardValueSplit(baValue, objectKey);

    if (sDebugQuery) qDebug() << __FILE__ << __LINE__ << "objectKey" << objectKey << baValue.toHex();
    JsonDbObject object;
    mObjectTable->get(objectKey, &object);
    return object;
}

quint32 UuidQuery::stateNumber() const
{
    return mObjectTable->stateNumber();
}

bool UuidQuery::seekToStart(QJsonValue &fieldValue)
{
    bool ok;
    if (mAscending) {
        if (!mMin.isUndefined()) {
            ObjectKey objectKey(mMin.toString());
            ok = mCursor->seekRange(objectKey.toByteArray());
        } else {
            ok = mCursor->first();
        }
    } else {
        if (!mMax.isUndefined()) {
            ObjectKey objectKey(mMax.toString());
            ok = mCursor->seekRange(objectKey.toByteArray());
        } else {
            ok = mCursor->last();
        }
    }
    QByteArray baKey;
    while (ok) {
        mCursor->current(&baKey, 0);
        if (baKey.size() == 16)
            break;
        if (mAscending)
            ok = mCursor->next();
        else
            ok = mCursor->prev();
    }
    if (ok) {
        QUuid quuid(QUuid::fromRfc4122(baKey));
        ObjectKey objectKey(quuid);
        fieldValue = objectKey.key.toString();
    }
    return ok;
}

bool UuidQuery::seekToNext(QJsonValue &fieldValue)
{
    bool ok = mAscending ? mCursor->next() : mCursor->prev();
    QByteArray baKey;
    while (ok) {
        mCursor->current(&baKey, 0);
        if (baKey.size() == 16)
            break;
        if (mAscending)
            ok = mCursor->next();
        else
            ok = mCursor->prev();
    }
    if (ok) {
        QUuid quuid(QUuid::fromRfc4122(baKey));
        ObjectKey objectKey(quuid);
        fieldValue = objectKey.key.toString();
    }
    return ok;
}

JsonDbObject UuidQuery::currentObjectAndTypeNumber(ObjectKey &objectKey)
{
    QByteArray baKey, baValue;
    mCursor->current(&baKey, &baValue);
    objectKey = ObjectKey(baKey);

    if (sDebugQuery) qDebug() << __FILE__ << __LINE__ << "objectKey" << objectKey << baKey.toHex();
    JsonDbObject object(QJsonDocument::fromBinaryData(baValue).object());
    return object;
}

JsonDbObject IndexQuery::first()
{
    mSparseMatchPossible = false;
    for (int i = 0; i < mQueryConstraints.size(); i++) {
        mSparseMatchPossible |= mQueryConstraints[i]->sparseMatchPossible();
    }

    QJsonValue fieldValue;
    bool ok = seekToStart(fieldValue);
    if (sDebugQuery) qDebug() << "IndexQuery::first" << __LINE__ << "ok after first/last()" << ok;
    for (; ok; ok = seekToNext(fieldValue)) {
        mFieldValue = fieldValue;
        if (sDebugQuery) qDebug() << "IndexQuery::first()"
                                  << "mPropertyName" << mPropertyName
                                  << "fieldValue" << fieldValue
                                  << (mAscending ? "ascending" : "descending");

        if (sDebugQuery) qDebug() << "IndexQuery::first()" << "matches(fieldValue)" << matches(fieldValue);

        if (!matches(fieldValue))
            continue;

        ObjectKey objectKey;
        JsonDbObject object(currentObjectAndTypeNumber(objectKey));
        if (sDebugQuery) qDebug() << "IndexQuery::first()" << __LINE__ << "objectKey" << objectKey << object.value(JsonDbString::kDeletedStr).toBool();
        if (object.contains(JsonDbString::kDeletedStr) && object.value(JsonDbString::kDeletedStr).toBool())
            continue;

        if (!mTypeNames.isEmpty() && !mTypeNames.contains(object.value(JsonDbString::kTypeStr).toString()))
            continue;
        if (sDebugQuery) qDebug() << "mTypeName" << mTypeNames << "!contains" << object << "->" << object.value(JsonDbString::kTypeStr);

        if (mResidualQuery && !mResidualQuery->match(object, &mObjectCache, mPartition))
            continue;

        if (sDebugQuery) qDebug() << "IndexQuery::first()" << "returning objectKey" << objectKey;

        return object;
    }
    mUuid.clear();
    return QJsonObject();
}
JsonDbObject IndexQuery::next()
{
    QJsonValue fieldValue;
    while (seekToNext(fieldValue)) {
        if (sDebugQuery) qDebug() << "IndexQuery::next()" << "mPropertyName" << mPropertyName
                                  << "fieldValue" << fieldValue
                                  << (mAscending ? "ascending" : "descending");
        if (sDebugQuery) qDebug() << "IndexQuery::next()" << "matches(fieldValue)" << matches(fieldValue);
        if (!matches(fieldValue)) {
            if (mSparseMatchPossible)
                continue;
            else
                break;
        }

        ObjectKey objectKey;
        JsonDbObject object(currentObjectAndTypeNumber(objectKey));
        if (object.contains(JsonDbString::kDeletedStr) && object.value(JsonDbString::kDeletedStr).toBool())
            continue;

        if (!mTypeNames.isEmpty() && !mTypeNames.contains(object.value(JsonDbString::kTypeStr).toString()))
            continue;

        if (sDebugQuery) qDebug() << "IndexQuery::next()" << "objectKey" << objectKey;

        if (mResidualQuery && !mResidualQuery->match(object, &mObjectCache, mPartition))
            continue;

        return object;
    }
    mUuid.clear();
    return QJsonObject();
}

bool lessThan(const QJsonValue &a, const QJsonValue &b)
{
    if (a.type() == b.type()) {
        if (a.type() == QJsonValue::Double) {
            return a.toDouble() < b.toDouble();
        } else if (a.type() == QJsonValue::String) {
            return a.toString() < b.toString();
        } else if (a.type() == QJsonValue::Bool) {
            return a.toBool() < b.toBool();
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool greaterThan(const QJsonValue &a, const QJsonValue &b)
{
    if (a.type() == b.type()) {
        if (a.type() == QJsonValue::Double) {
            return a.toDouble() > b.toDouble();
        } else if (a.type() == QJsonValue::String) {
            return a.toString() > b.toString();
        } else if (a.type() == QJsonValue::Bool) {
            return a.toBool() > b.toBool();
        } else {
            return false;
        }
    } else {
        return false;
    }
}

class QueryConstraintGt: public QueryConstraint {
public:
    QueryConstraintGt(const QJsonValue &v) { mValue = v; }
    bool matches(const QJsonValue &v) { return greaterThan(v, mValue); }
private:
    QJsonValue mValue;
};
class QueryConstraintGe: public QueryConstraint {
public:
    QueryConstraintGe(const QJsonValue &v) { mValue = v; }
    bool matches(const QJsonValue &v) { return greaterThan(v, mValue) || (v == mValue); }
private:
    QJsonValue mValue;
};
class QueryConstraintLt: public QueryConstraint {
public:
    QueryConstraintLt(const QJsonValue &v) { mValue = v; }
    bool matches(const QJsonValue &v) { return lessThan(v, mValue); }
private:
    QJsonValue mValue;
};
class QueryConstraintLe: public QueryConstraint {
public:
    QueryConstraintLe(const QJsonValue &v) { mValue = v; }
    bool matches(const QJsonValue &v) { return lessThan(v, mValue) || (v == mValue); }
private:
    QJsonValue mValue;
};
class QueryConstraintEq: public QueryConstraint {
public:
    QueryConstraintEq(const QJsonValue &v) { mValue = v; }
    bool matches(const QJsonValue &v) { return v == mValue; }
private:
    QJsonValue mValue;
};
class QueryConstraintNe: public QueryConstraint {
public:
    QueryConstraintNe(const QJsonValue &v) { mValue = v; }
    bool sparseMatchPossible() const { return true; }
    bool matches(const QJsonValue &v) { return v != mValue; }
private:
    QJsonValue mValue;
};
class QueryConstraintExists: public QueryConstraint {
public:
    QueryConstraintExists() { }
    bool matches(const QJsonValue &v) { return !v.isUndefined(); }
private:
};
class QueryConstraintNotExists: public QueryConstraint {
public:
    QueryConstraintNotExists() { }
    // this will never match
    bool matches(const QJsonValue &v) { return v.isUndefined(); }
private:
};
class QueryConstraintIn: public QueryConstraint {
public:
    QueryConstraintIn(const QJsonValue &v) { mList = v.toArray();}
    bool sparseMatchPossible() const { return true; }

    bool matches(const QJsonValue &v) {
        return mList.contains(v);
    }
private:
    QJsonArray mList;
};
class QueryConstraintNotIn: public QueryConstraint {
public:
    QueryConstraintNotIn(const QJsonValue &v) { mList = v.toArray();}
    bool sparseMatchPossible() const { return true; }
    bool matches(const QJsonValue &v) {
        return !mList.contains(v);
    }
private:
    QJsonArray mList;
};
class QueryConstraintContains: public QueryConstraint {
public:
    QueryConstraintContains(const QJsonValue &v) { mValue = v;}
    bool sparseMatchPossible() const { return true; }
    bool matches(const QJsonValue &v) {
        return v.toArray().contains(mValue);
    }
private:
    QJsonValue mValue;
};
class QueryConstraintStartsWith: public QueryConstraint {
public:
    QueryConstraintStartsWith(const QString &v) { mValue = v;}
    bool sparseMatchPossible() const { return true; }
    bool matches(const QJsonValue &v) {
        return (v.type() == QJsonValue::String) && v.toString().startsWith(mValue);
    }
private:
    QString mValue;
};
class QueryConstraintRegExp: public QueryConstraint {
public:
    QueryConstraintRegExp(const QRegExp &regexp) : mRegExp(regexp) {}
    bool matches(const QJsonValue &v) { return mRegExp.exactMatch(v.toString()); }
    bool sparseMatchPossible() const { return true; }
private:
    QString mValue;
    QRegExp mRegExp;
};

void JsonDbPartition::compileOrQueryTerm(IndexQuery *indexQuery, const QueryTerm &queryTerm)
{
    QString op = queryTerm.op();
    QJsonValue fieldValue = queryTerm.value();
    if (op == ">") {
        indexQuery->addConstraint(new QueryConstraintGt(fieldValue));
        indexQuery->setMin(fieldValue);
    } else if (op == ">=") {
        indexQuery->addConstraint(new QueryConstraintGe(fieldValue));
        indexQuery->setMin(fieldValue);
    } else if (op == "<") {
        indexQuery->addConstraint(new QueryConstraintLt(fieldValue));
        indexQuery->setMax(fieldValue);
    } else if (op == "<=") {
        indexQuery->addConstraint(new QueryConstraintLe(fieldValue));
        indexQuery->setMax(fieldValue);
    } else if (op == "=") {
        indexQuery->addConstraint(new QueryConstraintEq(fieldValue));
        indexQuery->setMin(fieldValue);
        indexQuery->setMax(fieldValue);
    } else if (op == "=~") {
        const QRegExp &re = queryTerm.regExpConst();
        QRegExp::PatternSyntax syntax = re.patternSyntax();
        Qt::CaseSensitivity cs = re.caseSensitivity();
        QString pattern = re.pattern();
        indexQuery->addConstraint(new QueryConstraintRegExp(re));
        if (cs == Qt::CaseSensitive) {
            QString prefix;
            if ((syntax == QRegExp::Wildcard)
                && mWildCardPrefixRegExp.exactMatch(pattern)) {
                prefix = mWildCardPrefixRegExp.cap(1);
                if (jsondbSettings->debug())
                    qDebug() << "wildcard regexp prefix" << pattern << prefix;
            }
            indexQuery->setMin(prefix);
            indexQuery->setMax(prefix);
        }
    } else if (op == "!=") {
        indexQuery->addConstraint(new QueryConstraintNe(fieldValue));
    } else if (op == "exists") {
        indexQuery->addConstraint(new QueryConstraintExists);
    } else if (op == "notExists") {
        indexQuery->addConstraint(new QueryConstraintNotExists);
    } else if (op == "in") {
        QJsonArray value = queryTerm.value().toArray();
        //qDebug() << "in" << value << value.size();
        if (value.size() == 1)
            indexQuery->addConstraint(new QueryConstraintEq(value.at(0)));
        else
            indexQuery->addConstraint(new QueryConstraintIn(queryTerm.value()));
    } else if (op == "notIn") {
        indexQuery->addConstraint(new QueryConstraintNotIn(queryTerm.value()));
    } else if (op == "in") {
        indexQuery->addConstraint(new QueryConstraintContains(queryTerm.value()));
    } else if (op == "startsWith") {
        indexQuery->addConstraint(new QueryConstraintStartsWith(queryTerm.value().toString()));
    }
}

IndexQuery *JsonDbPartition::compileIndexQuery(const JsonDbOwner *owner, const JsonDbQuery *query)
{
    IndexQuery *indexQuery = 0;
    JsonDbQuery *residualQuery = new JsonDbQuery();
    QString orderField;
    QSet<QString> typeNames;
    const QList<OrderTerm> &orderTerms = query->orderTerms;
    const QList<OrQueryTerm> &orQueryTerms = query->queryTerms;
    QString indexCandidate;
    int indexedQueryTermCount = 0;
    JsonDbObjectTable *table = mObjectTable; //TODO fix me
    JsonDbView *view = 0;
    if (orQueryTerms.size()) {
        for (int i = 0; i < orQueryTerms.size(); i++) {
            const OrQueryTerm orQueryTerm = orQueryTerms[i];
            const QList<QString> &querypropertyNames = orQueryTerm.propertyNames();
            if (querypropertyNames.size() == 1) {
                //QString fieldValue = queryTerm.value().toString();
                QString propertyName = querypropertyNames[0];

                const QList<QueryTerm> &queryTerms = orQueryTerm.terms();
                const QueryTerm &queryTerm = queryTerms[0];

                if ((typeNames.size() == 1)
                    && mViews.contains(typeNames.toList()[0])) {
                    view = mViews[typeNames.toList()[0]];
                    table = view->objectTable();
                }

                if (table->indexSpec(propertyName))
                    indexedQueryTermCount++;
                else if (indexCandidate.isEmpty() && (propertyName != JsonDbString::kTypeStr)) {
                    indexCandidate = propertyName;
                    if (!queryTerm.joinField().isEmpty())
                        indexCandidate = queryTerm.joinPaths()[0].join("->");
                }

                propertyName = queryTerm.propertyName();
                QString fieldValue = queryTerm.value().toString();
                QString op = queryTerm.op();
                if (propertyName == JsonDbString::kTypeStr) {
                    if ((op == "=") || (op == "in")) {
                        QSet<QString> types;
                        if (op == "=") {
                            types << fieldValue;
                            for (int i = 1; i < queryTerms.size(); ++i) {
                                if (queryTerms[i].propertyName() == JsonDbString::kTypeStr && queryTerms[i].op() == "=")
                                    types << queryTerms[i].value().toString();
                            }
                        } else {
                            QJsonArray array = queryTerm.value().toArray();
                            types.clear();
                            for (int t = 0; t < array.size(); t++)
                                types << array.at(t).toString();
                        }

                        if (typeNames.count()) {
                            typeNames.intersect(types);
                            if (!typeNames.count()) {
                                // make this a null query -- I really need a domain (partial order) here and not a set
                                typeNames = types;
                            }
                        } else {
                            typeNames = types;
                        }
                    } else if ((op == "!=") || (op == "notIn")) {
                        QSet<QString> types;
                        if (op == "!=")
                            types << fieldValue;
                        else {
                            QJsonArray array = queryTerm.value().toArray();
                            for (int t = 0; t < array.size(); t++)
                                types << array.at(t).toString();
                        }
                    }
                }
            }
        }
    }
    if ((typeNames.size() == 1)
        && mViews.contains(typeNames.toList()[0])) {
        view = mViews[typeNames.toList()[0]];
        table = view->objectTable();
    }
    if (!indexedQueryTermCount && !indexCandidate.isEmpty()) {
            if (jsondbSettings->debug())
                qDebug() << "adding index" << indexCandidate;
            //TODO: remove this
            table->addIndexOnProperty(indexCandidate);
    }

    for (int i = 0; i < orderTerms.size(); i++) {
        const OrderTerm &orderTerm = orderTerms[i];
        QString propertyName = orderTerm.propertyName;
        if (!table->indexSpec(propertyName)) {
            if (jsondbSettings->verbose() || jsondbSettings->performanceLog())
                qDebug() << "Unindexed sort term" << propertyName << orderTerm.ascending;
            if (0) {
                if (jsondbSettings->verbose())
                    qDebug() << "adding index for sort term" << propertyName;
                Q_ASSERT(table);
                //TODO: remove this
                table->addIndexOnProperty(propertyName);
                Q_ASSERT(table->indexSpec(propertyName));
                if (jsondbSettings->verbose())
                    qDebug() << "done adding index" << propertyName;
            } else {
                residualQuery->orderTerms.append(orderTerm);
                continue;
            }
        }
        if (!indexQuery) {
            orderField = propertyName;
            const IndexSpec *indexSpec = table->indexSpec(propertyName);
            if (view)
                view->updateView();

            indexQuery = IndexQuery::indexQuery(this, table, propertyName, indexSpec->propertyType,
                                                owner, orderTerm.ascending);
        } else if (orderField != propertyName) {
            qCritical() << QString("unimplemented: multiple order terms. Sorting on '%1'").arg(orderField);
            residualQuery->orderTerms.append(orderTerm);
        }
    }

    for (int i = 0; i < orQueryTerms.size(); i++) {
        const OrQueryTerm &orQueryTerm = orQueryTerms[i];
        const QList<QueryTerm> &queryTerms = orQueryTerm.terms();
        if (queryTerms.size() == 1) {
            QueryTerm queryTerm = queryTerms[0];
            QString propertyName = queryTerm.propertyName();
            QString fieldValue = queryTerm.value().toString();
            QString op = queryTerm.op();

            if (!queryTerm.joinField().isEmpty()) {
                residualQuery->queryTerms.append(queryTerm);
                propertyName = queryTerm.joinField();
                op = "exists";
                queryTerm.setPropertyName(propertyName);
                queryTerm.setOp(op);
                queryTerm.setJoinField(QString());
            }
            if (!table->indexSpec(propertyName)
                || (indexQuery
                    && (propertyName != orderField))) {
                if (jsondbSettings->verbose() || jsondbSettings->debug())
                    qDebug() << "residual query term" << propertyName << "orderField" << orderField;
                residualQuery->queryTerms.append(queryTerm);
                continue;
            }

            if (!indexQuery && (propertyName != JsonDbString::kTypeStr) && table->indexSpec(propertyName)) {
                orderField = propertyName;
                const IndexSpec *indexSpec = table->indexSpec(propertyName);
                if (view)
                    view->updateView();
                indexQuery = IndexQuery::indexQuery(this, table, propertyName, indexSpec->propertyType, owner);
            }

            if (propertyName == orderField) {
                compileOrQueryTerm(indexQuery, queryTerm);
            } else {
                residualQuery->queryTerms.append(orQueryTerm);
            }
        } else {
            residualQuery->queryTerms.append(orQueryTerm);
        }
    }

    if (!indexQuery) {
        QString defaultIndex = JsonDbString::kUuidStr;
        if (typeNames.size()) {
            if ((typeNames.size() == 1)
                && mViews.contains(typeNames.toList()[0])) {
                view = mViews[typeNames.toList()[0]];
                table = view->objectTable();
            } else
                defaultIndex = JsonDbString::kTypeStr;
        }
        const IndexSpec *indexSpec = table->indexSpec(defaultIndex);

        //qDebug() << "defaultIndex" << defaultIndex << "on table" << indexSpec->objectType;

        if (view)
            view->updateView();
        indexQuery = IndexQuery::indexQuery(this, table, defaultIndex, indexSpec->propertyType, owner);
        if (typeNames.size() == 0)
            qCritical() << "searching all objects" << query->query;

        if (defaultIndex == JsonDbString::kTypeStr) {
            foreach (const OrQueryTerm &term, orQueryTerms) {
                QList<QueryTerm> terms = term.terms();
                if (terms.size() == 1 && terms[0].propertyName() == JsonDbString::kTypeStr) {
                    compileOrQueryTerm(indexQuery, terms[0]);
                    break;
                }
            }
        }
    }
    if (typeNames.count() > 0)
        indexQuery->setTypeNames(typeNames);
    if (residualQuery->queryTerms.size() || residualQuery->orderTerms.size())
        indexQuery->setResidualQuery(residualQuery);
    else
        delete residualQuery;
    indexQuery->setAggregateOperation(query->mAggregateOperation);
    return indexQuery;
}

void JsonDbPartition::doIndexQuery(const JsonDbOwner *owner, JsonDbObjectList &results, int &limit, int &offset,
                                      IndexQuery *indexQuery)
{
    if (sDebugQuery) qDebug() << "doIndexQuery" << "limit" << limit << "offset" << offset;
    bool countOnly = (indexQuery->aggregateOperation() == "count");
    int count = 0;
    for (JsonDbObject object = indexQuery->first();
         !object.isEmpty();
         object = indexQuery->next()) {
        if (!owner->isAllowed(object, indexQuery->partition(), QString("read")))
            continue;
        if (limit && (offset <= 0)) {
            if (!countOnly) {
                if (sDebugQuery) qDebug() << "appending result" << object << endl;
                results.append(object);
            }
            limit--;
            count++;
        }
        offset--;
        if (limit == 0)
            break;
    }
    if (countOnly) {
        QJsonObject countObject;
        countObject.insert(QLatin1String("count"), count);
        results.append(countObject);
    }
}

bool JsonDbPartition::checkQuota(const JsonDbOwner *owner, int size) const
{
    Q_UNUSED(owner);
    Q_UNUSED(size);
    return true;
}

bool JsonDbPartition::addToQuota(const JsonDbOwner *owner, int size)
{
    Q_UNUSED(owner);
    Q_UNUSED(size);
    return true;
}

JsonDbQueryResult JsonDbPartition::queryObjects(const JsonDbOwner *owner, const JsonDbQuery *query, int limit, int offset)
{
    JsonDbQueryResult result;
    JsonDbObjectList results;
    JsonDbObjectList joinedResults;

    QElapsedTimer time;
    time.start();
    IndexQuery *indexQuery = compileIndexQuery(owner, query);

    int elapsedToCompile = time.elapsed();
    doIndexQuery(owner, results, limit, offset, indexQuery);
    int elapsedToQuery = time.elapsed();
    quint32 stateNumber = indexQuery->stateNumber();
    int length = results.size();
    JsonDbQuery *residualQuery = indexQuery->residualQuery();
    if (residualQuery && residualQuery->orderTerms.size()) {
        if (jsondbSettings->verbose())
            qDebug() << "queryPersistentObjects" << "sorting";
        sortValues(residualQuery, results, joinedResults);
    }

    QJsonArray sortKeys;
    sortKeys.append(indexQuery->propertyName());
    sortKeys.append(indexQuery->objectTable()->filename());

    delete indexQuery;

    QJsonValue::Type resultType = query->resultType;
    QStringList mapExpressions = query->mapExpressionList;
    QStringList mapKeys = query->mapKeyList;

    if (mapExpressions.length() && query->mAggregateOperation.compare("count")) {
        QMap<QString, JsonDbObject> objectCache;
        int nExpressions = mapExpressions.length();
        QVector<QVector<QStringList> > joinPaths(nExpressions);
        for (int i = 0; i < nExpressions; i++) {
            QString propertyName = mapExpressions[i];
            QStringList joinPath = propertyName.split("->");
            int joinPathSize = joinPath.size();
            QVector<QStringList> fieldPaths(joinPathSize);
            for (int j = 0; j < joinPathSize; j++) {
                QString joinField = joinPath[j];
                fieldPaths[j] = joinField.split('.');
            }
            joinPaths[i] = fieldPaths;
        }

        QList<JsonDbObject> mappedResult;
        QJsonArray valueArray;
        for (int r = 0; r < results.size(); r++) {
            const JsonDbObject obj = results.at(r);
            QJsonArray list;
            QJsonObject map;
            for (int i = 0; i < nExpressions; i++) {
                QJsonValue v;

                QVector<QStringList> &joinPath = joinPaths[i];
                int joinPathSize = joinPath.size();
                if (joinPathSize == 1) {
                    v = obj.propertyLookup(joinPath[0]);
                    QJsonObject vObj;
                    vObj.insert("v", v);
                } else {
                    JsonDbObject baseObject(obj);
                    for (int j = 0; j < joinPathSize-1; j++) {
                        QJsonValue uuidQJsonValue = baseObject.propertyLookup(joinPath[j]).toString();
                        QString uuid = uuidQJsonValue.toString();
                        if (uuid.isEmpty()) {
                            baseObject = JsonDbObject();
                        } else if (objectCache.contains(uuid)) {
                            baseObject = objectCache.value(uuid);
                        } else {
                            ObjectKey objectKey(uuid);
                            bool gotBaseObject = getObject(objectKey, baseObject);
                            if (gotBaseObject)
                                objectCache.insert(uuid, baseObject);
                        }
                    }
                    v = baseObject.propertyLookup(joinPath[joinPathSize-1]);
                }

                if (resultType == QJsonValue::Object)
                    map.insert(mapKeys[i], v);
                else if (resultType == QJsonValue::Array)
                    list.append(v);
                else
                    valueArray.append(v);
            }

            if (resultType == QJsonValue::Object) {
                mappedResult.append(map);
            } else if (resultType == QJsonValue::Array) {
                valueArray.append(list);
            }
        }

        result.data = mappedResult;
        result.values = valueArray;
    } else {
        result.data = results;
    }

    result.length = length;
    result.offset = offset;
    result.state = (qint32)stateNumber;
    result.sortKeys = sortKeys;
    int elapsedToDone = time.elapsed();
    if (jsondbSettings->verbose())
        qDebug() << "elapsed" << elapsedToCompile << elapsedToQuery << elapsedToDone << query->query;
    return result;
}

JsonDbWriteResult JsonDbPartition::updateObjects(const JsonDbOwner *owner, const JsonDbObjectList &objects, JsonDbPartition::WriteMode mode)
{
    JsonDbWriteResult result;
    WithTransaction transaction(this);
    QList<JsonDbUpdate> updated;
    QString errorMsg;

    foreach (const JsonDbObject &toUpdate, objects) {
        JsonDbObject object = toUpdate;

        bool forRemoval = object.value(JsonDbString::kDeletedStr).toBool();

        if (!object.contains(JsonDbString::kUuidStr) || object.value(JsonDbString::kUuidStr).isNull()) {
            if (forRemoval) {
                result.code = JsonDbError::MissingUUID;
                result.message = QLatin1String("Missing '_uuid' field in object");
                return result;
            }

            if (!object.contains(JsonDbString::kOwnerStr)
                || ((object.value(JsonDbString::kOwnerStr).toString() != owner->ownerId())
                    && !owner->isAllowed(object, mPartitionName, "setOwner")))
                object.insert(JsonDbString::kOwnerStr, owner->ownerId());
            object.generateUuid();
        }

        if (!forRemoval && object.value(JsonDbString::kTypeStr).toString().isEmpty()) {
            result.code = JsonDbError::MissingType;
            result.message = QLatin1String("Missing '_type' field in object");
            return result;
        }

        if (!(mode == ViewObject || checkNaturalObjectType(object, errorMsg))) {
            result.code = JsonDbError::MissingType;
            result.message = errorMsg;
        }
        if (!(forRemoval || validateSchema(object.type(), object, errorMsg))) {
            result.code = JsonDbError::FailedSchemaValidation;
            result.message = errorMsg;
            return result;
        }

        JsonDbObjectTable *objectTable = findObjectTable(object.type());

        if (mode != ViewObject) {
            if (mViewTypes.contains(object.type())) {
                result.code = JsonDbError::InvalidType;
                result.message = QString("Cannot write object of view type '%1'").arg(object.value(JsonDbString::kTypeStr).toString());
                return result;
            }

            transaction.addObjectTable(objectTable);
        }

        JsonDbObject master;
        bool forCreation = !getObject(object.uuid(), master, object.type());

        // FIXME: explicity disallow changing _type

        if (mode != ReplicatedWrite && forCreation && forRemoval) {
            result.code =  JsonDbError::MissingObject;
            result.message = QLatin1String("Cannot remove non-existing object");
            return result;
        }

        if (!(forCreation || owner->isAllowed(master, mPartitionName, "write"))) {
            result.code = JsonDbError::OperationNotPermitted;
            result.message = QLatin1String("Access denied");
            return result;
        }

        if (!master.value(JsonDbString::kOwnerStr).toString().isEmpty())
            object.insert(JsonDbString::kOwnerStr, master.value(JsonDbString::kOwnerStr));
        else if (object.value(JsonDbString::kOwnerStr).toString().isEmpty())
            object.insert(JsonDbString::kOwnerStr, owner->ownerId());

        if (!(forRemoval || owner->isAllowed(object, mPartitionName, "write"))) {
            result.code = JsonDbError::OperationNotPermitted;
            result.message = QLatin1String("Access denied");
            return result;
        }

        bool validWrite;
        QString versionWritten;
        JsonDbObject oldMaster = master;

        switch (mode) {
        case OptimisticWrite:
            validWrite = master.updateVersionOptimistic(object, &versionWritten);
            break;
        case ViewObject:
        case ForcedWrite:
            master = object;
            versionWritten = master.computeVersion();
            validWrite = true;
            break;
        case ReplicatedWrite:
            validWrite = master.updateVersionReplicating(object);
            versionWritten = master.version();
            break;
        default:
            result.code = JsonDbError::InvalidRequest;
            result.message = QLatin1String("Missing writeMode implementation.");
            return result;
        }

        if (!validWrite) {
            if (mode == ReplicatedWrite) {
                result.code = JsonDbError::InvalidRequest;
                result.message = QLatin1String("Replication has reject your update for sanity reasons");
            } else {
                if (jsondbSettings->debug())
                    qDebug() << "Stale update detected - expected version:" << oldMaster.version() << object;
                result.code = JsonDbError::UpdatingStaleVersion;
                result.message = QString("Updating stale version of object. Expected version %1, received %2").arg(oldMaster.version()).arg(versionWritten);
            }
            return result;
        }

        // recheck, it might just be a conflict removal
        forRemoval = master.isDeleted();

        if (master.type().isNull())
            master.insert(JsonDbString::kTypeStr, oldMaster.type());

        bool isVisibleWrite = oldMaster.version() != master.version();

        if (isVisibleWrite) {
            JsonDbError::ErrorCode errorCode;
            if (!(forRemoval || (errorCode = checkBuiltInTypeValidity(master, oldMaster, errorMsg)) == JsonDbError::NoError)) {
                result.code = errorCode;
                result.message = errorMsg;
                return result;
            } else if (oldMaster.type() == JsonDbString::kSchemaTypeStr &&
                       !checkCanRemoveSchema(oldMaster, errorMsg)) {
                result.code = JsonDbError::InvalidSchemaOperation;
                result.message = errorMsg;
                return result;
            }
        }

        ObjectKey objectKey(master.uuid());

        if (!forCreation)
            objectTable->deindexObject(objectKey, oldMaster, objectTable->stateNumber());

        if (!objectTable->put(objectKey, master)) {
            result.code = JsonDbError::DatabaseError;
            result.message = objectTable->errorMessage();
        }

        if (jsondbSettings->debug())
            qDebug() << "Wrote object" << objectKey << endl << master << endl << oldMaster;

        JsonDbNotification::Action action;
        quint32 stateNumber;

        if (forRemoval) {
            action = JsonDbNotification::Delete;
            stateNumber = objectTable->storeStateChange(objectKey, ObjectChange::Deleted, oldMaster);
        } else if (forCreation) {
            action = JsonDbNotification::Create;
            stateNumber = objectTable->storeStateChange(objectKey, ObjectChange::Created);
        } else {
            action =  JsonDbNotification::Update;
            stateNumber = objectTable->storeStateChange(objectKey, ObjectChange::Updated, oldMaster);
       }

        if (!forRemoval)
            objectTable->indexObject(objectKey, master, objectTable->stateNumber());

        updateBuiltInTypes(master, oldMaster);

        result.state = stateNumber;


        master.insert(JsonDbString::kVersionStr, versionWritten);
        result.objectsWritten.append(master);
        updated.append(JsonDbUpdate(oldMaster, master, action));
    }

    transaction.commit();

    emit objectsUpdated(updated);
    return result;
}

JsonDbWriteResult JsonDbPartition::updateObject(const JsonDbOwner *owner, const JsonDbObject &object, JsonDbPartition::WriteMode mode)
{
    return updateObjects(owner, JsonDbObjectList() << object, mode);
}

void JsonDbPartition::checkIndex(const QString &propertyName)
{
// TODO
    if (mObjectTable->indexSpec(propertyName))
        mObjectTable->indexSpec(propertyName)->index->checkIndex();
}

bool JsonDbPartition::compact()
{
    for (QHash<QString,QPointer<JsonDbView> >::const_iterator it = mViews.begin();
         it != mViews.end();
         ++it) {
        it.value()->objectTable()->compact();
    }
    bool result = true;
    result &= mObjectTable->compact();
    return result;
}

struct JsonDbStat JsonDbPartition::stat() const
{
    JsonDbStat result;
    for (QHash<QString,QPointer<JsonDbView> >::const_iterator it = mViews.begin();
          it != mViews.end();
          ++it) {
        result += it.value()->objectTable()->stat();
     }
    result += mObjectTable->stat();
    return result;
}

QString JsonDbPartition::name() const
{
    return mPartitionName;
}
void JsonDbPartition::setName(const QString &name)
{
    mPartitionName = name;
}

struct QJsonSortable {
    QJsonValue key;
    QJsonObject result;
    QJsonObject joinedResult;
};

bool sortableLessThan(const QJsonSortable &a, const QJsonSortable &b)
{
    return lessThan(a.key, b.key);
}
bool sortableGreaterThan(const QJsonSortable &a, const QJsonSortable &b)
{
    return greaterThan(a.key, b.key);
}

void JsonDbPartition::sortValues(const JsonDbQuery *parsedQuery, JsonDbObjectList &results, JsonDbObjectList &joinedResults)
{
    const QList<OrderTerm> &orderTerms = parsedQuery->orderTerms;
    if (!orderTerms.size() || (results.size() < 2))
        return;
    const OrderTerm &orderTerm0 = orderTerms[0];
    QString field0 = orderTerm0.propertyName;
    bool ascending = orderTerm0.ascending;
    QStringList path0 = field0.split('.');

    if (orderTerms.size() == 1) {
        QVector<QJsonSortable> valuesToSort(results.size());
        int resultsSize = results.size();
        int joinedResultsSize = joinedResults.size();

        for (int i = 0; i < resultsSize; i++) {
            QJsonSortable *p = &valuesToSort[i];
            JsonDbObject r = results.at(i);
            p->key = r.propertyLookup(path0);
            p->result = r;
            if (joinedResultsSize > i)
                p->joinedResult = joinedResults.at(i);
        }

        if (ascending)
            qStableSort(valuesToSort.begin(), valuesToSort.end(), sortableLessThan);
        else
            qStableSort(valuesToSort.begin(), valuesToSort.end(), sortableGreaterThan);

        results = JsonDbObjectList();
        joinedResults = JsonDbObjectList();
        for (int i = 0; i < resultsSize; i++) {
            QJsonSortable *p = &valuesToSort[i];
            results.append(p->result);
            if (joinedResultsSize > i)
                joinedResults.append(p->joinedResult);
        }
    } else {
        qCritical() << "Unimplemented: sorting on multiple keys or non-string keys";
    }
}

bool JsonDbPartition::checkCanRemoveSchema(const JsonDbObject &schema, QString &message)
{
    QString schemaName = schema.value("name").toString();

    // for View types, make sure no Maps or Reduces point at this type
    // the call to getObject will have updated the view, so pending Map or Reduce object removes will be forced
    JsonDbView *view = findView(schemaName);
    if (view && view->isActive()) {
        message = QString("An active view with targetType of %2 exists. You cannot remove the schema").arg(schemaName);
        return false;
    }

    // check if any objects exist
    GetObjectsResult getObjectResponse = getObjects(JsonDbString::kTypeStr, schemaName);

    // for non-View types, if objects exist the schema cannot be removed
    if (!mViewTypes.contains(schemaName)) {
        if (getObjectResponse.data.size() != 0) {
            message = QString("%1 object(s) of type %2 exist. You cannot remove the schema")
                    .arg(getObjectResponse.data.size())
                    .arg(schemaName);
            return false;
        }
    }

    return true;
}

bool JsonDbPartition::validateSchema(const QString &schemaName, const JsonDbObject &object, QString &errorMsg)
{
    errorMsg.clear();

    if (!jsondbSettings->validateSchemas()) {
        if (jsondbSettings->debug())
            qDebug() << "Not validating schemas";
        return true;
    }

    QJsonObject result = mSchemas.validate(schemaName, object);
    if (!result.value(JsonDbString::kCodeStr).isNull()) {
        errorMsg = result.value(JsonDbString::kMessageStr).toString();
        if (jsondbSettings->debug())
            qDebug() << "Schema validation error: " << errorMsg << object;
        return false;
    }

    return true;
}

bool JsonDbPartition::checkNaturalObjectType(const JsonDbObject &object, QString &errorMsg)
{
    QString type = object.value(JsonDbString::kTypeStr).toString();
    if (mViewTypes.contains(type)) {
        QString str = JsonWriter().toString(object.toVariantMap());
        errorMsg = QString("Cannot create/remove object of view type '%1': '%2'").arg(type).arg(str);
        return false;
    }

    return true;
}

JsonDbError::ErrorCode JsonDbPartition::checkBuiltInTypeValidity(const JsonDbObject &object, const JsonDbObject &oldObject, QString &errorMsg)
{
    QString objectType = object.value(JsonDbString::kTypeStr).toString();
    errorMsg.clear();

    if (objectType == JsonDbString::kSchemaTypeStr &&
            !checkCanAddSchema(object, oldObject, errorMsg))
        return JsonDbError::InvalidSchemaOperation;
    else if (objectType == JsonDbString::kMapTypeStr &&
             !JsonDbMapDefinition::validateDefinition(object, this, errorMsg))
        return JsonDbError::InvalidMap;
    else if (objectType == JsonDbString::kReduceTypeStr &&
             !JsonDbReduceDefinition::validateDefinition(object, this, errorMsg))
        return JsonDbError::InvalidReduce;
    else if (objectType == JsonDbString::kIndexTypeStr && !JsonDbIndex::validateIndex(object, oldObject, errorMsg))
        return JsonDbError::InvalidIndexOperation;

    return JsonDbError::NoError;
}

void JsonDbPartition::updateBuiltInTypes(const JsonDbObject &object, const JsonDbObject &oldObject)
{
    if (oldObject.type() == JsonDbString::kIndexTypeStr) {
        QString indexName = oldObject.value(JsonDbString::kNameStr).toString();
        QString propertyName = oldObject.value(JsonDbString::kPropertyNameStr).toString();

        if (indexName.isEmpty())
            indexName = propertyName;

        removeIndex(indexName, oldObject.value(JsonDbString::kObjectTypeStr).toString());
    }

    if (object.type() == JsonDbString::kIndexTypeStr && !object.isDeleted()) {
        QString indexName = object.value(JsonDbString::kNameStr).toString();
        QString propertyName = object.value(JsonDbString::kPropertyNameStr).toString();

        if (indexName.isEmpty())
            indexName = propertyName;


        addIndex(indexName, propertyName, object.value(JsonDbString::kPropertyTypeStr).toString(),
                 object.value(JsonDbString::kObjectTypeStr).toString(),
                 object.value(JsonDbString::kPropertyFunctionStr).toString(),
                 object.value(JsonDbString::kLocaleStr).toString(),
                 object.value(JsonDbString::kCollationStr).toString());
    }

    if (oldObject.type() == JsonDbString::kSchemaTypeStr) {
        removeSchema(oldObject.value(JsonDbString::kNameStr).toString());
    } else if (object.type() == JsonDbString::kSchemaTypeStr &&
             object.value(JsonDbString::kSchemaStr).type() == QJsonValue::Object
             && !object.isDeleted()) {
        setSchema(object.value(JsonDbString::kNameStr).toString(), object.value(JsonDbString::kSchemaStr).toObject());
    } else if (oldObject.type() == JsonDbString::kMapTypeStr || oldObject.type() == JsonDbString::kReduceTypeStr ||
               object.type() == JsonDbString::kMapTypeStr || object.type() == JsonDbString::kReduceTypeStr) {

        if (!oldObject.isEmpty())
            JsonDbView::removeDefinition(this, oldObject);

        if (!object.isDeleted())
            JsonDbView::createDefinition(this, object);
    }
}

void JsonDbPartition::setSchema(const QString &schemaName, const QJsonObject &schema)
{
    if (jsondbSettings->verbose())
        qDebug() << "setSchema" << schemaName << schema;

    QJsonObject errors = mSchemas.insert(schemaName, schema);

    if (!errors.isEmpty()) {
        qWarning() << "setSchema failed because of errors" << schemaName << schema;
        qWarning() << errors;
        // FIXME should we accept broken schemas?
    }

    if (schema.contains("extends")) {
        QJsonValue extendsValue = schema.value("extends");
        QString extendedSchemaName;
        if (extendsValue.type() == QJsonValue::String)
            extendedSchemaName = extendsValue.toString();
        else if ((extendsValue.type() == QJsonValue::Object)
                 && extendsValue.toObject().contains("$ref"))
            extendedSchemaName = extendsValue.toObject().value("$ref").toString();
        if (extendedSchemaName == JsonDbString::kViewTypeStr) {
            mViewTypes.insert(schemaName);
            addView(schemaName);
            if (jsondbSettings->verbose())
                qDebug() << "viewTypes" << mViewTypes;
        }
    }
    if (schema.contains("properties"))
        updateSchemaIndexes(schemaName, schema);
}

void JsonDbPartition::removeSchema(const QString &schemaName)
{
    if (jsondbSettings->verbose())
        qDebug() << "removeSchema" << schemaName;

    if (mSchemas.contains(schemaName)) {
        QJsonObject schema = mSchemas.take(schemaName);

        if (schema.contains("extends")) {
            QJsonValue extendsValue = schema.value("extends");
            QString extendedSchemaName;
            if (extendsValue.type() == QJsonValue::String)
                extendedSchemaName = extendsValue.toString();
            else if ((extendsValue.type() == QJsonValue::Object)
                     && extendsValue.toObject().contains("$ref"))
                extendedSchemaName = extendsValue.toObject().value("$ref").toString();

            if (extendedSchemaName == JsonDbString::kViewTypeStr) {
                mViewTypes.remove(schemaName);
                removeView(schemaName);
            }
        }
    }
}

void JsonDbPartition::updateSchemaIndexes(const QString &schemaName, QJsonObject object, const QStringList &path)
{
    QJsonObject properties = object.value("properties").toObject();
    const QList<QString> keys = properties.keys();
    for (int i = 0; i < keys.size(); i++) {
        const QString &k = keys[i];
        QJsonObject propertyInfo = properties.value(k).toObject();
        if (propertyInfo.contains("indexed")) {
            QString propertyType = (propertyInfo.contains("type") ? propertyInfo.value("type").toString() : "string");
            QStringList kpath = path;
            kpath << k;
            addIndexOnProperty(kpath.join("."), propertyType, schemaName);
        }
        if (propertyInfo.contains("properties"))
            updateSchemaIndexes(schemaName, propertyInfo, path + (QStringList() << k));
    }
}

void JsonDbPartition::setError(QJsonObject &map, int code, const QString &message)
{
    map.insert(JsonDbString::kCodeStr, code);
    map.insert(JsonDbString::kMessageStr, message);
}

QJsonObject JsonDbPartition::makeError(int code, const QString &message)
{
    QJsonObject map;
    setError(map, code, message);
    return map;
}

QJsonObject JsonDbPartition::makeResponse(const QJsonObject &resultmap, const QJsonObject &errormap, bool silent)
{
    QJsonObject map;
    if (jsondbSettings->verbose() && !silent && !errormap.isEmpty())
        qCritical() << errormap;

    if (!resultmap.isEmpty())
        map.insert( JsonDbString::kResultStr, resultmap);
    else
        map.insert( JsonDbString::kResultStr, QJsonValue());

    if (!errormap.isEmpty())
        map.insert( JsonDbString::kErrorStr, errormap );
    else
        map.insert( JsonDbString::kErrorStr, QJsonValue());
    return map;
}

QJsonObject JsonDbPartition::makeErrorResponse(QJsonObject &resultmap, int code, const QString &message, bool silent)
{
    QJsonObject errormap;
    setError(errormap, code, message);
    return makeResponse(resultmap, errormap, silent);
}

bool JsonDbPartition::responseIsError(const QJsonObject &responseMap)
{
    return responseMap.contains(JsonDbString::kErrorStr)
            && responseMap.value(JsonDbString::kErrorStr).isObject();
}

bool WithTransaction::addObjectTable(JsonDbObjectTable *table)
{
    if (!mPartition)
        return false;
    if (!mPartition->mTableTransactions.contains(table)) {
        bool ok = table->begin();
        mPartition->mTableTransactions.append(table);
        return ok;
    }
    return true;
}

void JsonDbPartition::initSchemas()
{
    if (jsondbSettings->verbose())
        qDebug() << "initSchemas";
    {
        JsonDbObjectList schemas = getObjects(JsonDbString::kTypeStr, JsonDbString::kSchemaTypeStr,
                                                QString()).data;
        for (int i = 0; i < schemas.size(); ++i) {
            JsonDbObject schemaObject = schemas.at(i);
            QString schemaName = schemaObject.value("name").toString();
            QJsonObject schema = schemaObject.value("schema").toObject();
            setSchema(schemaName, schema);
        }
    }

    foreach (const QString &schemaName, (QStringList() << JsonDbString::kNotificationTypeStr << JsonDbString::kViewTypeStr
                                         << "Capability" << JsonDbString::kIndexTypeStr)) {
        if (!mSchemas.contains(schemaName)) {
            QFile schemaFile(QString(":schema/%1.json").arg(schemaName));
            schemaFile.open(QIODevice::ReadOnly);
            JsonReader parser;
            bool ok = parser.parse(schemaFile.readAll());
            if (!ok) {
                qWarning() << "Parsing " << schemaName << " schema" << parser.errorString();
                return;
            }
            QJsonObject schema = QJsonObject::fromVariantMap(parser.result().toMap());
            JsonDbObject schemaObject;
            schemaObject.insert(JsonDbString::kTypeStr, JsonDbString::kSchemaTypeStr);
            schemaObject.insert("name", schemaName);
            schemaObject.insert("schema", schema);
            updateObject(mDefaultOwner, schemaObject, ForcedWrite);
        }
    }
    {
        JsonDbObject nameIndex;
        nameIndex.insert(JsonDbString::kTypeStr, JsonDbString::kIndexTypeStr);
        nameIndex.insert(JsonDbString::kPropertyNameStr, QLatin1String("name"));
        nameIndex.insert(JsonDbString::kPropertyTypeStr, QLatin1String("string"));
        nameIndex.insert(JsonDbString::kObjectTypeStr, QLatin1String("Capability"));
        updateObject(mDefaultOwner, nameIndex, ForcedWrite);

        const QString capabilityName("RootCapability");
        QFile capabilityFile(QString(":schema/%1.json").arg(capabilityName));
        capabilityFile.open(QIODevice::ReadOnly);
        JsonReader parser;
        bool ok = parser.parse(capabilityFile.readAll());
        if (!ok) {
            qWarning() << "Parsing " << capabilityName << " capability" << parser.errorString();
            return;
        }
        JsonDbObject capability = QJsonObject::fromVariantMap(parser.result().toMap());
        QString name = capability.value("name").toString();
        GetObjectsResult getObjectResponse = getObjects("name", name, "Capability");
        int count = getObjectResponse.data.size();
        if (!count) {
            if (jsondbSettings->verbose())
                qDebug() << "Creating capability" << capability;
            updateObject(mDefaultOwner, capability);
        } else {
            JsonDbObject currentCapability = getObjectResponse.data.at(0);
            if (currentCapability.value("accessRules") != capability.value("accessRules"))
                updateObject(mDefaultOwner, capability);
        }
    }
}

QT_END_NAMESPACE_JSONDB
