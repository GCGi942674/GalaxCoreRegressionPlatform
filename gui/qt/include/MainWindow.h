#pragma once
#include "RpcClient.h"
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QTreeWidget>
#include <QTableWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QDateEdit>
#include <QTimer>
#include <QPushButton>
#include <QElapsedTimer>
#include <QListWidget>
#include <QCheckBox>
#include <memory>

struct GuiSettings {
    QString host, catalogFile;
    quint16 port{0};
    int timeout{5000}, refreshSeconds{5}, logPageBytes{8192};
    QString target;
};
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(GuiSettings settings);
private:
    GuiSettings settings_;
    const bool demo_{false};
    bool loading_{false}, submitting_{false};
    RpcClient rpc_;
    Json workers_ = Json::array(), tasks_ = Json::array(), catalog_;
    QSet<QString> operations_;
    QStackedWidget* pages_{};
    QLabel* connection_{};
    QLabel *breadcrumb_{}, *navCount_{}, *hostTitle_{}, *flowSummary_{};
    QList<QLabel*> taskMetrics_;
    QComboBox* taskSource_{};
    int taskPage_{0};
    QPushButton *taskPrev_{}, *taskNext_{};
    QLabel* freshness_{};
    bool workersStale_{true}, tasksStale_{true};
    QElapsedTimer workerUpdated_, taskUpdated_;
    QTimer freshnessTimer_;
    QLabel* selection_{};
    QComboBox* machines_{};
    QListWidget* hostsList_{};
    QList<QPushButton*> hostControlButtons_;
    bool managementPending_{false};
    QCheckBox* showOffline_{};
    QLabel *workerSummary_{}, *catalogStatus_{};
    bool catalogLoading_{false};
    QLineEdit* caseSearch_{};
    QTreeWidget* tasksTree_{};
    QLineEdit* taskSearch_{};
    QComboBox* taskStatus_{};
    QLabel* taskSummary_{};
    QTreeWidget* casesTree_{};
    QTreeWidget* flowOptions_{};
    QTableWidget* slotsTable_{};
    QTableWidget* regressionTable_{};
    QLineEdit *batchName_{}, *target_{};
    QComboBox *revision_{}, *flow_{};
    QSpinBox *priority_{}, *timeout_{};
    QPushButton* submit_{};
    QDateEdit *dateA_{}, *dateB_{};
    QComboBox *regressionStage_{}, *regressionChange_{};
    QLineEdit* regressionSearch_{};
    QLabel* regressionSummary_{};
    QPushButton* regressionNext_{};
    Json regressionRows_=Json::array();
    QString regressionCursor_;
    bool regressionLoading_{false};
    uint64_t regressionGeneration_{0};
    std::string regressionInstance_;
    std::string regressionFilter_;
    QTimer refreshTimer_;
    QString lastInstance_;
    QString manualBatch_;
    Json pendingSubmissions_;
    int submitIndex_{0};
    QWidget* workersPage();
    QWidget* tasksPage();
    QWidget* addPage();
    QWidget* regressionPage();
    void refresh();
    void fetchList(const std::string& type, const Json& filter, const Json& cursor,
                   std::shared_ptr<Json> rows, std::string instance,
                   std::function<void(bool,Json,QString)> done, int page = 0, uint64_t generation = 0);
    void showWorkers();
    void controlWorkers(bool machine, const QString& target, const QString& action);
    void showTasks();
    void filterTasks();
    void loadCatalog();
    void applyCatalog();
    void loadServerCatalog(uint64_t offset=0, uint64_t generation=0,
                           std::shared_ptr<Json> result=nullptr, std::string instance={}, bool force=false);
    void resetFlowOptions();
    void filterCases();
    Json selectedFlowConfig() const;
    void submitBatch();
    void submitNext();
    void cancelSelection();
    void openTask(uint64_t id);
    void openBatch(const QString& batch);
    void openBatchRows(const QString& batch,const Json& records);
    void rerunFailures(const Json& rows);
    void loadRerun(std::shared_ptr<Json> ids,size_t index);
    void showDailyPlan();
    void openLogs(const Json& task);
    void queryDaily(bool next=false);
    void note(const QString& text, bool error = false);
    void updateFreshness();
    void invalidateSnapshots(bool workers, bool tasks);
};
