#include "charsegmentpanel.h"
#include "clirunner.h"
#include "kernelclient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

CharSegmentPanel::CharSegmentPanel(QWidget* parent) : QWidget(parent)
{
    auto* title = new QLabel(QStringLiteral("字符段（导出列集）"));
    title->setStyleSheet(QStringLiteral("QLabel{font-weight:600;font-size:13px;}"));

    m_hint = new QLabel(QStringLiteral("这些是导出 Excel 的列。默认内置一套；可新增 / 改名 / 删除 / 重排。"));
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QStringLiteral("QLabel{color:#666;font-size:11px;}"));

    m_list = new QListWidget();
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);

    m_addBtn  = new QPushButton(QStringLiteral("新增"));
    m_delBtn  = new QPushButton(QStringLiteral("删除"));
    m_upBtn   = new QPushButton(QStringLiteral("上移"));
    m_downBtn = new QPushButton(QStringLiteral("下移"));
    m_saveBtn = new QPushButton(QStringLiteral("保存到 config.json"));
    m_resetBtn= new QPushButton(QStringLiteral("恢复默认"));
    m_resetBtn->setStyleSheet(QStringLiteral("QPushButton{background:#b04a2f;color:#fff;padding:4px 10px;border-radius:4px;}"));

    auto* row1 = new QHBoxLayout();
    row1->addWidget(m_addBtn);
    row1->addWidget(m_delBtn);
    row1->addWidget(m_upBtn);
    row1->addWidget(m_downBtn);
    row1->addStretch();

    auto* row2 = new QHBoxLayout();
    row2->addWidget(m_saveBtn);
    row2->addWidget(m_resetBtn);
    row2->addStretch();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->addWidget(title);
    lay->addWidget(m_hint);
    lay->addWidget(m_list, 1);
    lay->addLayout(row1);
    lay->addLayout(row2);

    connect(m_addBtn,  &QPushButton::clicked, this, &CharSegmentPanel::onAdd);
    connect(m_delBtn,  &QPushButton::clicked, this, &CharSegmentPanel::onRemove);
    connect(m_upBtn,   &QPushButton::clicked, this, &CharSegmentPanel::onUp);
    connect(m_downBtn, &QPushButton::clicked, this, &CharSegmentPanel::onDown);
    connect(m_saveBtn, &QPushButton::clicked, this, &CharSegmentPanel::onSave);
    connect(m_resetBtn,&QPushButton::clicked, this, &CharSegmentPanel::onRestoreDefault);
    connect(m_list, &QListWidget::itemChanged, this, [](QListWidgetItem*){
        // editing handled inline; nothing else needed here
    });
}

QString CharSegmentPanel::cliExe(const QString& name) const
{
    const QString exe = name + QStringLiteral(".exe");
    QStringList cands;
    auto add = [&](const QString& d){ if (!d.isEmpty()) cands << QDir(d).filePath(exe); };
    add(m_cliDir);
    const QString appDir = QCoreApplication::applicationDirPath();
    add(appDir);
    add(QDir(appDir).filePath(QStringLiteral("cli")));
    add(QDir(appDir).filePath(QStringLiteral("..")));
    add(QDir(appDir).filePath(QStringLiteral("../cli")));
    for (const QString& c : cands) if (QFileInfo::exists(c)) return c;
    return cands.isEmpty() ? exe : cands.first();
}

QString CharSegmentPanel::configPath() const
{
    // The CLI loads config.json from its own directory first.
    QString cand = QDir(m_cliDir).filePath(QStringLiteral("config.json"));
    if (!m_cliDir.isEmpty() && QFileInfo::exists(cand)) return cand;
    const QString appDir = QCoreApplication::applicationDirPath();
    QString appCand = QDir(appDir).filePath(QStringLiteral("config.json"));
    if (QFileInfo::exists(appCand)) return appCand;
    return cand;
}

QStringList CharSegmentPanel::readHeaders() const
{
    QString p = configPath();
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return QStringList();
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close();
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return QStringList();
    QStringList out;
    for (const QJsonValue& v : doc.object().value(QStringLiteral("headers")).toArray())
        out.append(v.toString());
    return out;
}

bool CharSegmentPanel::writeHeaders(const QStringList& headers)
{
    QString p = configPath();
    QJsonObject root;
    QFile f(p);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError pe{};
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
        f.close();
        if (pe.error == QJsonParseError::NoError && doc.isObject())
            root = doc.object();
    }
    QJsonArray arr;
    for (const QString& h : headers) arr.append(h);
    root.insert(QStringLiteral("headers"), arr);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit statusMessage(QStringLiteral("字符段保存失败：无法写入 %1").arg(p));
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

void CharSegmentPanel::reload()
{
    m_list->clear();
    QStringList hs = readHeaders();
    if (hs.isEmpty()) {
        // Fall back to the built-in default so the user always sees something.
        hs = {QStringLiteral("pipe no"), QStringLiteral("TABLE"), QStringLiteral("NO"),
              QStringLiteral("type"), QStringLiteral("DN(mm)"), QStringLiteral("项目/DESCRIPTION"),
              QStringLiteral("QTY"), QStringLiteral("WEIGHT(kg)"), QStringLiteral("PAGE")};
    }
    for (const QString& h : hs) {
        auto* item = new QListWidgetItem(h);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        m_list->addItem(item);
    }
}

void CharSegmentPanel::onAdd()
{
    auto* item = new QListWidgetItem(QStringLiteral("新列名"));
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    m_list->addItem(item);
    m_list->setCurrentItem(item);
    m_list->editItem(item);
}

void CharSegmentPanel::onRemove()
{
    QListWidgetItem* it = m_list->currentItem();
    if (it) delete m_list->takeItem(m_list->row(it));
}

void CharSegmentPanel::onUp()
{
    int r = m_list->currentRow();
    if (r > 0) { m_list->insertItem(r - 1, m_list->takeItem(r)); m_list->setCurrentRow(r - 1); }
}

void CharSegmentPanel::onDown()
{
    int r = m_list->currentRow();
    if (r >= 0 && r < m_list->count() - 1) {
        m_list->insertItem(r + 1, m_list->takeItem(r));
        m_list->setCurrentRow(r + 1);
    }
}

void CharSegmentPanel::onRestoreDefault()
{
    if (m_cliDir.isEmpty()) {
        emit statusMessage(QStringLiteral("请先设置 CLI 目录，才能恢复默认字符段"));
        return;
    }
    // 走融合层（纯参数调用）：内嵌/命令行两条路都支持。
    const KernelClient::Result kr = KernelClient::run(
        QStringLiteral("EGpdf2excel"), {QStringLiteral("--reset-config")}, QString(),
        KernelClient::backendFromString(m_kernelBackend));
    const QString out = kr.out, err = kr.err;
    const int rc = kr.rc;
    emit statusMessage(QStringLiteral("[恢复默认/%1] rc=%2 %3").arg(kr.backend).arg(rc).arg(err.trimmed()));
    if (rc == 0) {
        reload();
        QMessageBox::information(this, QStringLiteral("恢复默认"),
            QStringLiteral("已写回内置默认 config.json（字符段恢复为默认列集）。"));
    }
}

void CharSegmentPanel::onSave()
{
    QStringList hs;
    for (int i = 0; i < m_list->count(); ++i)
        hs.append(m_list->item(i)->text().trimmed());
    hs.removeAll(QString());
    if (writeHeaders(hs)) {
        emit statusMessage(QStringLiteral("字符段已保存到 %1").arg(configPath()));
        QMessageBox::information(this, QStringLiteral("已保存"),
            QStringLiteral("字符段已保存到 config.json，下次提取生效。"));
    }
}
