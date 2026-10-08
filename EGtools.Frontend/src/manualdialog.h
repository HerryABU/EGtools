#pragma once

#include <QDialog>

// In-app manual ("帮助 → 使用说明书"). Renders the HTML manual embedded in the
// binary via a Qt resource, so it travels with the app (no external file).
class ManualDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ManualDialog(QWidget* parent = nullptr);
};