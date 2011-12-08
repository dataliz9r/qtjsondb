/****************************************************************************
**
** Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef JSONDB_CONNECTION_P_H
#define JSONDB_CONNECTION_P_H

#include <QObject>
#include <QVariant>
#include <QSet>
#include <QStringList>
#include <QLocalSocket>
#include <QTcpSocket>
#include <QScopedPointer>

#include "jsondb-global.h"

namespace QtAddOn { namespace JsonDb {

class QsonObject;
class QsonMap;
class QsonList;

class JsonDbConnectionPrivate;
class Q_ADDON_JSONDB_EXPORT JsonDbConnection : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)

public:
    enum Status {
        // maintain the order to match QML Component enum values (just in case)
        Disconnected = 0,
        Null = Disconnected,
        Ready = 1,
        Connecting = 2,
        Error = 3,

        Authenticating = 4
    };

    static JsonDbConnection *instance();
    static void              setDefaultToken( const QString& token );
    static QString           defaultToken();

    QT_DEPRECATED static QVariantMap makeFindRequest(const QVariant &);
    QT_DEPRECATED static QsonObject makeFindRequest(const QsonObject &);
    QT_DEPRECATED static QVariantMap makeRemoveRequest(const QString &);

    static QVariantMap makeCreateRequest(const QVariant &, const QString &partitionName = QString());
    static QVariantMap makeUpdateRequest(const QVariant &, const QString &partitionName = QString());
    static QVariantMap makeRemoveRequest(const QVariant &, const QString &partitionName = QString());

    static QVariantMap makeQueryRequest(const QString &, int offset = 0, int limit = -1,
                                        const QMap<QString,QVariant> &bindings = QMap<QString,QVariant>(),
                                        const QString &partitionName = QString());
    static QsonObject makeCreateRequest(const QsonObject &, const QString &partitionName = QString());
    static QsonObject makeUpdateRequest(const QsonObject &, const QString &partitionName = QString());
    static QsonObject makeRemoveRequest(const QsonObject &, const QString &partitionName = QString());
    static QsonObject makeNotification(const QString &, const QsonList &, const QString &partitionName = QString());
    static QVariantMap makeChangesSinceRequest(int stateNumber, const QStringList &types = QStringList(), const QString &partitionName = QString());

    JsonDbConnection(QObject *parent = 0);
    ~JsonDbConnection();

    Status status() const;

    // One-shot functions allow you to avoid constructing a JsonDbClient
    QT_DEPRECATED
    void oneShot( const QVariantMap& dbrequest, QObject *receiver=0,
                  const char *responseSlot=0, const char *errorSlot=0);
    // Synchronized calls pause execution until successful
    QsonObject sync(const QsonMap &dbrequest);
    QVariant sync(const QVariantMap &dbrequest);

    void setToken(const QString &token);
    void connectToServer(const QString &socketName = QString());
    void connectToHost(const QString &hostname, quint16 port);
    void disconnectFromServer();

    // General purpose request
    int  request(const QVariantMap &request);
    int  request(const QsonObject &request);
    bool request(int requestId, const QVariantMap &request);
    bool request(int requestId, const QsonMap &request);

    bool isConnected() const;
    Q_DECL_DEPRECATED inline bool connected() const
    { return isConnected(); }

    int makeRequestId();

    bool waitForConnected(int msecs = 30000);
    bool waitForDisconnected(int msecs = 30000);
    bool waitForBytesWritten(int msecs = 30000);

signals:
    void notified(const QString &notify_uuid, const QVariant &object, const QString &action);
    void notified(const QString &notify_uuid, const QsonObject &object, const QString &action);
    void response(int id, const QVariant &data);
    void response(int id, const QsonObject &data);
    void error(int id, int code, const QString &message);
    void readyWrite();

    void connected();
    void disconnected();

    // signals for properties
    void statusChanged();

private:
    Q_DISABLE_COPY(JsonDbConnection)
    Q_DECLARE_PRIVATE(JsonDbConnection)
    QScopedPointer<JsonDbConnectionPrivate> d_ptr;
    Q_PRIVATE_SLOT(d_func(), void _q_onConnected())
    Q_PRIVATE_SLOT(d_func(), void _q_onDisconnected())
    Q_PRIVATE_SLOT(d_func(), void _q_onError(QLocalSocket::LocalSocketError))
    Q_PRIVATE_SLOT(d_func(), void _q_onReceiveMessage(QsonObject))
};

} } // end namespace QtAddOn::JsonDb

#endif /* JSONDB_CONNECTION_P_H */
