#pragma once

#include "formats/core/batchexport.h"

#include <QDialog>

#include <map>

class QCheckBox;
class QDialogButtonBox;
class QLineEdit;

class BatchExportDialog : public QDialog
{
    Q_OBJECT

public:
    BatchExportDialog(const QString &what, const QString &sourceFolder, QWidget *parent = nullptr);

    BatchOptions options() const;
    void setOutputFolder(const QString &folder);

    void accept() override;

private:
    void updateButtons();

    std::map<BatchKind, QCheckBox *> m_kinds;
    QLineEdit *m_output = nullptr;
    QCheckBox *m_keepFolders = nullptr;
    QCheckBox *m_openArchives = nullptr;
    QCheckBox *m_keepUnconverted = nullptr;
    QCheckBox *m_keepOriginals = nullptr;
    QCheckBox *m_replace = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
