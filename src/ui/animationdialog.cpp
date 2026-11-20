#include "animationdialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

AnimationDialog::AnimationDialog(const std::vector<AnimationFile> &found, std::shared_ptr<const Model3D> model,
                                 QString folder, QWidget *parent)
    : QDialog(parent), m_model(std::move(model)), m_folder(std::move(folder))
{
    setWindowTitle(tr("Export model with animations"));
    resize(520, 560);

    const bool tickAll = found.size() <= size_t(kMaxTickedByDefault);
    QString summary;
    if (found.empty())
        summary = tr("No animations for this model were found next to it. Use “Add files…” to pick them.");
    else if (tickAll)
        summary = tr("Found %n animation(s) whose bones fit this model. Each ticked one is saved as a "
                     "separate animation in the .glb file.",
                     nullptr, int(found.size()));
    else
        summary = tr("Found %n animations whose bones fit this model. Tick the ones to save in the .glb file: "
                     "each one is saved as a separate animation. Use the filter to find them by name.",
                     nullptr, int(found.size()));
    m_summary = new QLabel(summary);
    m_summary->setWordWrap(true);

    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter by name"));
    m_filter->setToolTip(tr("Shows only the animations whose name contains this text"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &AnimationDialog::applyFilter);

    m_list = new QListWidget;
    m_list->setUniformItemSizes(true);
    connect(m_list, &QListWidget::itemChanged, this, &AnimationDialog::updateCount);

    auto *all = new QPushButton(tr("Select all"));
    auto *none = new QPushButton(tr("Select none"));
    auto *add = new QPushButton(tr("Add files…"));
    all->setToolTip(tr("Tick every animation shown by the filter"));
    none->setToolTip(tr("Untick every animation shown by the filter"));
    add->setToolTip(tr("Pick animation files that were not found automatically"));
    connect(all, &QPushButton::clicked, this, [this] { setVisibleChecked(true); });
    connect(none, &QPushButton::clicked, this, [this] { setVisibleChecked(false); });
    connect(add, &QPushButton::clicked, this, &AnimationDialog::addFiles);
    m_count = new QLabel;

    auto *row = new QHBoxLayout;
    row->addWidget(all);
    row->addWidget(none);
    row->addWidget(add);
    row->addStretch();
    row->addWidget(m_count);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Export…"));
    m_buttons->button(QDialogButtonBox::Ok)->setToolTip(tr("Save the model with the ticked animations as a .glb file"));
    m_buttons->button(QDialogButtonBox::Cancel)->setToolTip(tr("Close without exporting"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_summary);
    layout->addWidget(m_filter);
    layout->addWidget(m_list, 1);
    layout->addLayout(row);
    layout->addWidget(m_buttons);

    {
        const QSignalBlocker blocker(m_list);
        for (const AnimationFile &file : found)
            addFile(file, tickAll);
    }
    updateCount();
}

std::vector<AnimationFile> AnimationDialog::selected() const
{
    std::vector<AnimationFile> files;
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->checkState() == Qt::Checked)
            files.push_back(m_files.at(size_t(m_list->item(i)->data(Qt::UserRole).toInt())));
    return files;
}

void AnimationDialog::addFile(const AnimationFile &file, bool ticked)
{
    for (const AnimationFile &known : m_files)
        if (known.sameAnimation(file))
            return;
    m_files.push_back(file);
    auto *item = new QListWidgetItem(file.name, m_list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(ticked ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, int(m_files.size() - 1));
    QString tip = tr("%1\n%2 of %3 animated bones are in the model")
                      .arg(QDir::toNativeSeparators(file.path))
                      .arg(file.matchedBones)
                      .arg(file.bones);
    if (file.frames > 0)
        tip += tr("\n%n frame(s)", nullptr, file.frames);
    item->setToolTip(tip);
    item->setHidden(!m_filter->text().isEmpty() && !file.name.contains(m_filter->text(), Qt::CaseInsensitive));
}

void AnimationDialog::addFiles()
{
    const QStringList paths =
        QFileDialog::getOpenFileNames(this, tr("Add animations"), m_folder,
                                      tr("Models and animations (*.mdl *.bfres *.zs *.szs *.sarc *.pack *.was);;All files (*)"));
    QStringList rejected;
    for (const QString &path : paths) {
        const std::vector<AnimationFile> files = inspectAnimationFile(path, *m_model);
        for (const AnimationFile &file : files)
            addFile(file, files.size() <= size_t(kMaxTickedByDefault));
        if (files.empty())
            rejected << QFileInfo(path).fileName();
    }
    updateCount();
    if (!rejected.isEmpty())
        QMessageBox::information(this, tr("Some files were not added"),
                                 tr("These files are not animations whose bones fit this model:\n%1")
                                     .arg(rejected.join(QLatin1Char('\n'))));
}

void AnimationDialog::applyFilter(const QString &text)
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
    }
}

void AnimationDialog::setVisibleChecked(bool checked)
{
    {
        const QSignalBlocker blocker(m_list);
        for (int i = 0; i < m_list->count(); ++i)
            if (!m_list->item(i)->isHidden())
                m_list->item(i)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    updateCount();
}

void AnimationDialog::updateCount()
{
    int selected = 0;
    for (int i = 0; i < m_list->count(); ++i)
        selected += m_list->item(i)->checkState() == Qt::Checked ? 1 : 0;
    m_count->setText(tr("%1 of %2 selected").arg(selected).arg(m_list->count()));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(selected > 0);
}
