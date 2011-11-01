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

#ifndef QBTREEDATA_H
#define QBTREEDATA_H

#include <QByteArray>

struct btval;

class QBtreeTxn;
class QBtreeCursor;

class QBtreeData
{
public:
    QBtreeData() : mData(0), mPage(0), mSize(0) { }
    QBtreeData(const QByteArray &array);
    QBtreeData(const QBtreeData &other);
    ~QBtreeData();

    QBtreeData &operator=(const QBtreeData &other);

    inline bool isNull() const { return mData == 0; }

    inline const char *constData() const { return mData; }
    inline int size() const { return mSize; }

    QByteArray toByteArray() const;

    static QBtreeData fromRawData(const char *data, int size)
    {
        QBtreeData bv;
        bv.mData = data;
        bv.mSize = size;
        return bv;
    }

private:
    QByteArray mByteArray;
    const char *mData;
    void *mPage; // struct mpage *
    int mSize;

    QBtreeData(struct btval *);
    int ref();
    int deref();
    friend class QBtreeTxn;
    friend class QBtreeCursor;
};

#endif // QBTREEDATA_H