#pragma once

#include "logger/cluster/dx_cluster_service.h"
#include "logger/contest/contest_definition_loader.h"
#include "logger/contest/contest_engine.h"
#include "logger/cw/cw_keyer_service.h"
#include "logger/data/log_exporter.h"
#include "logger/data/sqlite_repository.h"
#include "logger/data/support_data_parser.h"
#include "logger/domain/types.h"
#include "logger/radio/radio_service.h"

#include <QDateTime>
#include <QHash>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QVector>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QHBoxLayout;
class QCheckBox;
class QComboBox;
class QTimer;
class QSpinBox;
class QFrame;
class QPushButton;
class QListWidget;
class QEvent;
class QDockWidget;
class QCloseEvent;

namespace logger {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(SqliteRepository &repository, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void configureOperator();
    void configureFunctionKeys();
    void configureEsmMessages();
    void configureRadio();
    void configureRadioB();
    void exportCabrillo();
    void exportAdif();
    void backupDatabase();
    void configureDxCluster();
    void disconnectDxClusters();
    void sendDxClusterCommand();
    void configureCwKeyer();
    void selectContest(int index);
    void createNewLog();
    void editLogProperties();
    void importContestDefinition();
    void importCallHistory();
    void importMasterCpy();
    void importCtyDat();
    void updateCallLookup(int vfo);
    void updateSentReportForMode(int vfo);
    void resetWindowLayout();
    void logQso();
    void editSelectedQso(int row, int column);
    void updateDupeIndicator();
    void updateDxClusterStatus(const logger::DxClusterStatus &status);
    void updateDxClusterStatus(logger::DxClusterController *controller,
                               const logger::DxClusterStatus &status);
    void receiveSpot(const logger::Spot &spot);
    void refreshBandmap();
    void activateSpot(int row, int column);
    void activateSpotId(qint64 spotId, int radio = -1, int context = -1);
    void queueCwMessage();
    void abortCwTransmission();
    void handleEntryEnter(int vfo, bool callsignField);
    void setRunMode(int mode);
    void setAutoCqEnabled(bool enabled);
    void stopAutoCq();
    void setCwWpm(int wpm);
    void updateCwStatus(const logger::CwKeyerStatus &status);
    void setSo2vEnabled(bool enabled);
    void setSo2rEnabled(bool enabled);
    void focusVfo(int vfo);
    void logQsoForVfo(int vfo);
    void updateDupeIndicatorForVfo(int vfo);
    void updateRadioSnapshotForRadio(int radio, const logger::RadioSnapshot &snapshot);

private:
    void loadBuiltInDefinitions();
    void loadOperator();
    void loadContests();
    void refreshDxClusterStatus();
    const Contest *activeContest() const;
    void applyActiveLogStation();
    void updateSentExchangeVisibility(int vfo);
    QStringList logNames(qint64 excludedId = 0) const;
    void saveSentExchangeValues(qint64 contestId,
                                const QMap<QString, QString> &values);
    void updateStationLabel();
    void rebuildQsoForm();
    void rebuildQsoForm(int vfo);
    void refreshRecentQsos();
    void exportCurrentLog(LogExportFormat format);
    void configureRadioContext(int radio);
    void autoConnectRadios();
    void updateSerialFields();
    ContestEngine::MultiplierResolver multiplierResolver() const;
    bool radioCatStale(int radio) const;
    void saveRadioConfig(int radio, const RadioConfig &config);
    RadioConfig radioConfigFromSettings(int radio = 0) const;
    void loadSupportData();
    void rebuildCheckPartialIndex();
    bool eventFilter(QObject *watched, QEvent *event) override;
    void createWindowMenu();
    void applyDefaultDockLayout();
    void restoreWindowLayout();
    void saveWindowLayout();
    QDockWidget *createPaneDock(const QString &title, const QString &objectName,
                                QWidget *content);
    int currentEntryContext() const;
    void setActiveEntryContext(int context);
    void refreshCheckWindows();
    void refreshScoreSummary();
    void refreshRate();
    void updateTopStatus();
    void updateEntryHeaders();
    void showOperatorMessage(const QString &message, bool alert = false);
    bool sendCwTemplate(QString message, int context, QString *problem,
                        QString callsignOverride = {});
    bool sendEsmSequence(const QString &sequence, int context,
                         QString callsignOverride = {});
    QString esmSequenceForContest(const QString &stage,
                                  int fallbackFunctionKey) const;
    bool sendFunctionKeyForContext(int index, int context);
    void sendFunctionKey(int index);
    void editFunctionKey(int index);
    void updateFunctionKeyButtons();
    void wipeEntry(int context);
    QString exchangeDisplay(const QMap<QString, QString> &fields,
                            const QString &fallback, bool sent) const;
    const ContestDefinition *activeDefinition() const;

    SqliteRepository &m_repository;
    RadioController m_radioControllers[2];
    QVector<DxClusterController *> m_dxClusterControllers;
    QVector<DxClusterConfig> m_dxClusterConfigs;
    QHash<DxClusterController *, DxClusterStatus> m_dxClusterStatuses;
    CwKeyerController m_cwKeyerController;
    // Global station settings; m_station is the effective station of the active log.
    Station m_defaultStation;
    QHash<QString, QString> m_builtInCabrilloContests;
    Station m_station;
    QVector<Contest> m_contests;
    QHash<qint64, ContestDefinition> m_definitions;
    QComboBox *m_contestSelector = nullptr;
    QCheckBox *m_so2vToggle = nullptr;
    QCheckBox *m_so2rToggle = nullptr;
    QComboBox *m_activeRadioSelector = nullptr;
    QComboBox *m_runModeSelector = nullptr;
    QLabel *m_stationLabel = nullptr;
    QLabel *m_activeContestLabel = nullptr;
    QLabel *m_radioLabel = nullptr;
    QLabel *m_dupeLabels[2]{};
    QLineEdit *m_callsignEdits[2]{};
    QLabel *m_callInfoLabels[2]{};
    QLabel *m_workedBandLabels[2]{};
    QLabel *m_entryHeaders[2]{};
    QComboBox *m_bandSelectors[2]{};
    QComboBox *m_modeSelectors[2]{};
    QHBoxLayout *m_exchangeLayouts[2]{};
    QMap<QString, QLineEdit *> m_exchangeEdits[2];
    QHBoxLayout *m_sentExchangeLayouts[2]{};
    QMap<QString, QLineEdit *> m_sentExchangeEdits[2];
    QFrame *m_entryGroups[2]{};
    QPushButton *m_focusButtons[2]{};
    int m_activeEntryContext = 0;
    QLabel *m_topStatus = nullptr;
    QLabel *m_messageLine = nullptr;
    QPushButton *m_functionKeyButtons[8]{};
    QTimer *m_clockTimer = nullptr;
    QTimer *m_messageTimer = nullptr;
    QTimer *m_autoCqTimer = nullptr;
    QAction *m_functionKeyActions[8]{};
    QAction *m_esmAction = nullptr;
    QAction *m_autoCqAction = nullptr;
    CwKeyerStatus m_cwStatus;
    bool m_autoCqEnabled = false;
    qint64 m_scoreTotal = 0;
    QTableWidget *m_recentQsos = nullptr;
    QVector<Qso> m_logQsos;
    QSet<QString> m_workedMultiplierKeys;
    ContestScore m_score;
    mutable QHash<QString, QMap<QString, QString>> m_ctyCache;
    int m_scorePoints = 0;
    int m_scoreMultipliers = 0;
    QTableWidget *m_bandmap = nullptr;
    QTableWidget *m_radioABandmap = nullptr;
    QTableWidget *m_clusterLog = nullptr;
    bool m_dxClusterConnected = false;
    QComboBox *m_bandmapBandFilter = nullptr;
    QComboBox *m_bandmapModeFilter = nullptr;
    QComboBox *m_bandmapContinentFilter = nullptr;
    QComboBox *m_clusterConnectionSelector = nullptr;
    QLineEdit *m_clusterCommandInput = nullptr;
    QCheckBox *m_hideWorkedSpots = nullptr;
    QLabel *m_clusterLabel = nullptr;
    QTimer *m_spotExpiryTimer = nullptr;
    QListWidget *m_checkPartial = nullptr;
    QTableWidget *m_checkCallGrid = nullptr;
    QLabel *m_checkCallTitle = nullptr;
    QLabel *m_rateLabel = nullptr;
    QTableWidget *m_summaryTable = nullptr;
    QLabel *m_finalScoreLabel = nullptr;
    QHash<qint64, Spot> m_spots;
    QHash<QString, CallHistoryEntry> m_callHistory;
    QStringList m_masterCallsigns;
    QStringList m_checkCallsigns;
    QVector<CtyPrefixEntry> m_ctyPrefixes;
    RadioSnapshot m_radioSnapshots[2];
    QDateTime m_radioSnapshotTimes[2];
    QString m_lastRadioModes[2];
    QTimer *m_catStaleTimer = nullptr;
    int m_cwConfiguredRadio = 0;
    QLineEdit *m_cwMessageEdit = nullptr;
    QComboBox *m_cwRadioSelector = nullptr;
    QSpinBox *m_cwWpmSpin = nullptr;
    QLabel *m_cwStatusLabel = nullptr;
    QDockWidget *m_bandmapDock = nullptr;
    QDockWidget *m_radioABandmapDock = nullptr;
    QDockWidget *m_cwDock = nullptr;
    QDockWidget *m_contestInfoDock = nullptr;
    QDockWidget *m_clusterDock = nullptr;
    QDockWidget *m_checkPartialDock = nullptr;
    QDockWidget *m_checkCallDock = nullptr;
    QDockWidget *m_rateDock = nullptr;
    QDockWidget *m_summaryDock = nullptr;
    QDockWidget *m_greylineDock = nullptr;
    QWidget *m_greylineWidget = nullptr;
};

} // namespace logger
