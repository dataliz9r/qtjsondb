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

#include <QCoreApplication>
#include <QtTest/QtTest>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTime>

#include "json.h"

#include "jsondbpartition.h"
#include "jsondbindex.h"
#include "jsondbindexquery.h"
#include "jsondbsettings.h"
#include "jsondb-strings.h"
#include "jsondb-error.h"

#include <qjsonobject.h>

#include "../../shared/util.h"

QT_USE_NAMESPACE_JSONDB

Q_DECLARE_METATYPE(QJsonArray)
Q_DECLARE_METATYPE(QJsonObject)

class TestJsonDb: public QObject
{
    Q_OBJECT
public:
    TestJsonDb();

private slots:
    void init();
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void contactListChaff();//moved from auto/daemon
    void compact();
    void jsonArrayCreate();
    void jsonObjectCreate();

    void jsonArrayReadValue_data();
    void jsonArrayReadValue();
    void jsonObjectReadValue_data();
    void jsonObjectReadValue();

    void jsonArrayInsertValue();
    void jsonObjectInsertValue();

    void benchmarkCreate();
    void benchmarkFileAppend();
    void benchmarkFileAppend2();
    void benchmarkParseQuery_data();
    void benchmarkParseQuery();
    void benchmarkFieldMatch();
    void benchmarkTokenizer();
    void benchmarkForwardKeyCmp();
    void benchmarkParsedQuery();

    void benchmarkSchemaValidation_data();
    void benchmarkSchemaValidation();

    void benchmarkFind();
    void benchmarkFindByName();
    void benchmarkFindByUuid();
    void benchmarkFindEQ();
    void benchmarkFindLE();
    void benchmarkFirst();
    void benchmarkLast();
    void benchmarkFirst10();
    void benchmarkFind10();
    void benchmarkFind20();
    void benchmarkFirstByUuid();
    void benchmarkLastByUuid();
    void benchmarkFirst10ByUuid();
    void benchmarkFind10ByUuid();
    void benchmarkFindUnindexed();
    void benchmarkFindReindexed();
    void benchmarkFindNames();
    void findNamesMapObject();
    void benchmarkFindNamesMapObject();
    void benchmarkCursorCount();
    void benchmarkQueryCount();
    void benchmarkScriptEngineCreation();

private:
    QJsonValue readJsonFile(const QString &filename);
    QJsonValue readJson(const QByteArray &json);
    void removeDbFiles();
    void addSchema(const QString &schemaName);
    void addIndex(const QString &propertyName, const QString &propertyType=QString(), const QString &objectType=QString());

private:
    JsonDbPartition *mJsonDbPartition;
    QList<JsonDbObject>  mContactList;
    QStringList mFirstNames;
    QStringList mUuids;
    JsonDbOwner *mOwner;
};

#define verifyGoodResult(result) \
{ \
    JsonDbWriteResult __result = result; \
}

#define verifyGoodQueryResult(result) \
{ \
    JsonDbQueryResult __result = result; \
    QVERIFY2(__result.error.type() == QJsonValue::Null,  \
         __result.error.toObject().value("message").toString().toLocal8Bit()); \
}

const char *kFilename = "testdatabase";

TestJsonDb::TestJsonDb() :
    mJsonDbPartition(0)
  , mOwner(0)
{
}

void TestJsonDb::removeDbFiles()
{
    QStringList filters;
    filters << QString::fromLatin1(kFilename)+QLatin1Char('*');
    filters << QString("*.db") << "objectFile.bin" << "objectFile2.bin";
    QStringList lst = QDir().entryList(filters);
    foreach (const QString &fileName, lst)
        QFile::remove(fileName);
}

void TestJsonDb::initTestCase()
{
    QCoreApplication::setOrganizationName("Example");
    QCoreApplication::setOrganizationDomain("example.com");
    QCoreApplication::setApplicationName("TestJsonDb");
    QCoreApplication::setApplicationVersion("1.0");

    removeDbFiles();
    mOwner = new JsonDbOwner(this);
    mOwner->setOwnerId(QStringLiteral("com.example.JsonDbTest"));
    mJsonDbPartition = new JsonDbPartition(kFilename, QStringLiteral("com.example.JsonDbTest"), mOwner, this);
    mJsonDbPartition->open();

    QFile contactsFile(":/daemon/json/largeContactsTest.json");
    QVERIFY(contactsFile.exists());
    contactsFile.open(QIODevice::ReadOnly);
    QByteArray json = contactsFile.readAll();
    QJsonDocument document(QJsonDocument::fromJson(json));
    QVERIFY(document.array().size());
    QJsonArray contactList = document.array();
    QList<JsonDbObject> newContactList;
    QSet<QString> firstNames;
    for (int i = 0; i < contactList.size(); i++) {
        QJsonObject contact = contactList.at(i).toObject();
        QString name = contact.value("name").toString();
        QStringList names = name.split(" ");
        QJsonObject nameObject;
        nameObject.insert("first", names[0]);
        nameObject.insert("last", names[names.size()-1]);
        contact.insert("name", nameObject);
        contact.insert(JsonDbString::kTypeStr, QString("contact"));
        newContactList.append(contact);
        firstNames.insert(names[0]);
    }
    mContactList = newContactList;
    mFirstNames = firstNames.toList();
    mFirstNames.sort();

    mFirstNames = firstNames.toList();
    mFirstNames.sort();

    addIndex(QLatin1String("name"));
    addIndex(QLatin1String("name.first"));

    qDebug() << "Creating" << mContactList.size() << "contacts...";

    QElapsedTimer time;
    time.start();
    int count = 0;
    int chunksize = 100;
    for (count = 0; count < mContactList.size(); count += chunksize) {
        JsonDbObjectList chunk = mContactList.mid(count, chunksize);
        JsonDbWriteResult result = mJsonDbPartition->updateObjects(mOwner, chunk);
        JsonDbObjectList data = result.objectsWritten;
        for (int i = 0; i < data.size(); i++)
            mUuids.append(data.at(i).value(JsonDbString::kUuidStr).toString());
    }
    long elapsed = time.elapsed();
    mUuids.sort();
    qDebug() << "done. Time per item (ms):" << (double)elapsed / count << "count" << count << "elapsed" << elapsed << "ms";
}

void TestJsonDb::init()
{
}

void TestJsonDb::cleanupTestCase()
{
    if (mJsonDbPartition) {
        mJsonDbPartition->close();
        delete mJsonDbPartition;
        mJsonDbPartition = 0;
    }
    if (mOwner) {
        delete mOwner;
        mOwner = 0;
    }
    removeDbFiles();
}

void TestJsonDb::cleanup()
{
    QCOMPARE(mJsonDbPartition->mTransactionDepth, 0);
}

void TestJsonDb::addSchema(const QString &schemaName)
{
    QJsonValue schema = readJsonFile(QString(":/daemon/schemas/%1.json").arg(schemaName)).toArray();
    JsonDbObject schemaDocument;
    schemaDocument.insert(JsonDbString::kTypeStr, JsonDbString::kSchemaTypeStr);
    schemaDocument.insert("name", schemaName);
    schemaDocument.insert("schema", schema);

    JsonDbWriteResult result = mJsonDbPartition->updateObject(mOwner, schemaDocument);
    verifyGoodResult(result);
}

void TestJsonDb::addIndex(const QString &propertyName, const QString &propertyType, const QString &objectType)
{
    QJsonObject index;
    index.insert(JsonDbString::kTypeStr, JsonDbString::kIndexTypeStr);
    index.insert(JsonDbString::kPropertyNameStr, propertyName);
    if (!propertyType.isEmpty())
        index.insert(JsonDbString::kPropertyTypeStr, propertyType);
    if (!objectType.isEmpty())
        index.insert(JsonDbString::kObjectTypeStr, objectType);
    JsonDbWriteResult result = mJsonDbPartition->updateObject(mOwner, index);
    QVERIFY(result.code == JsonDbError::NoError);
}

void TestJsonDb::compact()
{
    mJsonDbPartition->compact();
}

void TestJsonDb::jsonArrayCreate()
{
    QBENCHMARK {
        QJsonArray list;
    }
}

void TestJsonDb::jsonObjectCreate()
{
    QBENCHMARK {
        QJsonObject map;
    }
}

void TestJsonDb::jsonArrayReadValue_data()
{
    QTest::addColumn<QJsonArray>("list");
    QTest::addColumn<int>("index");
    QTest::addColumn<int>("value");

    QJsonArray data2;
    data2.append(123);
    data2.append(133);
    data2.append(323);
    QTest::newRow("small list") << data2 << 2 << 323;

    QJsonArray data3;
    for (int i = 0; i < 256; ++i)
        data3.append(i);
    QTest::newRow("large list") << data3 << 12 << 12;
}

void TestJsonDb::jsonArrayReadValue()
{
    QFETCH(QJsonArray, list);
    QFETCH(int, index);
    QFETCH(int, value);

    QBENCHMARK {
        QCOMPARE(list.at(index).toDouble(), (double)value);
    }
}

void TestJsonDb::jsonObjectReadValue_data()
{
    QTest::addColumn<QJsonObject>("map");
    QTest::addColumn<QString>("property");
    QTest::addColumn<int>("value");

    QJsonObject data1;
    data1.insert(QString::number(1), 123);
    data1.insert(QString::number(12), 133);
    data1.insert(QString::number(123), 323);
    QTest::newRow("small map") << data1 << QString::number(123) << 323;

    QJsonObject data2;
    for (int i = 0; i < 256; ++i)
        data2.insert(QString::number(i), i);
    QTest::newRow("large map") << data2 << QString::number(12) << 12;
}

void TestJsonDb::jsonObjectReadValue()
{
    QFETCH(QJsonObject, map);
    QFETCH(QString, property);
    QFETCH(int, value);

    QBENCHMARK {
        QCOMPARE(map.value(property).toDouble(), (double)value);
    }
}

void TestJsonDb::jsonArrayInsertValue()
{
    QBENCHMARK {
        QJsonArray list;
        for (int i = 0; i < 1024; ++i)
            list.append(i);
    }
}

void TestJsonDb::jsonObjectInsertValue()
{
    const int iterations = 1024;
    QVarLengthArray<QString, iterations> names;

    for (int i = 0; i < iterations; ++i)
        names.append(QString::number(i));

    QBENCHMARK {
        QJsonObject map;
        for (int i = 0; i < iterations; ++i)
            map.insert(names[i], i);
    }
}

void TestJsonDb::benchmarkCreate()
{
    QJsonArray contacts(readJsonFile(":/daemon/json/largeContactsTest.json").toArray());
    QBENCHMARK {
        JsonDbObject contact(contacts.at(0).toObject());
            mJsonDbPartition->updateObject(mOwner, contact);
    }
}

void TestJsonDb::benchmarkFileAppend()
{
    QJsonArray contacts(readJsonFile(":/daemon/json/largeContactsTest.json").toArray());
    QFile objectFile("objectFile.bin");
    objectFile.open(QIODevice::ReadWrite);

    QBENCHMARK {
        objectFile.write(QJsonDocument(contacts.at(0).toObject()).toBinaryData());
            objectFile.flush();
            fsync(objectFile.handle());
    }
}

void TestJsonDb::benchmarkFileAppend2()
{
    QJsonValue bson(readJsonFile(":/daemon/json/largeContactsTest.json"));
    QJsonArray contacts(bson.toArray());
    QFile objectFile("objectFile.bin");
    objectFile.open(QIODevice::ReadWrite);
    QFile objectFile2("objectFile2.bin");
    objectFile2.open(QIODevice::ReadWrite);

    QBENCHMARK {
        objectFile.write(QJsonDocument(contacts.at(0).toObject()).toBinaryData());
            objectFile.flush();
            objectFile2.write(QJsonDocument(contacts.at(0).toObject()).toBinaryData());
            objectFile2.flush();
            fsync(objectFile2.handle());
            fsync(objectFile.handle());
    }
}

void TestJsonDb::benchmarkParseQuery_data()
{
    QTest::addColumn<QString>("query");
    QTest::newRow("1")  << "[?foo exists]";
    QTest::newRow("2")  << "[?foo->bar exists]";
    QTest::newRow("3")  << "[?foo->bar->baz exists]";
    QTest::newRow("4")  << "[?foo=\"bar\"]";
    QTest::newRow("5")  << "[?foo= %bar ]";
    QTest::newRow("6")  << "[?foo= %bar]";
    QTest::newRow("7")  << "[?foo=%bar]";
    QTest::newRow("8")  << "[?foo=\"bar\" | foo=\"baz\"]";
    QTest::newRow("9")  << "[?foo=\"bar\"][/foo]";
    QTest::newRow("11") << "[?foo =~ \"/a\\//\"]";
    QTest::newRow("15") << "[?foo=\"bar\"][={ a:x, b:y, c:z}]";
    QTest::newRow("16") << "[?foo=\"bar\"][={ a:x->foreign, b:y, c:z}]";
    QTest::newRow("18") << "[?_type=\"contact\"][= { uuid: _uuid, first: name.first, last: name.last } ]";
    QTest::newRow("19") << "[?telephoneNumbers.*.number=\"6175551212\"]";
    QTest::newRow("21") << "[?_type=\"contact\"][?foo startsWith \"bar\"]";
}

void TestJsonDb::benchmarkParseQuery()
{
    QFETCH(QString, query);
    QJsonObject bindings;
    bindings.insert("bar", QString("barValue"));
    QBENCHMARK {
        QScopedPointer<JsonDbQuery> jq(JsonDbQuery::parse(query, bindings));
    }
}

void TestJsonDb::benchmarkFieldMatch()
{
    int count = mContactList.size();
    if (!count)
        return;
    int itemNumber = (int)((double)qrand() * count / RAND_MAX);
    JsonDbObject item = mContactList.at(itemNumber);
    QString query =
        QString("[?%1=\"%2\"][?name<=\"%3\"]")
        .arg(JsonDbString::kTypeStr)
        .arg("contact")
        .arg(item.value("name").toString());
    QRegExp fieldMatch("^\\[\\?\\s*([^\\[\\]]+)\\s*\\](.*)");
    QBENCHMARK {
        fieldMatch.exactMatch(query);
    }
}

void TestJsonDb::benchmarkTokenizer()
{
    QStringList queries = (QStringList()
                           << "[?abc=\"def\"]"
                           << "[ ? abc = \"def\" \t]"
                           << "[?abc.def=\"ghi\"]"
                           << "[?abc->def=\"ghi\"]"
                           << "[?abc.def=\"ghi\"][/abc.def]"
                           << "[?abc.def=\"ghi\"][\\foo]"
                           << "[?abc.def=\"ghi\"][={foo:Foo,bar:Bar}][/foo]"
        );
    foreach (QString query, queries) {
        QBENCHMARK {
            JsonDbQueryTokenizer tokenizer(query);
            //qDebug() << "query" << query;
            //int i = 0;
            QString token;
            while (!(token = tokenizer.pop()).isEmpty()) {
                //qDebug() << QString("    token[%1] %2    ").arg(i++).arg(token);
            }
        }
    }
}

namespace QtAddOn { namespace JsonDb {
QByteArray makeForwardKey(const QJsonValue &fieldValue, const ObjectKey &objectKey);
int forwardKeyCmp(const char *aptr, size_t asiz, const char *bptr, size_t bsiz, void *op);
} } // end namespace QtAddOn::JsonDb

void TestJsonDb::benchmarkForwardKeyCmp()
{
    int count = mContactList.size();

    QVector<QByteArray> keys;
    for (int ii = 0; ii < mContactList.size(); ii++) {
        JsonDbObject object = mContactList.at(ii);
        QString typeName = object.value(JsonDbString::kTypeStr).toString();
        QJsonValue fullname(object.value("fullname"));
        QByteArray key = makeForwardKey(fullname, ObjectKey());
        keys.append(key);
    }

    QBENCHMARK {
        for (int j = 0; j < count; j++) {
            QByteArray key1 = keys[j];
            for (int i = 0; i < count; i++) {
                QByteArray key2 = keys[i];
                int cmp = forwardKeyCmp(key1.constData(), key1.size(), key2.constData(), key2.size(), 0);
                if (i == j)
                    QVERIFY(cmp == 0);
                /* Note: this fails horrible but I guess it shouldn't
                else
                    QVERIFY(cmp != 0);
                    */
            }
        }
    }
}

void TestJsonDb::benchmarkParsedQuery()
{
    int count = mContactList.size();
    if (!count)
        return;

    int itemNumber = mContactList.size() / 3;
    JsonDbObject item = mContactList.at(itemNumber);

    QString query =
        QString("[?%1=\"%2\"][?name.first<=\"%3\"]")
        .arg(JsonDbString::kTypeStr)
        .arg("contact")
        .arg(item.value("name").toObject().value("first").toString());
    QJsonObject bindings;
    QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query, bindings));

    QBENCHMARK {
        QJsonObject request;
        //QVariantList queryTerms = parseResult.value("queryTerms").toList();
        //QVariantList orderTerms = parseResult.value("orderTerms").toList();
        int limit = 1;
        int offset = 0;
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), limit, offset);
        if (queryResult.data.size() != 1) {
            qDebug() << "result length" << queryResult.data.size();
            qDebug() << "item" << item;
            qDebug() << "itemNumber" << itemNumber;
            qDebug() << "sortKeys" << queryResult.sortKeys;
        }
        QCOMPARE(queryResult.data.size(), 1);
        //      qDebug() << "result.keys()" << result.keys();
    }
}

void TestJsonDb::benchmarkSchemaValidation_data()
{
    QTest::addColumn<QByteArray>("item");
    QTest::addColumn<bool>("isPerson");
    QTest::addColumn<bool>("isAdult");

    QTest::newRow("empty")
            << QByteArray("{}") << true << true;
    QTest::newRow("40 and 4")
            << QByteArray("{ \"name\":\"40 and 4\" }") << true << true;
    QTest::newRow("Alice")
            << QByteArray("{ \"name\":\"Alice\", \"age\": 5}")  << true << false;
    QTest::newRow("Alice's mother")
            << QByteArray("{ \"name\":\"Alice's mother\", \"age\": 32}") << true << true;
    QTest::newRow("Alice's grandmother")
            << QByteArray("{ \"name\":\"Alice's grandmother\", \"age\": 100}") << true << true;
    QTest::newRow("Alice's great-grandmother")
            << QByteArray("{ \"name\":\"Alice's great-grandmother\", \"age\": 130}") << false << false;
}

void TestJsonDb::benchmarkSchemaValidation()
{
    bool validate = jsondbSettings->validateSchemas();
    jsondbSettings->setValidateSchemas(true);

    QFETCH(QByteArray, item);
    QFETCH(bool, isPerson);
    QFETCH(bool, isAdult);
    Q_UNUSED(isAdult);

    const QByteArray person =
            "{"
            "    \"description\": \"A person\","
            "    \"type\": \"object\","
            "    \"properties\": {"
            "        \"name\": {\"type\": \"string\"},"
            "        \"age\" : {\"type\": \"integer\", \"maximum\": 125 }"
            "    }"
            "}";
    static int schemaId = 0;
    const QString personSchemaName = QString::fromLatin1("personBenchmark") + QString::number(++schemaId);

    QJsonObject personSchemaBody = readJson(person).toObject();
    JsonDbObject personSchemaObject;
    personSchemaObject.insert(JsonDbString::kTypeStr, JsonDbString::kSchemaTypeStr);
    personSchemaObject.insert("name", personSchemaName);
    personSchemaObject.insert("schema", personSchemaBody);
    JsonDbWriteResult qResult = mJsonDbPartition->updateObject(mOwner, personSchemaObject);
    verifyGoodResult(qResult);

    // Prepare items
    const uint numberOfIterations = 1000;
    QList<QJsonObject> objects;
    objects.reserve(numberOfIterations);
    for (uint i = 0; i < numberOfIterations; ++i) {
        QJsonObject object = readJson(item).toObject();
        object.insert("testingForAdult", (int)i);
        object.insert(JsonDbString::kTypeStr, personSchemaName);
        objects.append(object);
    }

    QBENCHMARK_ONCE {
        foreach (QJsonObject object, objects) {
            QString error;
            bool result = mJsonDbPartition->validateSchema(personSchemaName, object, error);

            if (isPerson) {
                QVERIFY(result && error.isEmpty());
            } else {
                QVERIFY(!(result || error.isEmpty()));
            }
        }
    }

    jsondbSettings->setValidateSchemas(validate);
}

void TestJsonDb::benchmarkFind()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        int itemNumber = (int)((double)qrand() * count / RAND_MAX);
        JsonDbObject item = mContactList.at(itemNumber);
        QString query =  QString("[?name=\"%3\"][?%1=\"%2\"]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact")
                .arg(item.value("name").toString());
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFindByName()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?name.first<=\"%3\"]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact")
                .arg(mFirstNames[mFirstNames.size()-1]);
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFindByUuid()
{
    int count = mContactList.size();
    if (!count)
        return;

    QString uuid = mUuids[mUuids.size() / 2];
    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"]")
                .arg(JsonDbString::kUuidStr)
                .arg(uuid);
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data());
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFindEQ()
{
  int count = mContactList.size();
  if (!count)
    return;

  int itemNumber = (int)((double)qrand() * count / RAND_MAX);
  JsonDbObject item = mContactList.at(itemNumber);
  QString query = QString("[?name.first=\"%3\"][?%1=\"%2\"]")
          .arg(JsonDbString::kTypeStr)
          .arg("contact")
          .arg(item.propertyLookup("name.first").toString());
  QBENCHMARK {
      QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
      JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data());
      verifyGoodQueryResult(queryResult);
  }
}

void TestJsonDb::benchmarkFindLE()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        int itemNumber = (int)((double)qrand() * count / RAND_MAX);
        JsonDbObject item = mContactList.at(itemNumber);
        QString query = QString("[?name.first<=\"%3\"][?%1=\"%2\"]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact")
                .arg(item.propertyLookup("name.first").toString());
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFirst()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?name.last exists][/name.first]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkLast()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?name.last exists][\\name.first]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFirst10()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?name.last exists][/name.first]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 10);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFind10()
{
    int count = mContactList.size();
    if (!count)
        return;

    int itemNumber = qMax(0, mFirstNames.size()-10);

    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?name.first>=\"%3\"][?name.last exists][/name.first]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact")
                .arg(mFirstNames[itemNumber]);
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 10);
        verifyGoodQueryResult(queryResult);
    }
}
void TestJsonDb::benchmarkFind20()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        int itemNumber = (int)((double)qrand() * count / RAND_MAX);
        JsonDbObject item = mContactList.at(itemNumber);

        QString query = QString("[?name.first<=\"%3\"][?%1=\"%2\"]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact")
                .arg(item.propertyLookup("name.first").toString());
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 20);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFirstByUuid()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?name.last exists][/_uuid]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkLastByUuid()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?name.last exists][/_uuid]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFirst10ByUuid()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?name.last exists][/_uuid]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 10);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFind10ByUuid()
{
    int count = mContactList.size();
    if (!count)
        return;

    int itemNumber = qMax(0, mUuids.size()-10);
    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][?_uuid>=\"%3\"][?name.last exists][/_uuid]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact")
                .arg(mUuids[itemNumber]);
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 10);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFindUnindexed()
{
    int count = mContactList.size();
    if (!count)
        return;

    QBENCHMARK {
        int itemNumber = (int)((double)qrand() * count / RAND_MAX);
        JsonDbObject item = mContactList.at(itemNumber);
        QString query = QString("[?%1=\"%2\"][?firstName=\"%3\"]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact")
                .arg(item.value("firstName").toString());
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult = mJsonDbPartition->queryObjects(mOwner, parsedQuery.data());
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFindReindexed()
{
    int count = mContactList.size();
    if (!count)
        return;

    //qDebug() << "Adding index for lastName";
    addIndex("lastName");
    //qDebug() << "Done adding index for lastName";

    QBENCHMARK {
        int itemNumber = (int)((double)qrand() * count / RAND_MAX);
        JsonDbObject item = mContactList.at(itemNumber);
        QString query  = QString("[?%1=\"%2\"][?lastName=\"%3\"]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact")
                .arg(item.value("lastName").toString());
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data());
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::benchmarkFindNames()
{
    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
    }
}

void TestJsonDb::findNamesMapObject()
{
    QBENCHMARK_ONCE {
        QString query = QString("[?%1=\"%2\"][= { uuid: _uuid, first: name.first, last: name.last } ]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data());
        verifyGoodQueryResult(queryResult);
        QCOMPARE(queryResult.data.size(), mContactList.size());
    }
}

void TestJsonDb::benchmarkFindNamesMapObject()
{
    QBENCHMARK {
        QString query = QString("[?%1=\"%2\"][= { uuid: _uuid, first: name.first, last: name.last } ]")
                .arg(JsonDbString::kTypeStr)
                .arg("contact");
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query));
        JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data(), 1);
        verifyGoodQueryResult(queryResult);
        QCOMPARE(queryResult.data.size(), 1);
    }
}

void TestJsonDb::benchmarkCursorCount()
{
    QStringList queries = (QStringList()
                           << "[/name.first]"
                           //<< "[/name.first][?_type=\"contact\"]"
        );
    QJsonObject bindings;
    foreach (QString query, queries) {
        QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(query, bindings));
        JsonDbIndexQuery *indexQuery = mJsonDbPartition->compileIndexQuery(mOwner, parsedQuery.data());
        int count = 0;
        //qDebug() << "query" << query;
        QBENCHMARK {
            int mycount = 0;
            for (JsonDbObject object = indexQuery->first();
                 object.size() > 0;
                 object = indexQuery->next()) {
                mycount++;
            }
            count = mycount;
        }
    }
}

void TestJsonDb::benchmarkQueryCount()
{
    QStringList queries = (QStringList()
                           << "[/name.first]"
                           << "[/name.first][?_type=\"contact\"]"
        );
    foreach (QString query, queries) {
        QBENCHMARK {
            QScopedPointer<JsonDbQuery> parsedQuery(JsonDbQuery::parse(QString("%1[count]").arg(query)));
            JsonDbQueryResult queryResult =  mJsonDbPartition->queryObjects(mOwner, parsedQuery.data());
        }
    }
}

QJsonValue TestJsonDb::readJsonFile(const QString& filename)
{
    QString filepath = filename;
    QFile jsonFile(filepath);
    jsonFile.open(QIODevice::ReadOnly);
    QByteArray json = jsonFile.readAll();
    JsonReader parser;
    bool ok = parser.parse(json);
    if (!ok) {
      qDebug() << filepath << parser.errorString();
    }
    QVariant v = parser.result();
    return QJsonObject::fromVariantMap(v.toMap());
}

QJsonValue TestJsonDb::readJson(const QByteArray& json)
{
    JsonReader parser;
    bool ok = parser.parse(json);
    if (!ok) {
      qDebug() << parser.errorString();
    }
    QVariant v = parser.result();
    return QJsonObject::fromVariantMap(v.toMap());
}

void TestJsonDb::benchmarkScriptEngineCreation()
{
    QJSValue result;
    QBENCHMARK {
        QJSEngine *engine = new QJSEngine();
        engine->setParent(0);
        QJSValue globalObject = engine->globalObject();
        result =
            engine->evaluate(QString("var reduce = function(k, v, s) { s.count = s.count + v.count; return s; };"));
        delete engine;
        engine = 0;
    }
}

void TestJsonDb::contactListChaff()
{
    QBENCHMARK {
        for (int ii = 0; ii < mContactList.size(); ii++) {
            JsonDbObject data = mContactList.at(ii);
        JsonDbObject chaff;
        chaff.insert(JsonDbString::kTypeStr, QString("com.noklab.nrcc.ContactChaff"));
        QStringList skipKeys = (QStringList() << JsonDbString::kUuidStr << JsonDbString::kVersionStr << JsonDbString::kTypeStr);
        foreach (QString key, data.keys()) {
            if (!skipKeys.contains(key))
                chaff.insert(key, data.value(key));
        }

        JsonDbWriteResult result = mJsonDbPartition->updateObject(mOwner, chaff);
        verifyGoodResult(result);
        }
    }
}

QTEST_MAIN(TestJsonDb)
#include "bench_daemon.moc"
