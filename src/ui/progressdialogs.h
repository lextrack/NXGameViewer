#pragma once

#include <QProgressDialog>

inline QProgressDialog *makeProgressDialog(QWidget *parent, const QString &label, const QString &title, int maximum,
                                           int minimumDuration = 300)
{
    auto *progress = new QProgressDialog(label, QObject::tr("Cancel"), 0, maximum, parent);
    progress->setWindowTitle(title);
    progress->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(minimumDuration);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    return progress;
}

inline void closeProgressDialog(QProgressDialog *progress)
{
    progress->reset();
    progress->hide();
    delete progress;
}
