/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the examples of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** You may use this file under the terms of the BSD license as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of Digia Plc and its Subsidiary(-ies) nor the names
**     of its contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

import QtQuick 2.0
import QtJsonDb 1.0 as JsonDb

Rectangle {
    width: 360
    height: 360

    property int fontsize: 20

    JsonDb.Partition {
        id: systemPartition
    }

    function createCallback(error, response)
    {
        if (error) {
            console.log("Error " + error.code + " " + error.message);
            return;
        }
    }

//! [Creating a Simple Index Object]
    function createIdentifierIndex(cb)
    {
        // declares an index on the "identifier" property of any object
        var indexDefinition = {
            "_type": "Index",
            "name": "identifier",
            "propertyName": "identifier",
            "propertyType": "string"
        };
        systemPartition.create(indexDefinition, createCallback);
    }
//! [Creating a Simple Index Object]

//! [Creating an Index Using a Property Function]
    function create(cb)
    {
        // declares an index on the "identifier" property of any object
        var indexDefinition = {
            "_type": "Index",
            "name": "identifierUppercase",
            "propertyFunction": (function (o) {
                if (o.identifier) {
                    var id = o.identifier;
                    jsondb.emit(id.toUpperCase());
                }
            }).toString(),
            "propertyType": "string"
        };
        systemPartition.create(indexDefinition, createCallback);
    }
//! [Creating an Index Using a Property Function]

    function createCollationIndex(cb)
    {
        var indexDefinition = {
            "_type": "Index",
            "name": "identifier",
            "propertyName": "identifier",
            "propertyType": "string",
            "locale" : "zh_CN",
            "collation" : "pinyin"
        };
        systemPartition.create(indexDefinition, createCallback);
    }

    Component.onCompleted: {
        createIdentifierIndex(createCallback)
        console.log("The simple index was created!")
        createCollationIndex(createCallback)
        console.log("The collation index was created!")
        create(createCallback)
        console.log("The property function index was created!")
    }
}
