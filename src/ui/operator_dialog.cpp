#include "logger/ui/operator_dialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace logger {

OperatorDialog::OperatorDialog(const Station &station, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Operator and station"));

    m_callsign = new QLineEdit(station.callsign, this);
    m_callsign->setObjectName(QStringLiteral("operatorCallsign"));
    m_operatorName = new QLineEdit(station.operatorName, this);
    m_operatorName->setObjectName(QStringLiteral("operatorName"));
    m_grid = new QLineEdit(station.grid, this);
    m_grid->setObjectName(QStringLiteral("operatorGrid"));
    m_powerClass = new QLineEdit(station.powerClass, this);
    m_powerClass->setObjectName(QStringLiteral("operatorPowerClass"));
    m_powerClass->setPlaceholderText(QStringLiteral("HIGH / LOW / QRP"));
    m_callsign->setPlaceholderText(tr("Required"));

    auto *form = new QFormLayout;
    form->addRow(tr("Station callsign:"), m_callsign);
    form->addRow(tr("Operator name:"), m_operatorName);
    form->addRow(tr("Grid locator:"), m_grid);
    form->addRow(tr("Power class:"), m_powerClass);

    auto *addressGroup = new QGroupBox(tr("Cabrillo contact data"), this);
    m_email = new QLineEdit(station.email, addressGroup);
    m_email->setObjectName(QStringLiteral("operatorEmail"));
    m_address = new QPlainTextEdit(station.address, addressGroup);
    m_address->setObjectName(QStringLiteral("operatorAddress"));
    m_address->setTabChangesFocus(true);
    m_address->setFixedHeight(m_address->fontMetrics().lineSpacing() * 3 + 12);
    m_city = new QLineEdit(station.city, addressGroup);
    m_city->setObjectName(QStringLiteral("operatorCity"));
    m_stateProvince = new QLineEdit(station.stateProvince, addressGroup);
    m_stateProvince->setObjectName(QStringLiteral("operatorStateProvince"));
    m_postalCode = new QLineEdit(station.postalCode, addressGroup);
    m_postalCode->setObjectName(QStringLiteral("operatorPostalCode"));
    m_country = new QLineEdit(station.country, addressGroup);
    m_country->setObjectName(QStringLiteral("operatorCountry"));
    auto *addressForm = new QFormLayout(addressGroup);
    addressForm->addRow(tr("E-mail:"), m_email);
    addressForm->addRow(tr("Address:"), m_address);
    addressForm->addRow(tr("City:"), m_city);
    addressForm->addRow(tr("State/province:"), m_stateProvince);
    addressForm->addRow(tr("Postal code:"), m_postalCode);
    addressForm->addRow(tr("Country:"), m_country);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(addressGroup);
    layout->addWidget(buttons);
}

Station OperatorDialog::station() const
{
    Station result;
    result.callsign = m_callsign->text().trimmed().toUpper();
    result.operatorName = m_operatorName->text().trimmed();
    result.grid = m_grid->text().trimmed().toUpper();
    result.powerClass = m_powerClass->text().trimmed().toUpper();
    result.email = m_email->text().trimmed();
    QStringList addressLines;
    for (const QString &line : m_address->toPlainText().split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty()) {
            addressLines.append(line.trimmed());
        }
    }
    result.address = addressLines.join(QLatin1Char('\n'));
    result.city = m_city->text().trimmed();
    result.stateProvince = m_stateProvince->text().trimmed();
    result.postalCode = m_postalCode->text().trimmed();
    result.country = m_country->text().trimmed();
    return result;
}

} // namespace logger