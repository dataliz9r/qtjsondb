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
#ifndef TestJsonDbListModel_H
#define TestJsonDbListModel_H

#include <QCoreApplication>
#include <QList>
#include <QTest>
#include <QFile>
#include <QProcess>
#include <QEventLoop>
#include <QDebug>
#include <QLocalSocket>

#include <jsondb-client.h>
#include <jsondb-error.h>

#include "jsondb-listmodel.h"

Q_USE_JSONDB_NAMESPACE

class Notification {
public:
    Notification( const QString& notifyUuid, const QVariant& object, const QString& action )
	: mNotifyUuid(notifyUuid), mObject(object), mAction(action) {}
    
    QString  mNotifyUuid;
    QVariant mObject;
    QString  mAction;
};

class QDeclarativeEngine;
class QDeclarativeComponent;
class JsonDbListModel;

class ModelData {
public:
    ModelData();
    ~ModelData();
    QDeclarativeEngine *engine;
    QDeclarativeComponent *component;
    QObject *model;
};

class TestJsonDbListModel: public QObject
{
    Q_OBJECT
public:
    TestJsonDbListModel();
    ~TestJsonDbListModel();

    void deleteDbFiles();
    void connectListModel(JsonDbListModel *model);

public slots:
    void notified(const QString& notifyUuid, const QVariant& object, const QString& action );
    void response(int id, const QVariant& data);
    void error( int id, int code, const QString& message );
    void dataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight);
    void rowsInserted(const QModelIndex &parent, int first, int last);
    void rowsRemoved(const QModelIndex &parent, int first, int last);
    void rowsMoved( const QModelIndex &parent, int start, int end, const QModelIndex &destination, int row );
    void modelReset();

private slots:
    void initTestCase();
    void cleanupTestCase();

    void createItem();
    void updateItemClient();
    void updateItemSet();
    void updateItemSetProperty();
    void deleteItem();
    void sortedQuery();
    void ordering();
    void itemNotInCache();
    void roles();
    void totalRowCount();
    void listProperty();

private:
    void waitForResponse(QVariant id, int code=-1, QVariant notificationId=QVariant());
    void waitForItemsCreated(int items);
    QStringList getOrderValues(const JsonDbListModel *listModel);
    JsonDbListModel *createModel();
    void deleteModel(JsonDbListModel *model);
    QVariant readJsonFile(const QString &filename);

private:
    QEventLoop       mEventLoop;
    QProcess        *mProcess;
    JsonDbClient    *mClient;
    QStringList      mNotificationsReceived;
    QList<ModelData*> mModels;
    QString           mPluginPath;

    // Response values
    QString          mMessage;
    QString          mLastUuid;
    int              mCode;
    int              mItemsCreated;
    QVariant         mId, mData, mNotificationId;
    bool             mWaitingForNotification;
    bool             mWaitingForDataChange;
    bool             mWaitingForRowsRemoved;
    QList<Notification> mNotifications;
};

#endif