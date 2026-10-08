#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVector>

// One rubber-band box in the SAME coordinate space the kernel uses:
// PDF points, origin top-left, y-down. Serialises to exactly the
// `regions[]` element of RegionRequest, so an exported scheme file can be fed
// straight to the CLI without any reshaping.
struct RegionSpec
{
    int    page = 0;
    QString kind = QStringLiteral("global_table"); // global_table | detail_table | material_no_region
    double x = 0, y = 0, w = 0, h = 0;

    QJsonObject toJson() const;
    static RegionSpec fromJson(const QJsonObject& o);
    bool isValid() const { return w > 0 && h > 0; }
};

// A named, reusable set of box selections — the "template" the user can save,
// reuse on other drawings, edit field-by-field, re-draw, and export.
struct Scheme
{
    QString id;
    QString name;
    QString note;
    QVector<RegionSpec> regions;

    QJsonObject toJson() const;
    static Scheme fromJson(const QJsonObject& o);
};

// Persists schemes to a JSON file under the user's app-data dir so they survive
// restarts. Loading is tolerant: a missing/corrupt file yields an empty store
// rather than an error, and unknown fields are ignored (forward compatible).
class SchemeStore
{
public:
    SchemeStore();

    QString path() const { return m_path; }
    bool load();
    bool save() const;

    QVector<Scheme> schemes() const { return m_schemes; }
    int count() const { return m_schemes.size(); }

    const Scheme* find(const QString& id) const;
    Scheme* findMutable(const QString& id);
    void add(const Scheme& s);
    void update(const Scheme& s);
    bool remove(const QString& id);

    // Builds a scheme from the boxes currently drawn on the canvas.
    Scheme makeFromSelection(const QString& name, const QString& note,
                             const QVector<RegionSpec>& regions) const;
    static QString newId();
    static QString defaultStorePath();

private:
    QString m_path;
    QVector<Scheme> m_schemes;
};