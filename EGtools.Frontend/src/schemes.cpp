#include "schemes.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QUuid>

namespace {
const char* kKindGlobal = "global_table";
const char* kKindDetail = "detail_table";
const char* kKindPipe   = "material_no_region";
} // namespace

// ---------------------------------------------------------------- RegionSpec

QJsonObject RegionSpec::toJson() const
{
    QJsonObject bbox;
    bbox.insert(QStringLiteral("x"), x);
    bbox.insert(QStringLiteral("y"), y);
    bbox.insert(QStringLiteral("width"), w);
    bbox.insert(QStringLiteral("height"), h);

    QJsonObject o;
    o.insert(QStringLiteral("page"), page);
    o.insert(QStringLiteral("kind"), kind);
    o.insert(QStringLiteral("bbox"), bbox);
    return o;
}

RegionSpec RegionSpec::fromJson(const QJsonObject& o)
{
    RegionSpec r;
    r.page = o.value(QStringLiteral("page")).toInt();
    r.kind = o.value(QStringLiteral("kind")).toString(QString::fromLatin1(kKindDetail));
    const QJsonObject b = o.value(QStringLiteral("bbox")).toObject();
    r.x = b.value(QStringLiteral("x")).toDouble();
    r.y = b.value(QStringLiteral("y")).toDouble();
    r.w = b.value(QStringLiteral("width")).toDouble();
    r.h = b.value(QStringLiteral("height")).toDouble();
    return r;
}

// -------------------------------------------------------------------- Scheme

QJsonObject Scheme::toJson() const
{
    QJsonArray regs;
    for (const RegionSpec& r : regions) regs.append(r.toJson());

    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("note"), note);
    o.insert(QStringLiteral("regions"), regs);
    return o;
}

Scheme Scheme::fromJson(const QJsonObject& o)
{
    Scheme s;
    s.id    = o.value(QStringLiteral("id")).toString();
    s.name  = o.value(QStringLiteral("name")).toString();
    s.note  = o.value(QStringLiteral("note")).toString();
    const QJsonArray regs = o.value(QStringLiteral("regions")).toArray();
    for (const QJsonValue& v : regs) s.regions.append(RegionSpec::fromJson(v.toObject()));
    return s;
}

// ---------------------------------------------------------------- SchemeStore

QString SchemeStore::defaultStorePath()
{
    QString dir = QDir::homePath() + QStringLiteral("/Documents/EGtools");
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("schemes.json"));
}

SchemeStore::SchemeStore() : m_path(defaultStorePath()) {}

QString SchemeStore::newId()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

bool SchemeStore::load()
{
    m_schemes.clear();
    QFile f(m_path);
    if (!f.exists()) return true;               // first run: empty store, not an error
    if (!f.open(QIODevice::ReadOnly)) return false;

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return false;

    const QJsonArray arr = doc.object().value(QStringLiteral("schemes")).toArray();
    for (const QJsonValue& v : arr) {
        Scheme s = Scheme::fromJson(v.toObject());
        if (s.id.isEmpty()) s.id = newId();
        if (s.name.isEmpty()) s.name = QObject::tr("未命名方案");
        m_schemes.append(s);
    }
    return true;
}

bool SchemeStore::save() const
{
    QJsonArray arr;
    for (const Scheme& s : m_schemes) arr.append(s.toJson());

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("schemes"), arr);

    // QSaveFile = write-to-temp + atomic rename, so an interrupted save can
    // never leave a half-written schemes.json behind.
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

const Scheme* SchemeStore::find(const QString& id) const
{
    for (const Scheme& s : m_schemes) if (s.id == id) return &s;
    return nullptr;
}

Scheme* SchemeStore::findMutable(const QString& id)
{
    for (Scheme& s : m_schemes) if (s.id == id) return &s;
    return nullptr;
}

void SchemeStore::add(const Scheme& s) { m_schemes.append(s); }

void SchemeStore::update(const Scheme& s)
{
    for (Scheme& e : m_schemes) if (e.id == s.id) { e = s; return; }
    m_schemes.append(s);
}

bool SchemeStore::remove(const QString& id)
{
    for (int i = 0; i < m_schemes.size(); ++i) {
        if (m_schemes[i].id == id) { m_schemes.remove(i); return true; }
    }
    return false;
}

Scheme SchemeStore::makeFromSelection(const QString& name, const QString& note,
                                      const QVector<RegionSpec>& regions) const
{
    Scheme s;
    s.id      = newId();
    s.name    = name;
    s.note    = note;
    s.regions = regions;
    return s;
}