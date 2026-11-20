#include "batchexportdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

BatchExportDialog::BatchExportDialog(const QString &what, const QString &sourceFolder, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Extract"));
    QSettings settings;
    settings.beginGroup(QStringLiteral("extract"));
    const BatchOptions defaults;
    const int savedKinds = settings.value(QStringLiteral("kinds"), defaults.kinds).toInt();

    auto *summary = new QLabel(tr("<b>%1</b><br>Files are saved in common formats when the app can convert them; "
                                  "archives are opened and the files inside are extracted too.")
                                   .arg(what.toHtmlEscaped()));
    summary->setWordWrap(true);

    auto *kindsBox = new QGroupBox(tr("Convert to common formats"));
    auto *kindsLayout = new QVBoxLayout(kindsBox);
    const std::tuple<BatchKind, QString, QString> kinds[] = {
        {BatchPicture, tr("Pictures and textures → PNG"), tr("Save every picture and texture found as a PNG file")},
        {BatchModel, tr("3D models → glTF (.glb), with the textures found next to them"),
         tr("Save every 3D model as a .glb file that opens in Blender and other 3D apps")},
        {BatchSound, tr("Sounds → WAV"), tr("Save every sound as a WAV file that any audio player can open")},
        {BatchJson, tr("Data → plain JSON (formats that support it)"),
         tr("Save data files as readable JSON text, for the formats that can be written that way")},
        {BatchCsv, tr("Tables → CSV (every table of each recognised file)"),
         tr("Save each table as a CSV file that opens in spreadsheet apps")},
    };
    for (const auto &[kind, text, tip] : kinds) {
        auto *box = new QCheckBox(text);
        box->setToolTip(tip);
        box->setChecked(savedKinds & kind);
        connect(box, &QCheckBox::toggled, this, &BatchExportDialog::updateButtons);
        kindsLayout->addWidget(box);
        m_kinds[kind] = box;
    }

    m_output = new QLineEdit(settings.value(QStringLiteral("folder"), QSettings().value(QStringLiteral("batchExport/folder")))
                                 .toString());
    m_output->setPlaceholderText(tr("Folder to save the files in"));
    m_output->setToolTip(tr("The folder the extracted files are saved in. Type or paste a path, or use Browse"));
    connect(m_output, &QLineEdit::textChanged, this, &BatchExportDialog::updateButtons);
    auto *browse = new QPushButton(tr("Browse…"));
    browse->setToolTip(tr("Pick the folder to save the extracted files in"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString start = m_output->text().isEmpty() ? QDir::homePath() : m_output->text();
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Save the files in"), start);
        if (!folder.isEmpty())
            setOutputFolder(folder);
    });
    auto *outputRow = new QHBoxLayout;
    outputRow->addWidget(m_output, 1);
    outputRow->addWidget(browse);

    auto *otherBox = new QGroupBox(tr("Archives and other files"));
    auto *otherLayout = new QVBoxLayout(otherBox);
    m_openArchives = new QCheckBox(tr("Open archives and extract the files inside them"));
    m_openArchives->setToolTip(tr("Archives (RPF, SARC, packages, texture dictionaries, sound banks…) become folders with "
                                  "their files, converted like any other file. When unticked, archives are saved as "
                                  "they are"));
    m_openArchives->setChecked(settings.value(QStringLiteral("openArchives"), defaults.openArchives).toBool());
    m_keepUnconverted = new QCheckBox(tr("Save files that cannot be converted as they are"));
    m_keepUnconverted->setToolTip(tr("Files in formats the app cannot convert are copied unchanged. When unticked, "
                                     "they are skipped"));
    m_keepUnconverted->setChecked(settings.value(QStringLiteral("keepUnconverted"), defaults.keepUnconverted).toBool());
    m_keepOriginals = new QCheckBox(tr("Also save the original of each converted file"));
    m_keepOriginals->setToolTip(tr("Keep the game's own file (for example the .dds or .awc) next to the PNG, WAV or "
                                   "glTF made from it"));
    m_keepOriginals->setChecked(settings.value(QStringLiteral("keepOriginals"), defaults.keepOriginals).toBool());
    for (QCheckBox *box : {m_openArchives, m_keepUnconverted, m_keepOriginals}) {
        connect(box, &QCheckBox::toggled, this, &BatchExportDialog::updateButtons);
        otherLayout->addWidget(box);
    }

    m_keepFolders = new QCheckBox(tr("Keep the folder structure"));
    m_keepFolders->setToolTip(sourceFolder.isEmpty()
                                  ? tr("Files keep the folders they have inside the archive")
                                  : tr("Selected folders keep their name and subfolders inside the output folder "
                                       "(paths are taken from %1)")
                                        .arg(QDir::toNativeSeparators(sourceFolder)));
    m_keepFolders->setChecked(settings.value(QStringLiteral("keepFolders"), true).toBool());
    m_replace = new QCheckBox(tr("Replace files that already exist"));
    m_replace->setToolTip(tr("When unticked, files already in the output folder are left as they are"));
    m_replace->setChecked(settings.value(QStringLiteral("replace"), false).toBool());

    auto *form = new QFormLayout;
    form->addRow(tr("Save in:"), outputRow);
    form->addRow(QString(), m_keepFolders);
    form->addRow(QString(), m_replace);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Extract"));
    m_buttons->button(QDialogButtonBox::Ok)->setToolTip(tr("Extract the selected files into the output folder"));
    m_buttons->button(QDialogButtonBox::Cancel)->setToolTip(tr("Close without extracting anything"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(summary);
    layout->addWidget(kindsBox);
    layout->addWidget(otherBox);
    layout->addLayout(form);
    layout->addWidget(m_buttons);
    updateButtons();
}

BatchOptions BatchExportDialog::options() const
{
    BatchOptions options;
    options.kinds = 0;
    for (const auto &[kind, box] : m_kinds)
        if (box->isChecked())
            options.kinds |= kind;
    options.outputFolder = QDir::fromNativeSeparators(m_output->text().trimmed());
    options.keepFolders = m_keepFolders->isChecked();
    options.replaceExisting = m_replace->isChecked();
    options.openArchives = m_openArchives->isChecked();
    options.keepUnconverted = m_keepUnconverted->isChecked();
    options.keepOriginals = m_keepOriginals->isChecked();
    return options;
}

void BatchExportDialog::setOutputFolder(const QString &folder)
{
    m_output->setText(QDir::toNativeSeparators(folder));
}

void BatchExportDialog::accept()
{
    const BatchOptions chosen = options();
    QSettings settings;
    settings.beginGroup(QStringLiteral("extract"));
    settings.setValue(QStringLiteral("kinds"), chosen.kinds);
    settings.setValue(QStringLiteral("openArchives"), chosen.openArchives);
    settings.setValue(QStringLiteral("keepUnconverted"), chosen.keepUnconverted);
    settings.setValue(QStringLiteral("keepOriginals"), chosen.keepOriginals);
    settings.setValue(QStringLiteral("folder"), QDir::toNativeSeparators(chosen.outputFolder));
    settings.setValue(QStringLiteral("keepFolders"), chosen.keepFolders);
    settings.setValue(QStringLiteral("replace"), chosen.replaceExisting);
    QDialog::accept();
}

void BatchExportDialog::updateButtons()
{
    bool anyKind = false;
    for (const auto &[kind, box] : m_kinds)
        anyKind = anyKind || box->isChecked();
    const bool anything = anyKind || m_keepUnconverted->isChecked() || m_keepOriginals->isChecked();
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(anything && !m_output->text().trimmed().isEmpty());
}
