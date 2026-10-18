#pragma once

#include "logger/domain/contest_definition.h"
#include "logger/domain/types.h"

#include <QDialog>
#include <QMap>
#include <QStringList>
#include <QVector>

class QComboBox;
class QDateEdit;
class QDialogButtonBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace logger {

struct NewLogContestOption {
    ContestDefinition definition;
    QString yaml;
    QString source;
};

// DXLog-style "contest properties" window: used for File > New log and for
// editing the properties of the active log.
class NewLogDialog final : public QDialog {
    Q_OBJECT

public:
    // New log: choose a contest from options and enter the station callsign.
    NewLogDialog(QVector<NewLogContestOption> options, const Station &station,
                 QStringList existingLogNames, QWidget *parent = nullptr);
    // Log properties: the contest is fixed, the remaining fields are editable.
    NewLogDialog(const NewLogContestOption &option, const Contest &log,
                 const QMap<QString, QString> &sentExchange,
                 QStringList existingLogNames, QWidget *parent = nullptr);

    const NewLogContestOption *selectedOption() const;
    bool selectContest(const QString &definitionId);
    // Returns name, stationCallsign, startsAtUtc and properties.
    Contest log() const;
    QMap<QString, QString> sentExchange() const;

public slots:
    void accept() override;

private:
    void buildUi(bool editMode);
    void populateContestList();
    void contestChanged();
    void rebuildCategoryChoices();
    void rebuildSentExchange(const QMap<QString, QString> &values);
    void updateSuggestedName();
    void updateOperatorDependentFields();
    void updateAcceptState();
    void importDefinition();
    QStringList validationProblems() const;

    QVector<NewLogContestOption> m_options;
    QStringList m_existingLogNames;
    bool m_editMode = false;
    bool m_nameEdited = false;
    QString m_lastSuggestedName;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_contestList = nullptr;
    QLabel *m_contestInfo = nullptr;
    QDateEdit *m_startDate = nullptr;
    QLineEdit *m_callsign = nullptr;
    QLineEdit *m_operators = nullptr;
    QLineEdit *m_club = nullptr;
    QLineEdit *m_location = nullptr;
    QLineEdit *m_soapbox = nullptr;
    QLineEdit *m_logName = nullptr;
    QComboBox *m_categoryOperator = nullptr;
    QComboBox *m_categoryAssisted = nullptr;
    QComboBox *m_categoryBand = nullptr;
    QComboBox *m_categoryMode = nullptr;
    QComboBox *m_categoryPower = nullptr;
    QComboBox *m_categoryStation = nullptr;
    QComboBox *m_categoryTransmitter = nullptr;
    QComboBox *m_categoryTime = nullptr;
    QComboBox *m_categoryOverlay = nullptr;
    QGroupBox *m_sentGroup = nullptr;
    QFormLayout *m_sentForm = nullptr;
    QMap<QString, QLineEdit *> m_sentEdits;
    QPushButton *m_importButton = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace logger
