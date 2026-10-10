#include "diffpage.h"
#include "clirunner.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QTemporaryDir>

DiffPage::DiffPage(QWidget* parent) : QWidget(parent)
{
    auto* baseRow = new QHBoxLayout();
    m_baseEdit = new QLineEdit();
    m_baseEdit->setPlaceholderText(QStringLiteral("基准图 (baseline) PDF"));
    auto* baseBtn = new QPushButton(QStringLiteral("选择…"));
    baseRow->addWidget(new QLabel(QStringLiteral("基准图:")));
    baseRow->addWidget(m_baseEdit, 1);
    baseRow->addWidget(baseBtn);

    auto* tgtRow = new QHBoxLayout();
    m_tgtEdit = new QLineEdit();
    m_tgtEdit->setPlaceholderText(QStringLiteral("目标图 (target) PDF"));
    auto* tgtBtn = new QPushButton(QStringLiteral("选择…"));
    tgtRow->addWidget(new QLabel(QStringLiteral("目标图:")));
    tgtRow->addWidget(m_tgtEdit, 1);
    tgtRow->addWidget(tgtBtn);

    m_runBtn = new QPushButton(QStringLiteral("开始比对（红线=新增/改动，蓝虚线=删除）"));
    m_runBtn->setStyleSheet(QStringLiteral("QPushButton{background:#2d7d46;color:#fff;padding:6px 12px;border-radius:5px;font-weight:600;}"));
    auto* openBtn = new QPushButton(QStringLiteral("打开差异图…"));

    m_status = new QLabel(QStringLiteral("内核状态：未探测"));
    m_status->setWordWrap(true);

    auto* ctrl = new QWidget();
    auto* ctrlLay = new QVBoxLayout(ctrl);
    ctrlLay->setContentsMargins(8, 8, 8, 8);
    ctrlLay->addLayout(baseRow);
    ctrlLay->addLayout(tgtRow);
    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(m_runBtn);
    btnRow->addWidget(openBtn);
    btnRow->addStretch();
    ctrlLay->addLayout(btnRow);
    ctrlLay->addWidget(m_status);
    ctrlLay->addStretch();

    m_jsonView = new QTextEdit();
    m_jsonView->setReadOnly(true);
    m_jsonView->setLineWrapMode(QTextEdit::NoWrap);

    m_preview = new QLabel(QStringLiteral("差异标注预览（运行后显示）"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet(QStringLiteral("QLabel{background:#f4f4f4;border:1px solid #ccc;border-radius:5px;color:#666;}"));
    m_preview->setMinimumHeight(280);

    auto* right = new QSplitter(Qt::Vertical);
    right->addWidget(m_jsonView);
    right->addWidget(m_preview);
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 2);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(ctrl);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(split);

    connect(baseBtn, &QPushButton::clicked, this, &DiffPage::onPickBaseline);
    connect(tgtBtn,  &QPushButton::clicked, this, &DiffPage::onPickTarget);
    connect(m_runBtn,&QPushButton::clicked, this, &DiffPage::onRun);
    connect(openBtn, &QPushButton::clicked, this, &DiffPage::onBrowsePreview);
}

QString DiffPage::cliExe(const QString& name) const
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

void DiffPage::refreshKernelStatus()
{
    bool ok = QFileInfo::exists(cliExe(QStringLiteral("EGpdf2df")));
    m_status->setText(ok ? QStringLiteral("内核状态：已就绪（EGpdf2df）")
                         : QStringLiteral("内核状态：未找到 EGpdf2df.exe — 请在首页设置 CLI 目录"));
    m_runBtn->setEnabled(ok);
}

void DiffPage::onPickBaseline()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择基准图 PDF"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (!p.isEmpty()) m_baseEdit->setText(p);
}

void DiffPage::onPickTarget()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择目标图 PDF"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (!p.isEmpty()) m_tgtEdit->setText(p);
}

void DiffPage::onRun()
{
    const QString a = m_baseEdit->text().trimmed();
    const QString b = m_tgtEdit->text().trimmed();
    if (a.isEmpty() || b.isEmpty()) {
        emit statusMessage(QStringLiteral("请先选择基准图与目标图"));
        return;
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) { emit statusMessage(QStringLiteral("无法创建临时目录")); return; }
    QString outPdf = QDir(tmp.path()).filePath(QStringLiteral("diff.pdf"));

    QString out, err;
    int rc = CliRunner::run(cliExe(QStringLiteral("EGpdf2df")),
        {a, b, QStringLiteral("-o"), outPdf}, out, err);
    emit statusMessage(QStringLiteral("[EGpdf2df] rc=%1 %2").arg(rc).arg(err.trimmed()));
    m_jsonView->setPlainText(out);

    if (rc != 0) return;

    // Render the produced diff PDF to a BMP and show it as the annotated preview.
    QString bmp = QDir(tmp.path()).filePath(QStringLiteral("diff.bmp"));
    QString out2, err2;
    if (CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
            {QStringLiteral("--render"), QStringLiteral("--page"), QStringLiteral("0"),
             QStringLiteral("--scale"), QStringLiteral("0.6"),
             QStringLiteral("--render-out"), bmp, outPdf}, out2, err2) == 0) {
        QImage img = QImageReader(bmp).read();
        if (!img.isNull()) {
            m_preview->setPixmap(QPixmap::fromImage(img).scaled(
                m_preview->width() - 16, m_preview->height() - 16,
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
            emit statusMessage(QStringLiteral("差异图已生成并预览（红实线=新增/改动，蓝虚线=删除）"));
            return;
        }
    }
    m_preview->setText(QStringLiteral("差异 PDF 已生成，但预览渲染失败：%1").arg(err2.trimmed()));
}

void DiffPage::onBrowsePreview()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("打开差异图 PDF"),
        QString(), QStringLiteral("PDF (*.pdf)"));
    if (p.isEmpty()) return;
    QTemporaryDir tmp;
    if (!tmp.isValid()) return;
    QString bmp = QDir(tmp.path()).filePath(QStringLiteral("view.bmp"));
    QString out, err;
    if (CliRunner::run(cliExe(QStringLiteral("EGpdf2excel")),
            {QStringLiteral("--render"), QStringLiteral("--page"), QStringLiteral("0"),
             QStringLiteral("--scale"), QStringLiteral("0.6"),
             QStringLiteral("--render-out"), bmp, p}, out, err) == 0) {
        QImage img = QImageReader(bmp).read();
        if (!img.isNull()) {
            m_preview->setPixmap(QPixmap::fromImage(img).scaled(
                m_preview->width() - 16, m_preview->height() - 16,
                Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
}
