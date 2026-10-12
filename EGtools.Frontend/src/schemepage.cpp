#include "schemepage.h"
#include "panels.h"

#include <QComboBox>
#include <QSizePolicy>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {

const char* const kKinds[] = { "global_table", "detail_table", "material_no_region" };

QString kindLabel(const QString& k)
{
    if (k == QLatin1String("global_table"))       return QStringLiteral("全域表");
    if (k == QLatin1String("material_no_region")) return QStringLiteral("材料编号区");
    return QStringLiteral("细表");
}

QPushButton* mkBtn(const QString& text, const QString& tip = QString())
{
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    return b;
}

} // namespace

SchemePage::SchemePage(SchemeStore* store, QWidget* parent)
    : QWidget(parent), m_store(store)
{
    m_store->load();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("框选方案 — 保存 / 复用 / 修改 / 导出"));
    title->setWordWrap(true);
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    title->setObjectName(QStringLiteral("sectionTitle"));
    root->addWidget(title);

    auto* split = new QSplitter(Qt::Horizontal);

    // ---------------- Left: scheme list ----------------
    auto* left = new QWidget();
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    m_list = new QListWidget();
    lv->addWidget(m_list, 1);
    auto* lrow = new QHBoxLayout();
    auto* bNew = mkBtn(QStringLiteral("新建空方案"));
    auto* bCap = mkBtn(QStringLiteral("从当前框选存入"));
    auto* bDel = mkBtn(QStringLiteral("删除"));
    lrow->addWidget(bNew);
    lrow->addWidget(bCap);
    lrow->addWidget(bDel);
    lv->addLayout(lrow);
    split->addWidget(left);

    // ---------------- Right: detail editor ----------------
    auto* right = new QWidget();
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);

    auto* nrow = new QHBoxLayout();
    nrow->addWidget(new QLabel(QStringLiteral("方案名:")));
    m_name = new QLineEdit();
    m_name->setPlaceholderText(QStringLiteral("例如：主干管 DN300 材料表"));
    nrow->addWidget(m_name, 1);
    rv->addLayout(nrow);

    m_note = new QTextEdit();
    m_note->setPlaceholderText(QStringLiteral("备注（可选）：这张方案适用于哪类图纸 / 哪一段管号"));
    m_note->setMaximumHeight(54);
    rv->addWidget(m_note);

    m_table = new QTableWidget(0, 7);
    m_table->setHorizontalHeaderLabels({QStringLiteral("类型(kind)"), QStringLiteral("页"),
        QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("宽"), QStringLiteral("高"),
        QStringLiteral("操作")});
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    rv->addWidget(m_table, 1);

    auto* hint = new QLabel(QStringLiteral(
        "直接在表格里改「类型 / 页 / X / Y / 宽 / 高」即可修改字段（无需编辑 JSON）。\n"
        "坐标单位为 PDF 点，左上原点、y 向下；「重写框选」会回到画布重新框一次。"));
    hint->setObjectName(QStringLiteral("hintText"));
    // 允许换行 + 忽略宽度诉求：否则这段说明会把整个方案页（进而主窗口）顶宽。
    hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    rv->addWidget(hint);

    auto* brow = new QHBoxLayout();
    auto* bAdd = mkBtn(QStringLiteral("+ 添加区域"));
    auto* bRm  = mkBtn(QStringLiteral("- 删除选中行"));
    auto* bRw  = mkBtn(QStringLiteral("重写框选"), QStringLiteral("回到画布重新框选选中的区域"));
    auto* bRe  = mkBtn(QStringLiteral("复用到画布"));
    auto* bEx  = mkBtn(QStringLiteral("导出 JSON"));
    auto* bIm  = mkBtn(QStringLiteral("导入 JSON"));
    brow->addWidget(bAdd);
    brow->addWidget(bRm);
    brow->addWidget(bRw);
    brow->addWidget(bRe);
    brow->addStretch();
    brow->addWidget(bEx);
    brow->addWidget(bIm);
    rv->addLayout(brow);

    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    root->addWidget(split, 1);
    makeSplitterNonDraggable(split);   // 不给拖（此时把手已创建，禁用才有效）

    m_current = new QLabel(QStringLiteral("未选中方案"));
    m_current->setObjectName(QStringLiteral("hintText"));
    root->addWidget(m_current);

    // ---- wiring ----
    connect(bNew,  &QPushButton::clicked, this, &SchemePage::onNewScheme);
    connect(bCap,  &QPushButton::clicked, this, &SchemePage::onCaptureCanvas);
    connect(bDel,  &QPushButton::clicked, this, &SchemePage::onDeleteScheme);
    connect(bAdd,  &QPushButton::clicked, this, &SchemePage::onAddRegion);
    connect(bRm,   &QPushButton::clicked, this, &SchemePage::onRemoveRegion);
    connect(bRw,   &QPushButton::clicked, this, &SchemePage::onRewriteRegion);
    connect(bRe,   &QPushButton::clicked, this, &SchemePage::onReuse);
    connect(bEx,   &QPushButton::clicked, this, &SchemePage::onExport);
    connect(bIm,   &QPushButton::clicked, this, &SchemePage::onImport);

    connect(m_list, &QListWidget::currentRowChanged, this, &SchemePage::onListSelectionChanged);
    connect(m_table, &QTableWidget::cellChanged, this, &SchemePage::onCellChanged);
    connect(m_name, &QLineEdit::editingFinished, this, &SchemePage::pushEditToScheme);
    connect(m_note, &QTextEdit::textChanged, this, &SchemePage::pushEditToScheme);

    reload();
}

// ------------------------------------------------------------------ helpers

Scheme* SchemePage::current()
{
    return m_currentId.isEmpty() ? nullptr : m_store->findMutable(m_currentId);
}

void SchemePage::reload() { rebuildList(m_currentId); }

void SchemePage::selectScheme(const QString& id) { rebuildList(id); }

void SchemePage::rebuildList(const QString& selectId)
{
    const QString want = selectId.isEmpty() ? m_currentId : selectId;
    m_loading = true;
    m_list->clear();
    int target = -1;
    const auto all = m_store->schemes();
    for (int i = 0; i < all.size(); ++i) {
        auto* it = new QListWidgetItem(QStringLiteral("%1  (%2 个区域)")
                                            .arg(all[i].name).arg(all[i].regions.size()), m_list);
        it->setData(Qt::UserRole, all[i].id);
        if (all[i].id == want) target = i;
    }
    m_loading = false;
    if (target >= 0) {
        m_list->setCurrentRow(target);
    } else {
        m_currentId.clear();
        m_table->setRowCount(0);
        m_name->clear();
        m_note->clear();
        m_current->setText(QStringLiteral("未选中方案"));
    }
}

void SchemePage::storeIntoTable(const Scheme& s)
{
    m_loading = true;
    m_table->setRowCount(s.regions.size());
    for (int r = 0; r < s.regions.size(); ++r) {
        const RegionSpec& g = s.regions[r];

        auto* cb = new QComboBox();
        for (const char* k : kKinds) cb->addItem(kindLabel(QString::fromLatin1(k)), QString::fromLatin1(k));
        cb->setCurrentIndex(cb->findData(g.kind));
        m_table->setCellWidget(r, 0, cb);

        m_table->setItem(r, 1, new QTableWidgetItem(QString::number(g.page)));
        m_table->setItem(r, 2, new QTableWidgetItem(QString::number(g.x, 'f', 1)));
        m_table->setItem(r, 3, new QTableWidgetItem(QString::number(g.y, 'f', 1)));
        m_table->setItem(r, 4, new QTableWidgetItem(QString::number(g.w, 'f', 1)));
        m_table->setItem(r, 5, new QTableWidgetItem(QString::number(g.h, 'f', 1)));

        auto* b = mkBtn(QStringLiteral("重写框选"));
        b->setProperty("row", r);
        m_table->setCellWidget(r, 6, b);
        connect(b, &QPushButton::clicked, this, [this, r] {
            emit rewriteRequested(m_currentId, r);
        });
    }
    m_loading = false;
}

void SchemePage::loadIntoEditor()
{
    Scheme* s = current();
    if (!s) {
        m_table->setRowCount(0);
        m_current->setText(QStringLiteral("未选中方案"));
        return;
    }
    m_loading = true;
    m_name->setText(s->name);
    m_note->setPlainText(s->note);
    m_loading = false;
    storeIntoTable(*s);
    m_current->setText(QStringLiteral("当前方案：%1 — 存储于 %2")
                       .arg(s->name).arg(m_store->path()));
}

QVector<RegionSpec> SchemePage::tableToRegions() const
{
    QVector<RegionSpec> out;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        RegionSpec g;
        if (auto* cb = qobject_cast<QComboBox*>(m_table->cellWidget(r, 0)))
            g.kind = cb->currentData().toString();
        auto val = [&](int c, double def) {
            auto* it = m_table->item(r, c);
            bool ok = false;
            double d = it ? it->text().trimmed().toDouble(&ok) : def;
            return ok ? d : def;
        };
        g.page = qMax(0, static_cast<int>(val(1, 0)));
        g.x = val(2, 0); g.y = val(3, 0); g.w = val(4, 0); g.h = val(5, 0);
        out.append(g);
    }
    return out;
}

void SchemePage::pushEditToScheme()
{
    if (m_loading) return;
    Scheme* s = current();
    if (!s) return;
    s->name   = m_name->text().trimmed().isEmpty() ? QStringLiteral("未命名方案") : m_name->text().trimmed();
    s->note   = m_note->toPlainText();
    s->regions = tableToRegions();
    m_store->update(*s);
    m_store->save();
    rebuildList(s->id);
}

// ------------------------------------------------------------------- actions

void SchemePage::onListSelectionChanged()
{
    auto* it = m_list->currentItem();
    if (!it) return;
    m_currentId = it->data(Qt::UserRole).toString();
    loadIntoEditor();
}

void SchemePage::onCellChanged(int, int) { pushEditToScheme(); }

void SchemePage::onNewScheme()
{
    Scheme s;
    s.id   = SchemeStore::newId();
    s.name = QStringLiteral("新建方案");
    RegionSpec g; g.kind = QStringLiteral("detail_table"); g.w = 200; g.h = 100;
    s.regions.append(g);
    m_store->add(s);
    m_store->save();
    rebuildList(s.id);
    emit statusMessage(QStringLiteral("已新建方案，可直接在右侧表格编辑字段"));
}

void SchemePage::onDeleteScheme()
{
    if (m_currentId.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("删除方案"),
            QStringLiteral("确定删除方案「%1」？").arg(current() ? current()->name : QString())) != QMessageBox::Yes)
        return;
    m_store->remove(m_currentId);
    m_store->save();
    m_currentId.clear();
    rebuildList();
}

void SchemePage::captureFromCanvas(const QVector<RegionSpec>& regions)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("存为方案"),
        QStringLiteral("方案名:"), QLineEdit::Normal,
        regions.size() == 1 ? kindLabel(regions.first().kind) : QStringLiteral("框选方案"), &ok);
    if (!ok) return;
    Scheme s = m_store->makeFromSelection(
        name.isEmpty() ? QStringLiteral("未命名方案") : name, QString(), regions);
    m_store->add(s);
    m_store->save();
    rebuildList(s.id);
    emit statusMessage(QStringLiteral("已把当前 %1 个框选存为方案「%2」").arg(regions.size()).arg(s.name));
}

void SchemePage::onCaptureCanvas()
{
    emit statusMessage(QStringLiteral("请先在工作区画布上框选区域，再点「从当前框选存入」"));
}

void SchemePage::onAddRegion()
{
    Scheme* s = current();
    if (!s) return;
    RegionSpec g; g.kind = QStringLiteral("detail_table"); g.w = 200; g.h = 100;
    s->regions.append(g);
    m_store->update(*s);
    m_store->save();
    loadIntoEditor();
}

void SchemePage::onRemoveRegion()
{
    Scheme* s = current();
    if (!s) return;
    const int row = m_table->currentRow();
    if (row < 0 || row >= s->regions.size()) return;
    s->regions.remove(row);
    m_store->update(*s);
    m_store->save();
    loadIntoEditor();
}

void SchemePage::onRewriteRegion()
{
    const int row = m_table->currentRow();
    if (row < 0) { emit statusMessage(QStringLiteral("请先在表格里选中要重写的区域行")); return; }
    emit rewriteRequested(m_currentId, row);
}

void SchemePage::onReuse()
{
    if (m_currentId.isEmpty()) return;
    pushEditToScheme();          // make sure pending edits are persisted first
    emit reuseRequested(m_currentId);
}

void SchemePage::onExport()
{
    Scheme* s = current();
    if (!s) return;
    pushEditToScheme();
    s = current();
    const QString p = QFileDialog::getSaveFileName(this, QStringLiteral("导出方案 JSON"),
        QStringLiteral("%1.json").arg(s->name), QStringLiteral("JSON (*.json)"));
    if (p.isEmpty()) return;

    // Wrap into a RegionRequest so the file can be fed to the CLI as-is.
    QJsonArray regs;
    for (const RegionSpec& r : s->regions) regs.append(r.toJson());
    QJsonObject req;
    req.insert(QStringLiteral("pdf"), QStringLiteral(""));   // user fills the target PDF
    req.insert(QStringLiteral("regions"), regs);

    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit statusMessage(QStringLiteral("导出失败：无法写入文件"));
        return;
    }
    f.write(QJsonDocument(req).toJson(QJsonDocument::Indented));
    f.close();
    emit statusMessage(QStringLiteral("已导出方案到 %1（可直接作为 --json 请求交给内核）").arg(p));
}

void SchemePage::onImport()
{
    const QString p = QFileDialog::getOpenFileName(this, QStringLiteral("导入方案 JSON"),
        QString(), QStringLiteral("JSON (*.json)"));
    if (p.isEmpty()) return;
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) {
        emit statusMessage(QStringLiteral("导入失败：无法读取文件"));
        return;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        emit statusMessage(QStringLiteral("导入失败：JSON 解析错误 — %1").arg(pe.errorString()));
        return;
    }
    const QJsonObject root = doc.object();
    const QJsonArray regs = root.value(QStringLiteral("regions")).toArray();
    if (regs.isEmpty()) {
        emit statusMessage(QStringLiteral("导入失败：文件中没有 regions 数组"));
        return;
    }
    Scheme s;
    s.id   = SchemeStore::newId();
    s.name = QFileInfo(p).completeBaseName();
    s.note = QStringLiteral("由 %1 导入").arg(QFileInfo(p).fileName());
    for (const QJsonValue& v : regs) s.regions.append(RegionSpec::fromJson(v.toObject()));
    m_store->add(s);
    m_store->save();
    rebuildList(s.id);
    emit statusMessage(QStringLiteral("已导入方案「%1」（%2 个区域）").arg(s.name).arg(s.regions.size()));
}

void SchemePage::appendStatusRow(const QString& text)
{
    emit statusMessage(text);
}