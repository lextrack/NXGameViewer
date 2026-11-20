#pragma once

#include "formats/modding/modlibrary.h"

#include <QDialog>

#include <functional>
#include <vector>

class QLabel;
class QPushButton;
class QTreeWidget;

class ModManagerDialog : public QDialog
{
    Q_OBJECT

public:
    ModManagerDialog(const QStringList &dataFolders, const QString &activeMod, QWidget *parent = nullptr);

    QString chosenMod() const { return m_chosen; }
    int modCount() const;
    bool selectMod(const QString &id);
    std::optional<ModRecord> selectedMod() const;
    QStringList fileList() const;
    void selectFiles(const QStringList &paths);
    bool removeSelectedFiles();
    bool deleteSelectedMod();
    bool packSelectedMod(const QString &zipPath);
    void forgetSelectedMod();
    void forgetMissing();
    void setConfirmation(std::function<bool(const QString &question, const QString &details)> confirm);
    void setUseTrash(bool useTrash) { m_useTrash = useTrash; }

private:
    void reload(const QString &selectId = {});
    void showFiles();
    void updateButtons();
    bool confirm(const QString &question, const QString &details);
    using Removal = std::function<bool(bool toTrash, QString *error)>;
    bool removeWithFallback(const std::vector<Removal> &removals);
    void chooseZip();

    QStringList m_dataFolders;
    QString m_active;
    QString m_chosen;
    std::vector<ModRecord> m_mods;
    std::vector<bool> m_inHistory;
    QTreeWidget *m_list = nullptr;
    QTreeWidget *m_files = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_use = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_zip = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_forget = nullptr;
    QPushButton *m_undo = nullptr;
    std::function<bool(const QString &, const QString &)> m_confirm;
    bool m_useTrash = true;
};
