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

#ifndef JsonDbProxy_H
#define JsonDbProxy_H

#include <QObject>
#include <QMultiMap>
#include <QJSValue>

#include "jsondb.h"

QT_BEGIN_HEADER

namespace QtAddOn { namespace JsonDb {

class JsonDbProxy : public QObject {
    Q_OBJECT
public:
    JsonDbProxy( const JsonDbOwner *owner, JsonDb *jsonDb, QObject *parent=0 );
    ~JsonDbProxy();

    Q_SCRIPTABLE QVariantMap find(QVariantMap object);
    Q_SCRIPTABLE QVariantMap create(QVariantMap object );
    Q_SCRIPTABLE QVariantMap update(QVariantMap object );
    Q_SCRIPTABLE QVariantMap remove(QVariantMap object );

    Q_SCRIPTABLE QVariantMap notification(QString query, QStringList actions, QString script);

    Q_SCRIPTABLE QVariantMap createList(QVariantList);
    Q_SCRIPTABLE QVariantMap updateList(QVariantList);
    Q_SCRIPTABLE QVariantMap removeList(QVariantList );

    void setOwner(const JsonDbOwner *owner) { mOwner = owner; }

private:
    const JsonDbOwner *mOwner;
    JsonDb      *mJsonDb;
};

class JsonDbMapProxy : public QObject {
    Q_OBJECT
public:
    JsonDbMapProxy( const JsonDbOwner *owner, JsonDb *jsonDb, QObject *parent=0 );
    ~JsonDbMapProxy();

    Q_SCRIPTABLE void emitViewObject(const QString &key, const QJSValue &value );
    Q_SCRIPTABLE void lookup(const QString &key, const QJSValue &value, const QJSValue &context );
    Q_SCRIPTABLE void lookupWithType(const QString &key, const QJSValue &value, const QJSValue &objectType, const QJSValue &context);

    void setOwner(const JsonDbOwner *owner) { mOwner = owner; }

    void clear() { mEmitted.clear(); mLookup.clear(); mContext = QJSValue(); }
    const QMultiMap<QString,QJSValue> &emitted() { return mEmitted; }
    const QJSValue &context() { return mContext; }

 signals:
    void viewObjectEmitted(const QString &, const QJSValue &);
    void lookupRequested(const QString &, const QJSValue &, const QJSValue &, const QJSValue &);
private:
    const JsonDbOwner *mOwner;
    JsonDb      *mJsonDb;
    QMultiMap<QString,QJSValue> mEmitted;
    QMultiMap<QString,QJSValue> mLookup;
    QJSValue mContext;
};

class Console : public QObject {
    Q_OBJECT
public:
    Console();
    Q_SCRIPTABLE void log(const QString &string);
    Q_SCRIPTABLE void debug(const QString &string);
};

} } // end namespace QtAddOn::JsonDb

QT_END_HEADER

#endif