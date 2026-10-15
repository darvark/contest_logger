#include "logger/ui/operator_dialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

namespace logger {

OperatorDialog::OperatorDialog(const Station &station, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Operator and station"));

    m_callsign = new QLineEdit(station.callsign, this);
    m_operatorName = new QLineEdit(station.operatorName, this);
    m_grid = new QLineEdit(station.grid, this);
    m_powerClass = new QLineEdit(station.powerClass, this);
    m_callsign->setPlaceholderText(tr("Required"));

    auto *form = new QFormLayout;
    form->addRow(tr("Station callsign:"), m_callsign);
    form->addRow(tr("Operator name:"), m_operatorName);
    form->addRow(tr("Grid locator:"), m_grid);
    form->addRow(tr("Power class:"), m_powerClass);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

Station OperatorDialog::station() const
{
    return {
        m_callsign->text().trimmed().toUpper(),
        m_operatorName->text().trimmed(),
        m_grid->text().trimmed().toUpper(),
        m_powerClass->text().trimmed().toUpper()
    };
}

} // namespace logger
