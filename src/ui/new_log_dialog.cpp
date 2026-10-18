#include "logger/ui/new_log_dialog.h"

#include "logger/contest/contest_definition_loader.h"
#include "logger/contest/contest_engine.h"

#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace logger {
namespace {

const QString OperatorTag = QStringLiteral("CATEGORY-OPERATOR");
const QString AssistedTag = QStringLiteral("CATEGORY-ASSISTED");
const QString BandTag = QStringLiteral("CATEGORY-BAND");
const QString ModeTag = QStringLiteral("CATEGORY-MODE");
const QString PowerTag = QStringLiteral("CATEGORY-POWER");
const QString StationTag = QStringLiteral("CATEGORY-STATION");
const QString TransmitterTag = QStringLiteral("CATEGORY-TRANSMITTER");
const QString TimeTag = QStringLiteral("CATEGORY-TIME");
const QString OverlayTag = QStringLiteral("CATEGORY-OVERLAY");
const QString OperatorsTag = QStringLiteral("OPERATORS");
const QString ClubTag = QStringLiteral("CLUB");
const QString LocationTag = QStringLiteral("LOCATION");
const QString SoapboxTag = QStringLiteral("SOAPBOX");

QComboBox *createCombo(const QString &objectName, const QStringList &values,
                       QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->setObjectName(objectName);
    combo->addItems(values);
    return combo;
}

void setComboValue(QComboBox *combo, const QString &value)
{
    const int index = combo->findText(value, Qt::MatchFixedString);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    } else if (!value.isEmpty()) {
        combo->addItem(value);
        combo->setCurrentIndex(combo->count() - 1);
    }
}

void installUppercase(QLineEdit *edit)
{
    QObject::connect(edit, &QLineEdit::textEdited, edit, [edit](const QString &text) {
        const QString upper = text.toUpper();
        if (upper != text) {
            const int cursor = edit->cursorPosition();
            edit->setText(upper);
            edit->setCursorPosition(cursor);
        }
    });
}

QString cabrilloMode(const QString &mode)
{
    const QString value = mode.trimmed().toUpper();
    if (value == QStringLiteral("CW") || value == QStringLiteral("RTTY")
        || value == QStringLiteral("FM")) {
        return value;
    }
    if (value == QStringLiteral("SSB") || value == QStringLiteral("PH")
        || value == QStringLiteral("USB") || value == QStringLiteral("LSB")
        || value == QStringLiteral("AM")) {
        return QStringLiteral("SSB");
    }
    return QStringLiteral("DIGI");
}

bool showsInSentForm(const ExchangeFieldDefinition &field)
{
    return field.type != ExchangeFieldType::Rst
        && !(field.type == ExchangeFieldType::Serial && field.generated);
}

QString fieldHint(const ExchangeFieldDefinition &field)
{
    if (field.type == ExchangeFieldType::Enumeration && !field.values.isEmpty()) {
        return field.values.join(QStringLiteral(", "));
    }
    if (field.type == ExchangeFieldType::Integer && field.maximum > 0) {
        return QStringLiteral("%1-%2").arg(field.minimum).arg(field.maximum);
    }
    if (field.type == ExchangeFieldType::Grid) {
        return QStringLiteral("JO90");
    }
    return {};
}

} // namespace

NewLogDialog::NewLogDialog(QVector<NewLogContestOption> options, const Station &station,
                           QStringList existingLogNames, QWidget *parent)
    : QDialog(parent)
    , m_options(std::move(options))
    , m_existingLogNames(std::move(existingLogNames))
{
    std::stable_sort(m_options.begin(), m_options.end(),
                     [](const NewLogContestOption &left, const NewLogContestOption &right) {
                         return left.definition.name.compare(
                                    right.definition.name, Qt::CaseInsensitive) < 0;
                     });
    setWindowTitle(tr("New log"));
    buildUi(false);

    QSettings settings;
    settings.beginGroup(QStringLiteral("newLog"));
    m_callsign->setText(station.callsign.trimmed().toUpper());
    m_club->setText(settings.value(ClubTag).toString());
    m_location->setText(settings.value(LocationTag).toString());
    setComboValue(m_categoryOperator, settings.value(OperatorTag,
                                                     QStringLiteral("SINGLE-OP")).toString());
    setComboValue(m_categoryAssisted, settings.value(AssistedTag,
                                                     QStringLiteral("NON-ASSISTED")).toString());
    setComboValue(m_categoryPower, settings.value(PowerTag, QStringLiteral("HIGH")).toString());
    setComboValue(m_categoryStation, settings.value(StationTag,
                                                    QStringLiteral("FIXED")).toString());
    setComboValue(m_categoryTransmitter, settings.value(TransmitterTag,
                                                        QStringLiteral("ONE")).toString());
    setComboValue(m_categoryOverlay, settings.value(OverlayTag).toString());
    const QString lastContest = settings.value(QStringLiteral("lastContest")).toString();
    settings.endGroup();

    populateContestList();
    if (lastContest.isEmpty() || !selectContest(lastContest)) {
        if (m_contestList->count() > 0) {
            m_contestList->setCurrentRow(0);
        }
    }
    updateOperatorDependentFields();
    updateAcceptState();
    m_callsign->setFocus();
}

NewLogDialog::NewLogDialog(const NewLogContestOption &option, const Contest &log,
                           const QMap<QString, QString> &sentExchange,
                           QStringList existingLogNames, QWidget *parent)
    : QDialog(parent)
    , m_options{option}
    , m_existingLogNames(std::move(existingLogNames))
    , m_editMode(true)
    , m_nameEdited(true)
{
    setWindowTitle(tr("Log properties"));
    buildUi(true);
    m_callsign->setText(log.stationCallsign.trimmed().toUpper());
    if (log.startsAtUtc.has_value() && log.startsAtUtc->isValid()) {
        m_startDate->setDate(log.startsAtUtc->toUTC().date());
    }
    populateContestList();
    m_contestList->setCurrentRow(0);
    rebuildSentExchange(sentExchange);
    m_logName->setText(log.name);
    m_operators->setText(log.properties.value(OperatorsTag));
    m_club->setText(log.properties.value(ClubTag));
    m_location->setText(log.properties.value(LocationTag));
    m_soapbox->setText(log.properties.value(SoapboxTag));
    const QList<std::pair<QComboBox *, QString>> categories{
        {m_categoryOperator, OperatorTag}, {m_categoryAssisted, AssistedTag},
        {m_categoryBand, BandTag}, {m_categoryMode, ModeTag},
        {m_categoryPower, PowerTag}, {m_categoryStation, StationTag},
        {m_categoryTransmitter, TransmitterTag}, {m_categoryTime, TimeTag},
        {m_categoryOverlay, OverlayTag}};
    for (const auto &[combo, tag] : categories) {
        if (log.properties.contains(tag)) {
            setComboValue(combo, log.properties.value(tag));
        }
    }
    updateOperatorDependentFields();
    updateAcceptState();
    m_callsign->setFocus();
}

void NewLogDialog::buildUi(bool editMode)
{
    setObjectName(QStringLiteral("newLogDialog"));
    setMinimumSize(880, 560);

    auto *contestGroup = new QGroupBox(tr("Contest"), this);
    m_filter = new QLineEdit(contestGroup);
    m_filter->setObjectName(QStringLiteral("newLogContestFilter"));
    m_filter->setPlaceholderText(tr("Search contests..."));
    m_filter->setClearButtonEnabled(true);
    m_contestList = new QListWidget(contestGroup);
    m_contestList->setObjectName(QStringLiteral("newLogContestList"));
    m_contestList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_contestInfo = new QLabel(contestGroup);
    m_contestInfo->setObjectName(QStringLiteral("newLogContestInfo"));
    m_contestInfo->setWordWrap(true);
    m_contestInfo->setTextFormat(Qt::RichText);
    m_contestInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_contestInfo->setMinimumHeight(140);
    m_importButton = new QPushButton(tr("&Import YAML..."), contestGroup);
    m_importButton->setObjectName(QStringLiteral("newLogImportButton"));
    auto *contestLayout = new QVBoxLayout(contestGroup);
    contestLayout->addWidget(m_filter);
    contestLayout->addWidget(m_contestList, 1);
    contestLayout->addWidget(m_contestInfo);
    contestLayout->addWidget(m_importButton, 0, Qt::AlignLeft);
    if (editMode) {
        m_filter->hide();
        m_importButton->hide();
        m_contestList->setEnabled(false);
        m_contestList->setMaximumHeight(m_contestList->fontMetrics().height() * 2 + 12);
    }

    auto *stationGroup = new QGroupBox(tr("Station"), this);
    m_startDate = new QDateEdit(QDateTime::currentDateTimeUtc().date(), stationGroup);
    m_startDate->setObjectName(QStringLiteral("newLogStartDate"));
    m_startDate->setCalendarPopup(true);
    m_startDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_callsign = new QLineEdit(stationGroup);
    m_callsign->setObjectName(QStringLiteral("newLogCallsign"));
    m_callsign->setPlaceholderText(tr("Required"));
    m_callsign->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9/]{0,20}")), m_callsign));
    installUppercase(m_callsign);
    m_operators = new QLineEdit(stationGroup);
    m_operators->setObjectName(QStringLiteral("newLogOperators"));
    installUppercase(m_operators);
    m_club = new QLineEdit(stationGroup);
    m_club->setObjectName(QStringLiteral("newLogClub"));
    m_location = new QLineEdit(stationGroup);
    m_location->setObjectName(QStringLiteral("newLogLocation"));
    m_location->setPlaceholderText(tr("e.g. DX, ARRL section, state"));
    installUppercase(m_location);
    m_soapbox = new QLineEdit(stationGroup);
    m_soapbox->setObjectName(QStringLiteral("newLogSoapbox"));
    m_logName = new QLineEdit(stationGroup);
    m_logName->setObjectName(QStringLiteral("newLogName"));
    auto *stationForm = new QFormLayout(stationGroup);
    stationForm->addRow(tr("Start date (UTC):"), m_startDate);
    stationForm->addRow(tr("&Callsign:"), m_callsign);
    stationForm->addRow(tr("&Operators:"), m_operators);
    stationForm->addRow(tr("Club:"), m_club);
    stationForm->addRow(tr("Location:"), m_location);
    stationForm->addRow(tr("Soapbox:"), m_soapbox);
    stationForm->addRow(tr("Log &name:"), m_logName);

    auto *categoryGroup = new QGroupBox(tr("Category"), this);
    m_categoryOperator = createCombo(
        QStringLiteral("newLogCategoryOperator"),
        {QStringLiteral("SINGLE-OP"), QStringLiteral("MULTI-OP"), QStringLiteral("CHECKLOG")},
        categoryGroup);
    m_categoryAssisted = createCombo(
        QStringLiteral("newLogCategoryAssisted"),
        {QStringLiteral("NON-ASSISTED"), QStringLiteral("ASSISTED")}, categoryGroup);
    m_categoryBand = createCombo(QStringLiteral("newLogCategoryBand"), {}, categoryGroup);
    m_categoryMode = createCombo(QStringLiteral("newLogCategoryMode"), {}, categoryGroup);
    m_categoryPower = createCombo(
        QStringLiteral("newLogCategoryPower"),
        {QStringLiteral("HIGH"), QStringLiteral("LOW"), QStringLiteral("QRP")},
        categoryGroup);
    m_categoryStation = createCombo(
        QStringLiteral("newLogCategoryStation"),
        {QStringLiteral("FIXED"), QStringLiteral("PORTABLE"), QStringLiteral("MOBILE"),
         QStringLiteral("ROVER"), QStringLiteral("ROVER-LIMITED"),
         QStringLiteral("ROVER-UNLIMITED"), QStringLiteral("EXPEDITION"),
         QStringLiteral("HQ"), QStringLiteral("SCHOOL"), QStringLiteral("EXPLORER"),
         QStringLiteral("DISTRIBUTED")},
        categoryGroup);
    m_categoryTransmitter = createCombo(
        QStringLiteral("newLogCategoryTransmitter"),
        {QStringLiteral("ONE"), QStringLiteral("TWO"), QStringLiteral("LIMITED"),
         QStringLiteral("UNLIMITED"), QStringLiteral("SWL")},
        categoryGroup);
    m_categoryTime = createCombo(
        QStringLiteral("newLogCategoryTime"),
        {QString(), QStringLiteral("6-HOURS"), QStringLiteral("8-HOURS"),
         QStringLiteral("12-HOURS"), QStringLiteral("24-HOURS")},
        categoryGroup);
    m_categoryOverlay = createCombo(
        QStringLiteral("newLogCategoryOverlay"),
        {QString(), QStringLiteral("CLASSIC"), QStringLiteral("ROOKIE"),
         QStringLiteral("TB-WIRES"), QStringLiteral("YOUTH"),
         QStringLiteral("NOVICE-TECH"), QStringLiteral("OVER-50")},
        categoryGroup);
    auto *categoryForm = new QFormLayout(categoryGroup);
    categoryForm->addRow(tr("Operator:"), m_categoryOperator);
    categoryForm->addRow(tr("Assisted:"), m_categoryAssisted);
    categoryForm->addRow(tr("Band:"), m_categoryBand);
    categoryForm->addRow(tr("Mode:"), m_categoryMode);
    categoryForm->addRow(tr("Power:"), m_categoryPower);
    categoryForm->addRow(tr("Station:"), m_categoryStation);
    categoryForm->addRow(tr("Transmitter:"), m_categoryTransmitter);
    categoryForm->addRow(tr("Time:"), m_categoryTime);
    categoryForm->addRow(tr("Overlay:"), m_categoryOverlay);

    m_sentGroup = new QGroupBox(tr("Sent exchange"), this);
    m_sentForm = new QFormLayout(m_sentGroup);

    auto *rightColumn = new QVBoxLayout;
    rightColumn->addWidget(stationGroup);
    rightColumn->addWidget(m_sentGroup);
    rightColumn->addStretch(1);
    auto *middleColumn = new QVBoxLayout;
    middleColumn->addWidget(categoryGroup);
    middleColumn->addStretch(1);
    auto *columns = new QHBoxLayout;
    columns->addWidget(contestGroup, 3);
    columns->addLayout(rightColumn, 3);
    columns->addLayout(middleColumn, 2);

    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(
        editMode ? tr("&Save") : tr("&Create log"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &NewLogDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(columns, 1);
    layout->addWidget(m_buttons);

    connect(m_filter, &QLineEdit::textChanged, this, &NewLogDialog::populateContestList);
    connect(m_contestList, &QListWidget::currentRowChanged,
            this, &NewLogDialog::contestChanged);
    connect(m_contestList, &QListWidget::itemDoubleClicked, this, [this] {
        m_callsign->setFocus();
        m_callsign->selectAll();
    });
    connect(m_importButton, &QPushButton::clicked, this, &NewLogDialog::importDefinition);
    connect(m_callsign, &QLineEdit::textChanged, this, [this] {
        updateSuggestedName();
        updateOperatorDependentFields();
        updateAcceptState();
    });
    connect(m_startDate, &QDateEdit::dateChanged, this, &NewLogDialog::updateSuggestedName);
    connect(m_logName, &QLineEdit::textEdited, this, [this](const QString &text) {
        m_nameEdited = !text.trimmed().isEmpty();
        if (!m_nameEdited) {
            updateSuggestedName();
        }
        updateAcceptState();
    });
    connect(m_categoryOperator, &QComboBox::currentTextChanged,
            this, &NewLogDialog::updateOperatorDependentFields);
}

void NewLogDialog::populateContestList()
{
    const NewLogContestOption *previous = selectedOption();
    const QString previousSource = previous != nullptr ? previous->source : QString();
    const QString previousId = previous != nullptr ? previous->definition.id : QString();
    const QString filter = m_filter->text().trimmed();
    {
        const QSignalBlocker blocker(m_contestList);
        m_contestList->clear();
        for (int index = 0; index < m_options.size(); ++index) {
            const ContestDefinition &definition = m_options.at(index).definition;
            if (!filter.isEmpty()
                && !definition.name.contains(filter, Qt::CaseInsensitive)
                && !definition.id.contains(filter, Qt::CaseInsensitive)
                && !definition.description.contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
            auto *item = new QListWidgetItem(definition.name, m_contestList);
            item->setData(Qt::UserRole, index);
            item->setToolTip(QStringLiteral("%1 (%2)").arg(definition.id,
                                                           m_options.at(index).source));
            if (definition.id == previousId && m_options.at(index).source == previousSource) {
                m_contestList->setCurrentItem(item);
            }
        }
        if (m_contestList->currentRow() < 0 && m_contestList->count() > 0) {
            m_contestList->setCurrentRow(0);
        }
    }
    contestChanged();
}

const NewLogContestOption *NewLogDialog::selectedOption() const
{
    if (m_contestList == nullptr) {
        return nullptr;
    }
    const QListWidgetItem *item = m_contestList->currentItem();
    if (item == nullptr) {
        return nullptr;
    }
    const int index = item->data(Qt::UserRole).toInt();
    return index >= 0 && index < m_options.size() ? &m_options.at(index) : nullptr;
}

bool NewLogDialog::selectContest(const QString &definitionId)
{
    for (int row = 0; row < m_contestList->count(); ++row) {
        const int index = m_contestList->item(row)->data(Qt::UserRole).toInt();
        if (m_options.at(index).definition.id.compare(definitionId, Qt::CaseInsensitive)
            == 0) {
            m_contestList->setCurrentRow(row);
            return true;
        }
    }
    return false;
}

void NewLogDialog::contestChanged()
{
    const NewLogContestOption *option = selectedOption();
    if (option == nullptr) {
        m_contestInfo->setText(m_options.isEmpty()
                                   ? tr("No contest definitions are installed.")
                                   : tr("No contest matches the search."));
        rebuildCategoryChoices();
        rebuildSentExchange({});
        updateSuggestedName();
        updateAcceptState();
        return;
    }
    const ContestDefinition &definition = option->definition;
    const auto fieldLabels = [](const QVector<ExchangeFieldDefinition> &fields) {
        QStringList labels;
        for (const ExchangeFieldDefinition &field : fields) {
            labels.append(field.label.toHtmlEscaped());
        }
        return labels.isEmpty() ? QStringLiteral("-") : labels.join(QStringLiteral(", "));
    };
    m_contestInfo->setText(
        QStringLiteral("<b>%1</b><br><small>%2</small><p>%3</p>"
                       "<b>%4</b> %5<br><b>%6</b> %7<br>"
                       "<b>%8</b> %9<br><b>%10</b> %11")
            .arg(definition.name.toHtmlEscaped(),
                 QStringLiteral("%1 &middot; %2").arg(definition.id.toHtmlEscaped(),
                                                       option->source.toHtmlEscaped()),
                 definition.description.toHtmlEscaped(), tr("Bands:"),
                 definition.bands.join(QStringLiteral(", ")).toHtmlEscaped(), tr("Modes:"),
                 definition.modes.join(QStringLiteral(", ")).toHtmlEscaped(),
                 tr("Sent:"))
            .arg(fieldLabels(definition.sentExchange), tr("Received:"),
                 fieldLabels(definition.receivedExchange)));
    rebuildCategoryChoices();
    QMap<QString, QString> previousValues;
    for (auto it = m_sentEdits.cbegin(); it != m_sentEdits.cend(); ++it) {
        if (!it.value()->text().trimmed().isEmpty()) {
            previousValues.insert(it.key(), it.value()->text().trimmed());
        }
    }
    rebuildSentExchange(previousValues);
    updateSuggestedName();
    updateAcceptState();
}

void NewLogDialog::rebuildCategoryChoices()
{
    const NewLogContestOption *option = selectedOption();
    const QString previousBand = m_categoryBand->currentText();
    const QString previousMode = m_categoryMode->currentText();
    QStringList bands{QStringLiteral("ALL")};
    QStringList modes;
    if (option != nullptr) {
        for (const QString &band : option->definition.bands) {
            bands.append(band.trimmed().toUpper());
        }
        for (const QString &mode : option->definition.modes) {
            const QString mapped = cabrilloMode(mode);
            if (!modes.contains(mapped)) {
                modes.append(mapped);
            }
        }
    }
    if (modes.size() != 1) {
        modes.prepend(QStringLiteral("MIXED"));
    }
    m_categoryBand->clear();
    m_categoryBand->addItems(bands);
    m_categoryMode->clear();
    m_categoryMode->addItems(modes);
    if (bands.contains(previousBand)) {
        m_categoryBand->setCurrentText(previousBand);
    }
    if (modes.contains(previousMode)) {
        m_categoryMode->setCurrentText(previousMode);
    }
}

void NewLogDialog::rebuildSentExchange(const QMap<QString, QString> &values)
{
    while (m_sentForm->rowCount() > 0) {
        m_sentForm->removeRow(0);
    }
    m_sentEdits.clear();
    const NewLogContestOption *option = selectedOption();
    if (option != nullptr) {
        for (const ExchangeFieldDefinition &field : option->definition.sentExchange) {
            if (!showsInSentForm(field)) {
                continue;
            }
            auto *edit = new QLineEdit(m_sentGroup);
            edit->setObjectName(QStringLiteral("newLogSent_%1").arg(field.name));
            edit->setText(values.value(field.name, field.defaultValue));
            edit->setPlaceholderText(field.required ? tr("Required") : tr("Optional"));
            const QString hint = fieldHint(field);
            if (!hint.isEmpty()) {
                edit->setToolTip(hint);
                edit->setPlaceholderText(QStringLiteral("%1 (%2)")
                                             .arg(edit->placeholderText(), hint));
            }
            if (field.width > 0) {
                edit->setMaxLength(field.width);
            }
            installUppercase(edit);
            connect(edit, &QLineEdit::textChanged, this, &NewLogDialog::updateAcceptState);
            m_sentForm->addRow(field.required ? tr("%1:").arg(field.label)
                                              : tr("%1 (optional):").arg(field.label),
                               edit);
            m_sentEdits.insert(field.name, edit);
        }
    }
    if (m_sentEdits.isEmpty()) {
        auto *none = new QLabel(tr("Generated automatically (RST, serial number)."),
                                m_sentGroup);
        none->setEnabled(false);
        m_sentForm->addRow(none);
    }
}

void NewLogDialog::updateSuggestedName()
{
    if (m_nameEdited) {
        return;
    }
    const NewLogContestOption *option = selectedOption();
    if (option == nullptr) {
        m_logName->clear();
        m_lastSuggestedName.clear();
        return;
    }
    const QString callsign = m_callsign->text().trimmed().toUpper();
    const QString base = QStringLiteral("%1%2 %3")
        .arg(callsign.isEmpty() ? QString() : callsign + QLatin1Char(' '),
             option->definition.name)
        .arg(m_startDate->date().year());
    QString suggested = base;
    const auto taken = [this](const QString &candidate) {
        return std::any_of(m_existingLogNames.cbegin(), m_existingLogNames.cend(),
                           [&candidate](const QString &name) {
                               return name.compare(candidate, Qt::CaseInsensitive) == 0;
                           });
    };
    for (int number = 2; taken(suggested); ++number) {
        suggested = QStringLiteral("%1 (%2)").arg(base).arg(number);
    }
    m_lastSuggestedName = suggested;
    m_logName->setText(suggested);
}

void NewLogDialog::updateOperatorDependentFields()
{
    const bool multiOperator =
        m_categoryOperator->currentText() == QStringLiteral("MULTI-OP");
    m_categoryTransmitter->setEnabled(multiOperator);
    if (!multiOperator) {
        setComboValue(m_categoryTransmitter, QStringLiteral("ONE"));
    }
    const QString callsign = m_callsign->text().trimmed().toUpper();
    m_operators->setPlaceholderText(
        multiOperator ? tr("Space-separated operator callsigns")
                      : (callsign.isEmpty() ? tr("Defaults to the station callsign")
                                            : callsign));
}

void NewLogDialog::updateAcceptState()
{
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(
        selectedOption() != nullptr && !m_callsign->text().trimmed().isEmpty()
        && !m_logName->text().trimmed().isEmpty());
}

QStringList NewLogDialog::validationProblems() const
{
    QStringList problems;
    const NewLogContestOption *option = selectedOption();
    if (option == nullptr) {
        problems.append(tr("Select a contest."));
        return problems;
    }
    if (!ContestEngine::isValidCallsign(m_callsign->text().trimmed().toUpper())) {
        problems.append(tr("Enter a valid station callsign."));
    }
    if (m_logName->text().trimmed().isEmpty()) {
        problems.append(tr("Enter a log name."));
    }
    const ContestDefinition &definition = option->definition;
    if (definition.bands.isEmpty() || definition.modes.isEmpty()) {
        return problems;
    }
    const ContestEvaluationResult result = ContestEngine().evaluate(
        definition, QStringLiteral("K1ABC"), definition.bands.constFirst(),
        definition.modes.constFirst(), {}, {}, definition.serialStart, sentExchange());
    for (const ContestValidationIssue &issue : result.issues) {
        if (!issue.field.startsWith(QStringLiteral("sent."))) {
            continue;
        }
        const QString name = issue.field.mid(5);
        const auto field = std::find_if(
            definition.sentExchange.cbegin(), definition.sentExchange.cend(),
            [&name](const ExchangeFieldDefinition &candidate) {
                return candidate.name == name;
            });
        if (field != definition.sentExchange.cend() && showsInSentForm(*field)) {
            problems.append(tr("Sent %1 %2.").arg(field->label, issue.message));
        }
    }
    return problems;
}

void NewLogDialog::accept()
{
    const QStringList problems = validationProblems();
    if (!problems.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), problems.join(QLatin1Char('\n')));
        if (!ContestEngine::isValidCallsign(m_callsign->text().trimmed().toUpper())) {
            m_callsign->setFocus();
            m_callsign->selectAll();
        }
        return;
    }
    if (!m_editMode) {
        const Contest values = log();
        QSettings settings;
        settings.beginGroup(QStringLiteral("newLog"));
        settings.setValue(QStringLiteral("lastContest"), selectedOption()->definition.id);
        for (const QString &tag : {OperatorTag, AssistedTag, PowerTag, StationTag,
                                   TransmitterTag, OverlayTag, ClubTag, LocationTag}) {
            settings.setValue(tag, values.properties.value(tag));
        }
        settings.endGroup();
    }
    QDialog::accept();
}

Contest NewLogDialog::log() const
{
    Contest result;
    result.name = m_logName->text().trimmed();
    result.stationCallsign = m_callsign->text().trimmed().toUpper();
    result.startsAtUtc = QDateTime(m_startDate->date(), QTime(0, 0), QTimeZone::utc());
    const QList<std::pair<QString, QString>> values{
        {OperatorTag, m_categoryOperator->currentText()},
        {AssistedTag, m_categoryAssisted->currentText()},
        {BandTag, m_categoryBand->currentText()},
        {ModeTag, m_categoryMode->currentText()},
        {PowerTag, m_categoryPower->currentText()},
        {StationTag, m_categoryStation->currentText()},
        {TransmitterTag, m_categoryTransmitter->currentText()},
        {TimeTag, m_categoryTime->currentText()},
        {OverlayTag, m_categoryOverlay->currentText()},
        {OperatorsTag, m_operators->text().simplified().toUpper()},
        {ClubTag, m_club->text().trimmed()},
        {LocationTag, m_location->text().trimmed().toUpper()},
        {SoapboxTag, m_soapbox->text().trimmed()}};
    for (const auto &[tag, value] : values) {
        if (!value.trimmed().isEmpty()) {
            result.properties.insert(tag, value.trimmed());
        }
    }
    return result;
}

QMap<QString, QString> NewLogDialog::sentExchange() const
{
    QMap<QString, QString> result;
    for (auto it = m_sentEdits.cbegin(); it != m_sentEdits.cend(); ++it) {
        result.insert(it.key(), it.value()->text().trimmed().toUpper());
    }
    return result;
}

void NewLogDialog::importDefinition()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("Import contest definition"), {},
        tr("Contest definitions (*.yaml *.yml);;All files (*)"));
    if (filePath.isEmpty()) {
        return;
    }
    QFile source(filePath);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("Unable to read definition"),
                              QStringLiteral("%1: %2").arg(filePath, source.errorString()));
        return;
    }
    NewLogContestOption option;
    option.yaml = QString::fromUtf8(source.readAll());
    option.source = filePath;
    QString error;
    if (!ContestDefinitionLoader().loadText(option.yaml, filePath, option.definition,
                                            &error)) {
        QMessageBox::critical(this, tr("Invalid contest definition"), error);
        return;
    }
    m_options.append(option);
    const int index = static_cast<int>(m_options.size()) - 1;
    {
        const QSignalBlocker blocker(m_filter);
        m_filter->clear();
    }
    populateContestList();
    for (int row = 0; row < m_contestList->count(); ++row) {
        if (m_contestList->item(row)->data(Qt::UserRole).toInt() == index) {
            m_contestList->setCurrentRow(row);
            break;
        }
    }
}

} // namespace logger
