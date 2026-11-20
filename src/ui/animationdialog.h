#pragma once

#include "formats/models/animationsearch.h"

#include <QDialog>

#include <memory>
#include <vector>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;

class AnimationDialog : public QDialog
{
    Q_OBJECT

public:
    AnimationDialog(const std::vector<AnimationFile> &found, std::shared_ptr<const Model3D> model, QString folder,
                    QWidget *parent = nullptr);

    std::vector<AnimationFile> selected() const;

    static constexpr int kMaxTickedByDefault = 24;

private:
    void addFile(const AnimationFile &file, bool ticked);
    void addFiles();
    void applyFilter(const QString &text);
    void setVisibleChecked(bool checked);
    void updateCount();

    std::shared_ptr<const Model3D> m_model;
    std::vector<AnimationFile> m_files;
    QString m_folder;
    QLabel *m_summary = nullptr;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_count = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
