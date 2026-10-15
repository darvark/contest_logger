#pragma once

#include "logger/domain/types.h"

#include <QDialog>

class QLineEdit;

namespace logger {

class OperatorDialog final : public QDialog {
    Q_OBJECT

public:
    explicit OperatorDialog(const Station &station, QWidget *parent = nullptr);
    Station station() const;

private:
    QLineEdit *m_callsign = nullptr;
    QLineEdit *m_operatorName = nullptr;
    QLineEdit *m_grid = nullptr;
    QLineEdit *m_powerClass = nullptr;
};

} // namespace logger
