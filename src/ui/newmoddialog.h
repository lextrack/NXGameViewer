#pragma once

#include "formats/modding/modlayout.h"

#include <QDialog>

#include <map>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

class NewModDialog : public QDialog
{
    Q_OBJECT

public:
    NewModDialog(const QString &contextPath, const QString &currentModFolder, QWidget *parent = nullptr);

    ModLocation location() const;
    QString romfsPath() const;

    void accept() override;

private:
    ModLayout layout() const;
    void layoutChanged();
    void baseChanged();
    QString baseNote() const;
    QString problem() const;
    void updateResult();

    QComboBox *m_layout = nullptr;
    QLabel *m_baseLabel = nullptr;
    QLineEdit *m_base = nullptr;
    QLineEdit *m_titleId = nullptr;
    QLabel *m_titleHint = nullptr;
    QLabel *m_nameLabel = nullptr;
    QLineEdit *m_name = nullptr;
    QLabel *m_result = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    std::map<ModLayout, QString> m_bases;
    ModLayout m_shownLayout = ModLayout::YuzuFamily;
};
