#include "manualdialog.h"
#include <QDialogButtonBox>
#include <QFile>
#include <QTextBrowser>
#include <QVBoxLayout>

ManualDialog::ManualDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("EGtools 使用说明书"));
    resize(860, 720);

    auto* view = new QTextBrowser(this);
    view->setOpenExternalLinks(true);
    view->setHtml(QString());

    QFile f(QStringLiteral(":/manual.html"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        view->setHtml(QString::fromUtf8(f.readAll()));
    else
        view->setPlainText(QStringLiteral("未能加载内置说明书资源 :/manual.html"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(view, 1);
    lay->addWidget(buttons);
}