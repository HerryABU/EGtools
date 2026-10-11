#include "excelcomparepage.h"
#include "clirunner.h"
#include "kernelclient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTextEdit>
#include <QVBoxLayout>

ExcelComparePage::ExcelComparePage(QWidget* parent) : QWidget(parent)
{
    auto* oldRow = new QHBoxLayout();
    m_oldEdit = new QLineEdit();
    m_oldEdit->setPlaceholderText(QStringLiteral("旧图纸 Excel"));
    auto* oldBtn = new QPushButton(QStringLiteral("选择…"));
    oldRow->addWidget(new QLabel(QStringLiteral("旧图:")));
    oldRow->addWidget(m_oldEdit, 1);
    oldRow->addWidget(oldBtn);

    auto* newRow = new QHBoxLayout();
    m_newEdit = new QLineEdit();
    m_newEdit->setPlaceholderText(QStringLiteral("新图纸 Excel"));
    auto* newBtn = new QPushButton(QStringLiteral("选择…"));
    newRow->addWidget(new QLabel(QStringLiteral("新图:")));
    newRow->addWidget(m_newEdit, 1);
    newRow->addWidget(newBtn);

    m_runBtn = new QPushButton(QStringLiteral("开始比对（输出变化清单 .xlsx）"));
    m_runBtn->setStyleSheet(QStringLiteral("QPushButton{background:#2d5d8a;color:#fff;padding:6px 12px;border-radius:5px;font-weight:600;}"));

    m_status = new QLabel(QStringLiteral("内核状态：未探测"));
    m_status->setWordWrap(true);

    auto* ctrl = new QWidget();
    auto* ctrlLay = new QVBoxLayout(ctrl);
    ctrlLay->setContentsMargins(8, 8, 8, 8);
    ctrlLay->addLayout(oldRow);
    ctrlLay->addLayout(newRow);
    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(m_runBtn);
    btnRow->addStretch();
    ctrlLay->addLayout(btnRow);
    ctrlLay->addWidget(m_status);
    ctrlLay->addStretch();

    m_jsonView = new QTextEdit();
    m_jsonView->setReadOnly(true);
    m_jsonView->setLineWrapMode(QTextEdit::NoWrap);

    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(ctrl);
    split->addWidget(m_jsonView);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(split);

    connect(oldBtn, &QPushButton::clicked, this, &ExcelComparePage::onPickOld);
    connect(newBtn, &QPushButton::clicked, this, &ExcelComparePage::onPickNew);
    connect(m_runBtn,&QPushButton::clicked, this, &ExcelComparePage::onRun);
}

QString ExcelComparePage::cliExe(const QString& name) const
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

void ExcelComparePage::refreshKernelStatus()
{
    bool ok = QFileInfo::exists(cliExe(QStringLiteral("EGexcel2df")));
    m_status->setText(ok ? QStringLiteral("内核状态：已就绪（EGexcel2df）")
                         : QStringLiteral("内核状态：未找到 EGexcel2df.exe — 请在首页设置 CLI 目录"));
    m_runBtn->setEnabled(ok);
}

void ExcelComparePage::onPickOld()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择旧图纸 Excel"),
        QString(), QStringLiteral("Excel (*.xlsx *.xlsm)"));
    if (!p.isEmpty()) m_oldEdit->setText(p);
}

void ExcelComparePage::onPickNew()
{
    QString p = QFileDialog::getOpenFileName(this, QStringLiteral("选择新图纸 Excel"),
        QString(), QStringLiteral("Excel (*.xlsx *.xlsm)"));
    if (!p.isEmpty()) m_newEdit->setText(p);
}

void ExcelComparePage::onRun()
{
    const QString a = m_oldEdit->text().trimmed();
    const QString b = m_newEdit->text().trimmed();
    if (a.isEmpty() || b.isEmpty()) {
        emit statusMessage(QStringLiteral("请先选择旧图与新图 Excel"));
        return;
    }
    QString outXlsx = QFileDialog::getSaveFileName(this, QStringLiteral("输出对比清单"),
        QStringLiteral("图纸变化清单.xlsx"), QStringLiteral("Excel (*.xlsx)"));
    if (outXlsx.isEmpty()) return;

    // 走融合层：纯参数调用（无 JSON 契约）两条路都支持 —— 内嵌直接进程内调，命令行起 exe。
    const KernelClient::Result kr = KernelClient::run(
        QStringLiteral("EGexcel2df"),
        {a, b, QStringLiteral("-o"), outXlsx}, QString(),
        KernelClient::backendFromString(m_kernelBackend));
    const QString out = kr.out, err = kr.err;
    const int rc = kr.rc;
    emit statusMessage(QStringLiteral("[EGexcel2df/%1] rc=%2 %3").arg(kr.backend).arg(rc).arg(err.trimmed()));
    m_jsonView->setPlainText(out);
    if (rc == 0)
        emit statusMessage(QStringLiteral("Excel 比对完成，输出：%1").arg(outXlsx));
}
