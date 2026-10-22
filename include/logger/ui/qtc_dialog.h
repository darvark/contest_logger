#pragma once

#include "logger/domain/types.h"

#include <QDialog>
#include <QVector>

class QComboBox;
class QSpinBox;
class QTableWidget;

namespace logger {

class QtcDialog final : public QDialog {
public:
    explicit QtcDialog(const QVector<Qso> &qsos, QWidget *parent = nullptr);

    const Qso *partnerQso() const;
    bool transfer(QtcTransfer *result, QString *error = nullptr) const;

private:
    QVector<Qso> m_qsos;
    QComboBox *m_partner = nullptr;
    QComboBox *m_direction = nullptr;
    QSpinBox *m_groupNumber = nullptr;
    QTableWidget *m_messages = nullptr;
};

} // namespace logger
