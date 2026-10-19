#include "ConsoleSupport.h"
#include "CaseListImport.h"

QWidget* MainWindow::workersPage() {
    auto widget=page(QStringLiteral("工作机状态"),QStringLiteral("按物理服务器管理子 slot；停止接单不会中断运行中的 testcase。"));
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());machines_=new QComboBox(widget);machines_->hide();
    auto split=new QSplitter;auto left=new QWidget;auto leftBox=new QVBoxLayout(left);leftBox->setContentsMargins(4,8,4,8);
    auto heading=new QLabel(QStringLiteral("工作服务器"));heading->setObjectName("sectionTitle");leftBox->addWidget(heading);
    auto autoName=new QLabel(QStringLiteral("自动注册 · 按主机分组"));autoName->setObjectName("muted");leftBox->addWidget(autoName);
    hostsList_=new QListWidget;hostsList_->setObjectName("hostList");leftBox->addWidget(hostsList_,1);
    showOffline_=new QCheckBox(QStringLiteral("显示离线历史 slots"));showOffline_->setChecked(demo_);leftBox->addWidget(showOffline_);
    connect(showOffline_,&QCheckBox::toggled,this,[this]{showWorkers();});split->addWidget(GcrpUi::card(left));
    auto right=new QWidget;auto rightBox=new QVBoxLayout(right);rightBox->setContentsMargins(10,8,10,8);rightBox->setSpacing(14);
    hostTitle_=new QLabel;hostTitle_->setObjectName("sectionTitle");hostTitle_->setTextFormat(Qt::PlainText);rightBox->addWidget(hostTitle_);
    auto toolbar=new QHBoxLayout;
    for(const auto& operation:QList<QPair<QString,QString>>{
            {QStringLiteral("全部上线"),"resume"},{QStringLiteral("全部下线"),"pause"},{QStringLiteral("更新"),"update"}}){
        auto button=new QPushButton(operation.first);button->setMinimumWidth(112);hostControlButtons_.append(button);toolbar->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,action=operation.second]{controlWorkers(true,machines_->currentText(),action);});
    }toolbar->addStretch();rightBox->addLayout(toolbar);
    slotsTable_=table({QStringLiteral("Slot / Worker ID"),QStringLiteral("通信状态"),QStringLiteral("接单状态 / 维护"),QStringLiteral("当前执行"),QStringLiteral("操作")});
    slotsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);slotsTable_->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);slotsTable_->verticalHeader()->setDefaultSectionSize(76);rightBox->addWidget(slotsTable_,1);
    slotsTable_->setObjectName("workerSlots");slotsTable_->horizontalHeader()->setSectionResizeMode(4,QHeaderView::Fixed);slotsTable_->setColumnWidth(4,260);
    auto hint=new QLabel(QStringLiteral("操作语义　下线＝停止接新任务；运行中的 testcase 正常结束。SVN 更新等待执行和结果上报结束后进行。离线主机需先启动 Worker，不能在此远程开机。"));
    hint->setWordWrap(true);hint->setObjectName("notice");rightBox->addWidget(hint);
    workerSummary_=new QLabel;workerSummary_->setWordWrap(true);workerSummary_->setObjectName("muted");rightBox->addWidget(workerSummary_);
    split->addWidget(GcrpUi::card(right));split->setSizes({230,900});split->setStretchFactor(1,1);layout->addWidget(split,1);
    connect(hostsList_,&QListWidget::currentItemChanged,this,[this](QListWidgetItem* item,QListWidgetItem*){if(item)machines_->setCurrentText(item->data(Qt::UserRole).toString());});
    connect(machines_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{showWorkers();});
    connect(slotsTable_,&QTableWidget::cellDoubleClicked,this,[this](int row,int){if(auto item=slotsTable_->item(row,3)){bool ok=false;auto id=item->data(Qt::UserRole).toString().toULongLong(&ok);if(ok)openTask(id);}});
    return widget;
}

QWidget* MainWindow::tasksPage() {
    auto widget=page(QStringLiteral("任务管理"),QStringLiteral("以批次查看任务进度，完成率与通过率分别统计。"));
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());
    auto add=new QPushButton(QStringLiteral("＋ 添加批次"));add->setProperty("kind","primary");add->setMaximumWidth(150);
    auto titleItem=layout->takeAt(0);auto titleRow=new QHBoxLayout;titleRow->addWidget(titleItem->widget(),1);delete titleItem;titleRow->addWidget(add);layout->insertLayout(0,titleRow);
    connect(add,&QPushButton::clicked,this,[this]{const auto nav=findChildren<QPushButton*>("navigation");if(nav.size()==4)nav[2]->click();});
    auto metrics=new QHBoxLayout;metrics->setSpacing(16);
    for(const auto& caption:QStringList{QStringLiteral("批次"),QStringLiteral("运行中 testcase"),QStringLiteral("排队中"),QStringLiteral("异常 / 失败")}){
        auto card=new QFrame;card->setObjectName("metricCard");auto box=new QVBoxLayout(card);box->setContentsMargins(20,15,20,15);
        auto label=new QLabel(caption);label->setObjectName("muted");auto value=new QLabel("0");value->setObjectName("metricValue");box->addWidget(label);box->addWidget(value);taskMetrics_<<value;metrics->addWidget(card,1);
    }layout->addLayout(metrics);
    auto panel=new QFrame;panel->setObjectName("contentCard");auto content=new QVBoxLayout(panel);content->setContentsMargins(0,0,0,0);content->setSpacing(0);
    auto filters=new QHBoxLayout;filters->setContentsMargins(18,14,18,14);
    taskSearch_=new QLineEdit;taskSearch_->setObjectName("taskSearch");taskSearch_->setMinimumWidth(260);taskSearch_->setClearButtonEnabled(true);
    taskSearch_->setPlaceholderText(QStringLiteral("搜索批次、模板、版本或 testcase"));
    taskSource_=new QComboBox;taskSource_->addItem(QStringLiteral("全部来源"),"");taskSource_->addItem(QStringLiteral("每日定时"),"scheduled");taskSource_->addItem(QStringLiteral("人工提交"),"manual");taskSource_->addItem(QStringLiteral("来源未分类"),"unknown");
    taskStatus_=new QComboBox;taskStatus_->setObjectName("taskStatus");
    for(const auto& choice:QList<QPair<QString,QString>>{{QStringLiteral("全部结果"),""},{"PASS","PASS"},{"FAIL","FAIL"},{"TIMEOUT","TIMEOUT"},{"CRASH","CRASH"},{"ERROR","ERROR"},{"UNKNOWN","UNKNOWN"},{QStringLiteral("执行中"),"RUNNING"},{QStringLiteral("排队中"),"QUEUED"},{"CANCELLED","CANCELLED"}})taskStatus_->addItem(choice.first,choice.second);
    filters->addWidget(taskSearch_,1);filters->addWidget(taskSource_);filters->addWidget(taskStatus_);content->addLayout(filters);
    tasksTree_=new QTreeWidget;tasksTree_->setObjectName("batchTable");
    tasksTree_->setHeaderLabels({QStringLiteral("批次 / 模板"),QStringLiteral("版本 / 来源"),QStringLiteral("状态"),QStringLiteral("结果分布"),QStringLiteral("运行 / 排队"),QStringLiteral("完成 / 通过率"),QStringLiteral("创建时间"),QStringLiteral("操作")});
    tasksTree_->setRootIsDecorated(false);tasksTree_->setItemsExpandable(false);tasksTree_->setIndentation(0);tasksTree_->setAlternatingRowColors(false);tasksTree_->setMouseTracking(true);
    tasksTree_->setSelectionMode(QAbstractItemView::SingleSelection);tasksTree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);tasksTree_->header()->setStretchLastSection(false);
    tasksTree_->header()->setSectionResizeMode(QHeaderView::Fixed);tasksTree_->header()->setSectionResizeMode(0,QHeaderView::Interactive);tasksTree_->setColumnWidth(0,220);
    const QList<int> widths{0,85,108,205,75,145,110,220};for(int i=1;i<widths.size();++i)tasksTree_->setColumnWidth(i,widths[i]);
    tasksTree_->header()->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter);tasksTree_->setUniformRowHeights(false);content->addWidget(tasksTree_,1);
    auto footer=new QHBoxLayout;footer->setContentsMargins(18,12,18,12);taskSummary_=new QLabel;taskSummary_->setWordWrap(true);footer->addWidget(taskSummary_,1);
    taskPrev_=new QPushButton(QStringLiteral("上一页"));taskNext_=new QPushButton(QStringLiteral("下一页"));footer->addWidget(taskPrev_);footer->addWidget(taskNext_);content->addLayout(footer);layout->addWidget(panel,1);
    auto filter=[this]{taskPage_=0;filterTasks();};
    connect(taskSearch_,&QLineEdit::textChanged,this,filter);connect(taskSource_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,filter);connect(taskStatus_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,filter);
    connect(taskPrev_,&QPushButton::clicked,this,[this]{--taskPage_;filterTasks();});connect(taskNext_,&QPushButton::clicked,this,[this]{++taskPage_;filterTasks();});
    connect(tasksTree_,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem* item,int){openBatch(item->data(0,Qt::UserRole+1).toString());});
    return widget;
}

QWidget* MainWindow::addPage() {
    auto widget=page(QStringLiteral("添加任务"),QStringLiteral("勾选 testcase 目录，按批次入队；人工提交不计入每日回归。"));
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());auto split=new QSplitter;
    auto treePanel=new QWidget;auto treeLayout=new QVBoxLayout(treePanel);treeLayout->setContentsMargins(10,10,10,10);treeLayout->setSpacing(14);
    auto heading=new QLabel(QStringLiteral("选择 testcase"));heading->setObjectName("sectionTitle");treeLayout->addWidget(heading);
    auto source=new QLabel(QStringLiteral("Server 管理的 test2 目录 · 自动递归查找 run.tcl"));source->setObjectName("muted");treeLayout->addWidget(source);
    auto searchRow=new QHBoxLayout;caseSearch_=new QLineEdit;caseSearch_->setClearButtonEnabled(true);caseSearch_->setPlaceholderText(QStringLiteral("搜索例子名称 / 目录"));
    auto selectVisible=new QPushButton(QStringLiteral("全选筛选结果"));searchRow->addWidget(caseSearch_,1);searchRow->addWidget(selectVisible);treeLayout->addLayout(searchRow);
    casesTree_=new QTreeWidget;casesTree_->setObjectName("testcaseSelection");casesTree_->setHeaderHidden(true);casesTree_->setMouseTracking(true);casesTree_->setIndentation(22);treeLayout->addWidget(casesTree_,1);
    auto strip=new QHBoxLayout;selection_=new QLabel(QStringLiteral("已选择 0 个 testcase"));strip->addWidget(selection_,1);auto clear=new QPushButton(QStringLiteral("清空"));strip->addWidget(clear);treeLayout->addLayout(strip);
    connect(caseSearch_,&QLineEdit::textChanged,this,[this]{filterCases();});
    connect(clear,&QPushButton::clicked,this,[this]{for(QTreeWidgetItemIterator it(casesTree_);*it;++it)if(!(*it)->data(0,Qt::UserRole).toString().isEmpty())(*it)->setCheckState(0,Qt::Unchecked);});
    connect(selectVisible,&QPushButton::clicked,this,[this]{for(QTreeWidgetItemIterator it(casesTree_);*it;++it)if(!(*it)->isHidden()&&!(*it)->data(0,Qt::UserRole).toString().isEmpty())(*it)->setCheckState(0,Qt::Checked);});
    split->addWidget(GcrpUi::card(treePanel));
    auto formContent=new QWidget;formContent->setObjectName("formContent");auto formBox=new QVBoxLayout(formContent);formBox->setContentsMargins(18,14,18,14);formBox->setSpacing(12);
    auto title=new QLabel(QStringLiteral("批次设置"));title->setObjectName("sectionTitle");formBox->addWidget(title);
    auto intro=new QLabel(QStringLiteral("配置提交时生成快照，不影响其他人的批次。"));intro->setObjectName("muted");intro->setWordWrap(true);formBox->addWidget(intro);
    auto field=[formBox](const QString& label,QWidget* input){auto caption=new QLabel(label);caption->setObjectName("fieldLabel");formBox->addWidget(caption);formBox->addWidget(input);};
    target_=new QLineEdit(settings_.target);target_->setObjectName("businessTarget");target_->setPlaceholderText(QStringLiteral("填写本次验证的业务目标"));
    field(QStringLiteral("业务目标（必填）"),target_);
    batchName_=new QLineEdit;batchName_->setObjectName("manualBatchName");batchName_->setReadOnly(true);batchName_->setPlaceholderText(QStringLiteral("选择执行模板后自动生成"));
    batchName_->setToolTip(QStringLiteral("名称自动使用“人工验证 + 模板名”；同名批次仍有独立的唯一标识，不会合并。"));
    revision_=new QComboBox;revision_->setObjectName("artifactRevision");flow_=new QComboBox;flow_->setObjectName("executionTemplate");
    field(QStringLiteral("批次名称（自动生成）"),batchName_);field(QStringLiteral("GalaxCore ZIP 版本"),revision_);
    auto versionsHint=new QLabel(QStringLiteral("最近 50 个成功版本 + 每日回归受保护版本，以 Server 清单为准"));versionsHint->setWordWrap(true);versionsHint->setObjectName("muted");formBox->addWidget(versionsHint);
    field(QStringLiteral("执行模板"),flow_);
    auto flowRow=new QHBoxLayout;flowSummary_=new QLabel;flowSummary_->setWordWrap(true);auto configureFlow=new QPushButton(QStringLiteral("配置阶段"));flowRow->addWidget(flowSummary_,1);flowRow->addWidget(configureFlow);formBox->addLayout(flowRow);
    flowOptions_=new QTreeWidget(widget);flowOptions_->setHeaderLabels({"flow_config","value"});flowOptions_->hide();
    connect(flowOptions_,&QTreeWidget::itemChanged,this,[this]{const auto config=selectedFlowConfig();int count=0;for(auto it=config.begin();it!=config.end();++it)if(it.value().is_number_integer()&&it.value().get<int>()!=0)++count;flowSummary_->setText(QStringLiteral("flow_config\n%1 项开启 · %2 项可配置").arg(count).arg(config.size()));});
    connect(configureFlow,&QPushButton::clicked,this,[this]{
        auto dialog=new QDialog(this);dialog->setObjectName("flowDialog");dialog->setWindowTitle(QStringLiteral("配置 flow_config"));dialog->resize(760,610);
        auto box=new QVBoxLayout(dialog);box->setContentsMargins(24,22,24,20);box->setSpacing(16);
        auto title=new QLabel(QStringLiteral("配置 flow_config"));title->setObjectName("dialogTitle");box->addWidget(title);
        auto hint=new QLabel(QStringLiteral("勾选为 1，未勾选为 0。仅应用于当前待提交批次，不修改 Server 模板。"));hint->setWordWrap(true);box->addWidget(hint);
        auto toolbar=new QHBoxLayout;auto all=new QPushButton(QStringLiteral("全选"));auto none=new QPushButton(QStringLiteral("全部关闭"));auto reset=new QPushButton(QStringLiteral("恢复模板默认"));toolbar->addWidget(all);toolbar->addWidget(none);toolbar->addWidget(reset);toolbar->addStretch();box->addLayout(toolbar);
        auto scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);auto contents=new QWidget;contents->setObjectName("flowContents");auto grid=new QGridLayout(contents);grid->setSpacing(8);
        QMap<QString,QCheckBox*> checks;QMap<QString,QSpinBox*> numbers;
        for(int i=0;i<flowOptions_->topLevelItemCount();++i){auto item=flowOptions_->topLevelItem(i);auto name=item->text(0);
            if(auto number=qobject_cast<QSpinBox*>(flowOptions_->itemWidget(item,1))){auto row=new QWidget;auto h=new QHBoxLayout(row);h->addWidget(new QLabel(name));auto n=new QSpinBox;n->setRange(0,1000000);n->setValue(number->value());h->addWidget(n);numbers[name]=n;grid->addWidget(row,i/2,i%2);}
            else{auto check=new QCheckBox(name);check->setChecked(item->checkState(0)==Qt::Checked);check->setObjectName("flowOption");checks[name]=check;grid->addWidget(check,i/2,i%2);}
        }scroll->setWidget(contents);box->addWidget(scroll,1);
        connect(all,&QPushButton::clicked,dialog,[checks]{for(auto check:checks)check->setChecked(true);});connect(none,&QPushButton::clicked,dialog,[checks]{for(auto check:checks)check->setChecked(false);});
        connect(reset,&QPushButton::clicked,dialog,[this,checks,numbers]{const auto key=flow_->currentText().toStdString();const auto values=catalog_.is_object()&&catalog_.contains("templates")&&catalog_["templates"].contains(key)?catalog_["templates"][key]["flow_config"]:Json::object();for(auto it=checks.begin();it!=checks.end();++it)it.value()->setChecked(values.value(it.key().toStdString(),0)!=0);for(auto it=numbers.begin();it!=numbers.end();++it)it.value()->setValue(values.value(it.key().toStdString(),0));});
        auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用配置"));buttons->button(QDialogButtonBox::Ok)->setProperty("kind","primary");buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));box->addWidget(buttons);
        connect(buttons,&QDialogButtonBox::accepted,dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
        if(dialog->exec()==QDialog::Accepted)for(int i=0;i<flowOptions_->topLevelItemCount();++i){auto item=flowOptions_->topLevelItem(i);if(checks.contains(item->text(0)))item->setCheckState(0,checks[item->text(0)]->isChecked()?Qt::Checked:Qt::Unchecked);else if(numbers.contains(item->text(0)))qobject_cast<QSpinBox*>(flowOptions_->itemWidget(item,1))->setValue(numbers[item->text(0)]->value());}
        delete dialog;
    });
    connect(flow_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{resetFlowOptions();});
    auto advancedToggle=new QPushButton(QStringLiteral("▸ 高级设置"));advancedToggle->setCheckable(true);advancedToggle->setProperty("kind","quiet");formBox->addWidget(advancedToggle);
    auto advanced=new QWidget;advanced->setObjectName("advancedSettings");auto advancedBox=new QFormLayout(advanced);advancedBox->setContentsMargins(0,0,0,0);
    priority_=new QSpinBox;priority_->setRange(-100,100);timeout_=new QSpinBox;timeout_->setRange(1,86400);timeout_->setValue(5400);
    advancedBox->addRow(QStringLiteral("优先级"),priority_);advancedBox->addRow(QStringLiteral("单例超时 / 秒"),timeout_);formBox->addWidget(advanced);
    advanced->hide();
    connect(advancedToggle,&QPushButton::toggled,advanced,[advanced,advancedToggle](bool visible){advanced->setVisible(visible);advancedToggle->setText(visible?QStringLiteral("▾ 高级设置"):QStringLiteral("▸ 高级设置"));});
    auto hint=new QLabel(QStringLiteral("每次提交生成独立批次并加入队列。同优先级按服务端接收顺序调度，不锁住其他用户的界面。"));hint->setWordWrap(true);hint->setObjectName("notice");formBox->addWidget(hint);
    auto reload=new QPushButton(QStringLiteral("从 Server 刷新目录 / ZIP / 模板"));formBox->addWidget(reload);
    connect(reload,&QPushButton::clicked,this,[this]{if(demo_)loadCatalog();else if(operations_.contains("query_catalog"))loadServerCatalog(0,0,nullptr,{},true);else note(QStringLiteral("Server 不支持 query_catalog"),true);});
    auto importButton=new QPushButton(QStringLiteral("导入测试案例列表…"));importButton->setProperty("kind","quiet");formBox->addWidget(importButton);
    importButton->setToolTip(QStringLiteral("每行一个相对于 test2 的 run.tcl 路径；全部匹配后替换当前勾选，不自动提交。支持任意文件扩展名。"));
    connect(importButton,&QPushButton::clicked,this,[this]{
        if(catalogLoading_||submitting_){note(QStringLiteral("目录加载或任务提交中，请稍后导入。"),true);return;}
        auto path=QFileDialog::getOpenFileName(this,QStringLiteral("导入测试案例列表"),{},QStringLiteral("所有文件 (*)"));if(path.isEmpty())return;
        try{QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("无法打开测试案例列表文件");
            auto bytes=file.read(8*1024*1024+1);if(file.error()!=QFileDevice::NoError)throw std::runtime_error("读取测试案例列表失败");
            const int count=importCaseList(casesTree_,bytes);caseSearch_->clear();filterCases();
            note(QStringLiteral("已导入并勾选 %1 个测试案例（替换原勾选，未提交任务）。").arg(count));
        }catch(const std::exception& error){QMessageBox::warning(this,QStringLiteral("测试案例列表导入失败"),QString::fromUtf8(error.what())+QStringLiteral("\n原有勾选保持不变。"));}
    });
    catalogStatus_=new QLabel(QStringLiteral("等待 Server 目录，无需在 GUI 主机部署 test2 或 ZIP。"));catalogStatus_->setWordWrap(true);catalogStatus_->setTextFormat(Qt::PlainText);catalogStatus_->setObjectName("muted");formBox->addWidget(catalogStatus_);
    formBox->addStretch();submit_=new QPushButton(QStringLiteral("预览并提交"));submit_->setObjectName("submitManualBatch");submit_->setProperty("kind","primary");submit_->setEnabled(demo_);
    connect(submit_,&QPushButton::clicked,this,[this]{submitBatch();});connect(casesTree_,&QTreeWidget::itemChanged,this,[this]{selection_->setText(QStringLiteral("已选择 %1 个 testcase").arg(selectedCases(casesTree_).size()));});
    auto formScroll=new QScrollArea;formScroll->setFrameShape(QFrame::NoFrame);formScroll->setWidgetResizable(true);formScroll->setWidget(formContent);formScroll->setMinimumWidth(340);
    auto right=new QWidget;auto rightBox=new QVBoxLayout(right);rightBox->setContentsMargins(0,0,0,0);rightBox->addWidget(formScroll,1);auto footer=new QHBoxLayout;footer->setContentsMargins(16,10,16,10);footer->addWidget(submit_);rightBox->addLayout(footer);
    split->addWidget(GcrpUi::card(right));split->setStretchFactor(0,3);split->setStretchFactor(1,2);split->setSizes({650,430});layout->addWidget(split,1);
    resetFlowOptions();return widget;
}

QWidget* MainWindow::regressionPage() {
    auto widget=page(QStringLiteral("每日回归"),QStringLiteral("按日期比较每日定时批次；人工批次独立，缺少结果单独列出。"));
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());auto bar=new QHBoxLayout;
    dateA_=new QDateEdit(demo_?QDate(2026,10,11):QDate::currentDate().addDays(-1));dateB_=new QDateEdit(demo_?QDate(2026,10,12):QDate::currentDate());
    dateA_->setCalendarPopup(true);dateB_->setCalendarPopup(true);dateA_->setDisplayFormat("yyyy-MM-dd");dateB_->setDisplayFormat("yyyy-MM-dd");dateA_->setMinimumWidth(140);dateB_->setMinimumWidth(140);
    bar->addWidget(new QLabel(QStringLiteral("基准日期")));bar->addWidget(dateA_);bar->addWidget(new QLabel(QStringLiteral("比较日期")));bar->addWidget(dateB_);
    auto compare=new QPushButton(QStringLiteral("比较 / 首页"));bar->addWidget(compare);
    auto plan=new QPushButton(QStringLiteral("比较日 · 回归完整性"));plan->setObjectName("dailyPlan");
    connect(plan,&QPushButton::clicked,this,[this]{showDailyPlan();});bar->addWidget(plan);bar->addStretch();layout->addLayout(bar);
    auto filters=new QHBoxLayout;regressionStage_=new QComboBox;regressionStage_->addItem(QStringLiteral("全部阶段"),"");
    regressionChange_=new QComboBox;
    for(const auto& item:QList<QPair<QString,QString>>{{QStringLiteral("全部变化"),""},{QStringLiteral("新回退"),"REGRESSION"},
        {QStringLiteral("新通过"),"IMPROVEMENT"},{QStringLiteral("缺少结果"),"MISSING"},
        {QStringLiteral("异常 / 未完成"),"EXCEPTION"},{QStringLiteral("无确认回退"),"UNCHANGED"}})regressionChange_->addItem(item.first,item.second);
    regressionSearch_=new QLineEdit;regressionSearch_->setPlaceholderText(QStringLiteral("testcase 路径搜索"));
    filters->addWidget(regressionStage_);filters->addWidget(regressionChange_);filters->addWidget(regressionSearch_,1);layout->addLayout(filters);
    regressionSummary_=new QLabel;regressionSummary_->setWordWrap(true);layout->addWidget(regressionSummary_);
    regressionTable_=table(demo_?QStringList{"模板阶段","testcase","基准结果","比较结果","变化","证据"}:
        QStringList{"模板阶段","testcase","基准结果","比较结果","变化","证据"});layout->addWidget(GcrpUi::card(regressionTable_),1);
    auto actions=new QHBoxLayout;auto baseline=new QPushButton(QStringLiteral("基准任务 / 日志"));
    auto current=new QPushButton(QStringLiteral("比较任务 / 日志"));regressionNext_=new QPushButton(QStringLiteral("下一页"));regressionNext_->setEnabled(false);
    actions->addWidget(baseline);actions->addWidget(current);actions->addStretch();actions->addWidget(regressionNext_);layout->addLayout(actions);
    auto open=[this](const char* side){const auto row=regressionTable_->currentRow();
        if(demo_||row<0||row>=int(regressionRows_.size())||regressionRows_[row][side].is_null())return;
        openTask(regressionRows_[row][side]["task_id"].get<uint64_t>());};
    connect(baseline,&QPushButton::clicked,this,[open]{open("baseline");});connect(current,&QPushButton::clicked,this,[open]{open("comparison");});
    connect(regressionNext_,&QPushButton::clicked,this,[this]{queryDaily(true);});
    auto hint=new QLabel(demo_?QStringLiteral("演示数据：PASS→FAIL 为新回退；TIMEOUT 和缺失结果另外展示。"):
        QStringLiteral("只统计带 scheduled 来源和日期的定时任务；人工 / 未分类历史记录排除。重复记录按同日同阶段同 case 的最新 task_id 展示，每页 50 条。"));
    hint->setWordWrap(true);layout->addWidget(hint);connect(compare,&QPushButton::clicked,this,[this]{queryDaily();});return widget;
}
