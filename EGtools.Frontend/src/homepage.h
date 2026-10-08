#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

// The landing page ("首页"). Shows a quick-start walkthrough plus entry cards
// for EVERY capability of the toolset, so the user has one place to start and
// can reach any function without hunting through menus. It holds no logic: it
// only emits navigation requests that MainWindow fulfils.
class HomePage : public QWidget
{
    Q_OBJECT
public:
    explicit HomePage(QWidget* parent = nullptr);

    // Shows whether the CLI kernels were found, so the user knows up-front
    // whether extraction will work.
    void setKernelStatus(bool found, const QString& detail);

signals:
    void startExtract();        // go to the PDF workspace
    void openPdfRequested();
    void wholePageExtract();    // EGpdf2excel full-page table extraction
    void diffRequested();       // EGpdf2df drawing diff
    void excelCompareRequested(); // EGexcel2df Excel comparison
    void schemesRequested();      // reusable box-selection schemes
    void manualRequested();
    void aboutRequested();

private:
    QWidget* makeCard(const QString& iconText, const QString& title,
                      const QString& desc, const QString& cta, QPushButton** outBtn);
    QLabel* m_kernelBadge = nullptr;
};