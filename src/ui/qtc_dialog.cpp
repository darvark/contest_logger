#include "logger/ui/qtc_dialog.h"

#include "logger/contest/qtc_service.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace logger {

QtcDialog::QtcDialog(const QVector<Qso> &qsos, QWidget *parent)
    : QDialog(parent), m_qsos(qsos)
{
    setWindowTitle(tr("WAE QTC exchange"));
    resize(680, 400);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_partner = new QComboBox(this);
    m_partner->setObjectName(QStringLiteral("qtcPartnerQso"));
    for (qsizetype index = 0; index < m_qsos.size(); ++index) {
        const Qso &qso = m_qsos.at(index);
        m_partner->addItem(
            QStringLiteral("%1  %2  %3")
                .arg(qso.timestampUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                     qso.callsign, qso.band),
            static_cast<int>(index));
    }
    form->addRow(tr("Partner QSO:"), m_partner);
    m_direction = new QComboBox(this);
    m_direction->setObjectName(QStringLiteral("qtcDirection"));
    m_direction->addItem(tr("Send QTC"), static_cast<int>(QtcDirection::Sent));
    m_direction->addItem(tr("Receive QTC"), static_cast<int>(QtcDirection::Received));
    form->addRow(tr("Direction:"), m_direction);
    m_groupNumber = new QSpinBox(this);
    m_groupNumber->setObjectName(QStringLiteral("qtcGroupNumber"));
    m_groupNumber->setRange(1, 10);
    form->addRow(tr("QTC group number:"), m_groupNumber);
    layout->addLayout(form);
    m_messages = new QTableWidget(10, 3, this);
    m_messages->setObjectName(QStringLiteral("qtcMessages"));
    m_messages->setHorizontalHeaderLabels(
        {tr("UTC (HHMM)"), tr("Callsign"), tr("Serial")});
    m_messages->horizontalHeader()->setStretchLastSection(true);
    m_messages->verticalHeader()->setVisible(false);
    for (int row = 0; row < m_messages->rowCount(); ++row) {
        m_messages->setItem(row, 0, new QTableWidgetItem);
        m_messages->setItem(row, 1, new QTableWidgetItem);
        m_messages->setItem(row, 2, new QTableWidgetItem);
    }
    layout->addWidget(m_messages);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("saveQtc"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QtcTransfer value;
        QString error;
        if (!transfer(&value, &error)) {
            QMessageBox::warning(this, tr("Invalid QTC"), error);
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    const auto updateGroupNumber = [this] {
        const Qso *qso = partnerQso();
        if (qso == nullptr) {
            return;
        }
        const auto direction = static_cast<QtcDirection>(
            m_direction->currentData().toInt());
        int next = 1;
        for (const Qso &entry : m_qsos) {
            for (const QtcTransfer &existing : entry.qtcTransfers) {
                if (existing.direction == direction
                    && existing.partnerCallsign.compare(
                           qso->callsign, Qt::CaseInsensitive) == 0) {
                    next = qMax(next, existing.groupNumber + 1);
                }
            }
        }
        m_groupNumber->setValue(qMin(next, 10));
    };
    connect(m_partner, qOverload<int>(&QComboBox::currentIndexChanged),
            this, updateGroupNumber);
    connect(m_direction, qOverload<int>(&QComboBox::currentIndexChanged),
            this, updateGroupNumber);
    updateGroupNumber();
}

const Qso *QtcDialog::partnerQso() const
{
    const int index = m_partner->currentData().toInt();
    return index >= 0 && index < m_qsos.size() ? &m_qsos.at(index) : nullptr;
}

bool QtcDialog::transfer(QtcTransfer *result, QString *error) const
{
    if (result == nullptr) {
        if (error != nullptr) {
            *error = tr("QTC result destination is missing.");
        }
        return false;
    }
    const Qso *partner = partnerQso();
    if (partner == nullptr) {
        if (error != nullptr) {
            *error = tr("Select the QSO exchanged with the QTC partner.");
        }
        return false;
    }
    QVector<Qso> currentLog = m_qsos;
    QtcTransfer value = QtcService::createTransfer(
        *partner, static_cast<QtcDirection>(m_direction->currentData().toInt()),
        currentLog);
    value.groupNumber = m_groupNumber->value();
    for (int row = 0; row < m_messages->rowCount(); ++row) {
        const QString time = m_messages->item(row, 0)->text().trimmed();
        const QString callsign = m_messages->item(row, 1)->text().trimmed();
        const QString serialText = m_messages->item(row, 2)->text().trimmed();
        if (time.isEmpty() && callsign.isEmpty() && serialText.isEmpty()) {
            continue;
        }
        const QTime parsedTime = QTime::fromString(time, QStringLiteral("HHmm"));
        bool serialOk = false;
        const int serial = serialText.toInt(&serialOk);
        if (!parsedTime.isValid() || !serialOk) {
            if (error != nullptr) {
                *error = tr("Message %1 needs a valid UTC time (HHMM) and serial.")
                             .arg(row + 1);
            }
            return false;
        }
        value.messages.append(QtcMessage{
            QDateTime(partner->timestampUtc.toUTC().date(), parsedTime, Qt::UTC),
            callsign.trimmed().toUpper(), serial
        });
    }
    QVector<QtcTransfer> existing;
    for (const Qso &qso : m_qsos) {
        for (const QtcTransfer &transfer : qso.qtcTransfers) {
            if (transfer.partnerCallsign.compare(
                    partner->callsign, Qt::CaseInsensitive) == 0
                && transfer.direction == value.direction) {
                existing.append(transfer);
            }
        }
    }
    if (!QtcService::validate(*partner, value, existing, error)) {
        return false;
    }
    *result = std::move(value);
    return true;
}

} // namespace logger
