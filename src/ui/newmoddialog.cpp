#include "newmoddialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QVBoxLayout>

namespace {

QString settingsKey(ModLayout layout)
{
    switch (layout) {
    case ModLayout::YuzuFamily: return QStringLiteral("newMod/base/yuzu");
    case ModLayout::Ryujinx: return QStringLiteral("newMod/base/ryujinx");
    case ModLayout::Atmosphere: return QStringLiteral("newMod/base/atmosphere");
    }
    return {};
}

}

NewModDialog::NewModDialog(const QString &contextPath, const QString &currentModFolder, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New mod"));
    QSettings settings;

    auto *intro = new QLabel(tr("Makes the folder an emulator loads changed game files from. Files you change "
                                "are saved in it at the same path they have in the game."));
    intro->setWordWrap(true);

    m_layout = new QComboBox;
    m_layout->addItem(tr("yuzu and similar emulators"), int(ModLayout::YuzuFamily));
    m_layout->addItem(tr("Ryujinx"), int(ModLayout::Ryujinx));
    m_layout->addItem(tr("Atmosphère (console SD card)"), int(ModLayout::Atmosphere));
    m_layout->setToolTip(tr("Where the mod will be used: each emulator, and the console, expects mods in a different folder layout"));

    for (ModLayout l : {ModLayout::YuzuFamily, ModLayout::Ryujinx, ModLayout::Atmosphere})
        m_bases[l] = settings.value(settingsKey(l)).toString();
    ModLayout start = ModLayout(settings.value(QStringLiteral("newMod/layout"), int(ModLayout::YuzuFamily)).toInt());
    const std::optional<ModLocation> current = parseModFolder(currentModFolder);
    if (current) {
        start = current->layout;
        m_bases[current->layout] = QDir::toNativeSeparators(current->base);
    }
    const QString emulator = suggestedEmulatorFolder(contextPath);
    if (m_bases[ModLayout::YuzuFamily].isEmpty() && !emulator.isEmpty())
        m_bases[ModLayout::YuzuFamily] = QDir::toNativeSeparators(emulator);

    m_base = new QLineEdit;
    m_base->setToolTip(tr("The mod folder is made inside this folder. Type or paste a path, or use Browse"));
    connect(m_base, &QLineEdit::textChanged, this, &NewModDialog::baseChanged);
    auto *browse = new QPushButton(tr("Browse…"));
    browse->setToolTip(tr("Pick the folder to make the mod in"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString initial = m_base->text().isEmpty() ? QDir::homePath() : m_base->text();
        const QString folder = QFileDialog::getExistingDirectory(this, m_baseLabel->text().chopped(1), initial);
        if (!folder.isEmpty())
            m_base->setText(QDir::toNativeSeparators(folder));
    });
    auto *baseRow = new QHBoxLayout;
    baseRow->addWidget(m_base, 1);
    baseRow->addWidget(browse);

    m_titleId = new QLineEdit;
    m_titleId->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9A-Fa-f]{0,16}")), m_titleId));
    m_titleId->setPlaceholderText(tr("16 hexadecimal digits, for example 0100…"));
    m_titleId->setFont(QFont(QStringLiteral("monospace")));
    m_titleId->setToolTip(tr("The game's 16-digit title ID, which tells the emulator which game the mod is for"));
    m_titleHint = new QLabel;
    m_titleHint->setWordWrap(true);
    if (const std::optional<QString> id = titleIdFromPath(contextPath)) {
        m_titleId->setText(applicationTitleId(*id));
        m_titleHint->setText(applicationTitleId(*id) == *id
                                 ? tr("Taken from the path of the open file.")
                                 : tr("Taken from the path of the open file. %1 is an update's ID; mods use the "
                                      "game's own ID, which ends in 000.").arg(*id));
    } else if (current) {
        m_titleId->setText(current->titleId);
        m_titleHint->setText(tr("Taken from the current mod folder."));
    } else {
        m_titleHint->setText(tr("The open file's path has no title ID. Type the game's title ID: emulators show it in "
                                "the game's properties, or in the name of the folder their \"Open Mod Data Location\" "
                                "opens."));
    }
    connect(m_titleId, &QLineEdit::textChanged, this, [this] {
        m_titleHint->clear();
        updateResult();
    });

    m_name = new QLineEdit;
    m_name->setPlaceholderText(tr("Shown in the emulator's list of mods"));
    m_name->setToolTip(tr("The mod's name, shown in the emulator's list of mods"));
    connect(m_name, &QLineEdit::textChanged, this, &NewModDialog::updateResult);

    m_result = new QLabel;
    m_result->setWordWrap(true);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_baseLabel = new QLabel;
    m_nameLabel = new QLabel(tr("Mod name:"));
    auto *form = new QFormLayout;
    form->addRow(tr("Emulator:"), m_layout);
    form->addRow(m_baseLabel, baseRow);
    form->addRow(tr("Title ID:"), m_titleId);
    form->addRow(QString(), m_titleHint);
    form->addRow(m_nameLabel, m_name);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setToolTip(tr("Make the mod folder and use it for the files you change"));
    m_buttons->button(QDialogButtonBox::Cancel)->setToolTip(tr("Close without making a mod"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(m_result);
    layout->addStretch();
    layout->addWidget(m_buttons);

    m_shownLayout = start;
    m_layout->setCurrentIndex(m_layout->findData(int(start)));
    m_base->setText(m_bases[start]);
    connect(m_layout, &QComboBox::currentIndexChanged, this, &NewModDialog::layoutChanged);
    layoutChanged();
    setMinimumWidth(560);
}

ModLayout NewModDialog::layout() const
{
    return ModLayout(m_layout->currentData().toInt());
}

void NewModDialog::layoutChanged()
{
    m_bases[m_shownLayout] = m_base->text();
    m_shownLayout = layout();
    switch (m_shownLayout) {
    case ModLayout::YuzuFamily:
        m_baseLabel->setText(tr("Emulator data folder:"));
        m_base->setPlaceholderText(tr("The folder that holds load (and usually dump)"));
        break;
    case ModLayout::Ryujinx:
        m_baseLabel->setText(tr("Ryujinx data folder:"));
        m_base->setPlaceholderText(tr("The folder that holds mods"));
        break;
    case ModLayout::Atmosphere:
        m_baseLabel->setText(tr("SD card:"));
        m_base->setPlaceholderText(tr("The root of the SD card"));
        break;
    }
    m_base->setText(m_bases[m_shownLayout]);
    baseChanged();
    m_nameLabel->setEnabled(layoutHasModName(m_shownLayout));
    m_name->setEnabled(layoutHasModName(m_shownLayout));
    updateResult();
}

void NewModDialog::baseChanged()
{
    const std::optional<QString> id = titleIdFolder(layout(), m_base->text().trimmed());
    if (id && m_titleId->text().toUpper() != *id) {
        m_titleId->setText(*id);
        m_titleHint->setText(tr("Taken from the folder you chose, which is this game's mod folder."));
    }
    updateResult();
}

QString NewModDialog::baseNote() const
{
    const QString base = location().base;
    if (titleIdFolder(layout(), base))
        return {};
    const QStringList parts = QDir::cleanPath(base).split(u'/');
    for (const QString &part : parts)
        if (isTitleId(part))
            return tr("This folder is inside a game's folder (%1). Mods only load from the emulator's data folder, so "
                      "check this is the right place.")
                .arg(part.toUpper());
    const QString last = parts.isEmpty() ? QString() : parts.last();
    auto has = [&](const QString &name) {
        return last.compare(name, Qt::CaseInsensitive) == 0
               || QFileInfo(QDir(base).filePath(matchExistingCase(base, name))).isDir();
    };
    switch (layout()) {
    case ModLayout::YuzuFamily:
        if (!has(QStringLiteral("load")))
            return tr("There is no <tt>load</tt> folder here yet. Check this is the emulator's data folder: the "
                      "emulator's File menu has an item that opens it.");
        break;
    case ModLayout::Ryujinx:
        if (!has(QStringLiteral("mods")) && last.compare(QLatin1String("contents"), Qt::CaseInsensitive) != 0)
            return tr("There is no <tt>mods</tt> folder here yet. Check this is the Ryujinx data folder: "
                      "File → Open Ryujinx Folder opens it.");
        break;
    case ModLayout::Atmosphere:
        break;
    }
    return {};
}

ModLocation NewModDialog::location() const
{
    return ModLocation{layout(), QDir::fromNativeSeparators(m_base->text().trimmed()), m_titleId->text().toUpper(),
                       layoutHasModName(layout()) ? m_name->text().trimmed() : QString()};
}

QString NewModDialog::romfsPath() const
{
    return modRomfsPath(location());
}

QString NewModDialog::problem() const
{
    const ModLocation l = location();
    if (l.base.isEmpty())
        return tr("Choose the %1").arg(m_baseLabel->text().chopped(1).toLower());
    if (!QFileInfo(l.base).isDir())
        return tr("%1 is not a folder.").arg(QDir::toNativeSeparators(l.base));
    if (!isTitleId(l.titleId))
        return tr("The title ID must have 16 hexadecimal digits.");
    if (const std::optional<QString> id = titleIdFolder(l.layout, l.base); id && *id != l.titleId)
        return tr("The folder you chose is the mod folder of title ID %1, not %2.").arg(*id, l.titleId);
    if (layoutHasModName(l.layout))
        return modNameProblem(l.modName);
    return {};
}

void NewModDialog::updateResult()
{
    const QString issue = problem();
    QPushButton *ok = m_buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(issue.isEmpty());
    if (!issue.isEmpty()) {
        m_result->setText(issue);
        ok->setText(tr("Create"));
        return;
    }
    const QString path = romfsPath();
    const bool exists = QFileInfo(path).isDir();
    QString text = tr("Mod folder: <tt>%1</tt>").arg(QDir::toNativeSeparators(path).toHtmlEscaped());
    if (const QString note = baseNote(); !note.isEmpty())
        text += QStringLiteral("<br><b>%1</b> %2").arg(tr("Check:"), note);
    if (exists)
        text += tr("<br>This folder already exists. Files you change are added to it; nothing in it is removed.");
    if (layout() == ModLayout::Atmosphere)
        text += tr("<br>Atmosphère has one romfs per game, so every mod for this game shares this folder.");
    m_result->setText(text);
    ok->setText(exists ? tr("Use this folder") : tr("Create"));
}

void NewModDialog::accept()
{
    if (!problem().isEmpty())
        return;
    const QString path = romfsPath();
    if (!QDir().mkpath(path)) {
        QMessageBox::warning(this, windowTitle(), tr("Could not create %1.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("newMod/layout"), int(layout()));
    const std::optional<ModLocation> made = parseModFolder(path);
    settings.setValue(settingsKey(layout()), QDir::toNativeSeparators(made ? made->base : location().base));
    QDialog::accept();
}
