#include "ConsoleSupport.h"
#include "CaseListImport.h"
#include "DailyStyle.h"

QWidget* MainWindow::workersPage() {
    auto widget=page(QStringLiteral("工作机状态"),QStringLiteral("按物理服务器管理子 slot；下线后不再派发新任务，正在执行的 testcase 正常完成。"));
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
            {QStringLiteral("全部上线"),"resume"},{QStringLiteral("全部下线"),"pause"},{QStringLiteral("更新全部SVN"),"update"}}){
        auto button=new QPushButton(operation.first);button->setMinimumWidth(112);hostControlButtons_.append(button);toolbar->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,action=operation.second]{controlWorkers(true,machines_->currentText(),action);});
    }
    hostRecoveryButton_=new QPushButton(QStringLiteral("整机故障恢复…"));hostRecoveryButton_->setObjectName("hostRecoveryButton");
    hostRecoveryButton_->setMinimumWidth(140);toolbar->addSpacing(16);toolbar->addWidget(hostRecoveryButton_);
    connect(hostRecoveryButton_,&QPushButton::clicked,this,[this]{recoverOfflineWorkers(true,machines_->currentText());});
    toolbar->addStretch();rightBox->addLayout(toolbar);
    auto policyRow=new QHBoxLayout;policyRow->setSpacing(8);
    policyRow->addWidget(new QLabel(QStringLiteral("运行模式")));
    for(const auto& choice:QList<QPair<QString,QString>>{{QStringLiteral("Auto"),"auto"},{QStringLiteral("半开"),"half"},{QStringLiteral("全开"),"full"}}){
        auto button=new QPushButton(choice.first);button->setObjectName("hostPolicy_"+choice.second);
        hostPolicyButtons_.append(button);policyRow->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,mode=choice.second]{setHostPolicy(machines_->currentText(),{{"enabled",true},{"mode",mode.toStdString()},{"duration_minutes",240}});});
    }
    auto plan=new QPushButton(QStringLiteral("每周计划…"));plan->setObjectName("hostPolicyPlan");hostPolicyButtons_.append(plan);policyRow->addWidget(plan);
    connect(plan,&QPushButton::clicked,this,&MainWindow::editHostPolicy);policyRow->addStretch();rightBox->addLayout(policyRow);
    hostPolicySummary_=new QLabel;hostPolicySummary_->setObjectName("hostPolicySummary");hostPolicySummary_->setTextFormat(Qt::PlainText);hostPolicySummary_->setWordWrap(true);rightBox->addWidget(hostPolicySummary_);
    auto resources=new QFrame;resources->setObjectName("hostResources");auto resourceBox=new QVBoxLayout(resources);resourceBox->setContentsMargins(0,0,0,0);resourceBox->setSpacing(7);
    auto resourceHeading=new QHBoxLayout;auto resourceTitle=new QLabel(QStringLiteral("主机资源"));resourceTitle->setObjectName("sectionTitle");resourceHeading->addWidget(resourceTitle);
    resourceHeading->addStretch();hostResourceStatus_=new QLabel;hostResourceStatus_->setObjectName("hostResourceStatus");hostResourceStatus_->setTextFormat(Qt::PlainText);resourceHeading->addWidget(hostResourceStatus_);resourceBox->addLayout(resourceHeading);
    auto resourceCards=new QHBoxLayout;resourceCards->setSpacing(10);
    for(const auto& entry:QList<QPair<QString,QString>>{{"cpu",QStringLiteral("CPU 使用率")},{"memory",QStringLiteral("可用内存")},{"disk",QStringLiteral("工作盘可用空间")},{"concurrency",QStringLiteral("活动任务 / 目标并发")}}){
        auto card=new QFrame;card->setObjectName("metricCard");auto box=new QVBoxLayout(card);box->setContentsMargins(12,9,12,9);box->setSpacing(4);
        auto caption=new QLabel(entry.second);caption->setObjectName("muted");box->addWidget(caption);
        auto value=new QLabel(QStringLiteral("—"));value->setObjectName("resource_"+entry.first);value->setTextFormat(Qt::PlainText);value->setStyleSheet("font-size:19px;font-weight:600;");box->addWidget(value);hostResourceValues_[entry.first]=value;
        auto detail=new QLabel;detail->setObjectName("resourceDetail_"+entry.first);detail->setTextFormat(Qt::PlainText);detail->setWordWrap(true);detail->setStyleSheet("font-size:11px;color:#72829a;");box->addWidget(detail);hostResourceDetails_[entry.first]=detail;
        auto bar=new QProgressBar;bar->setObjectName("resourceBar_"+entry.first);bar->setRange(0,1000);bar->setTextVisible(false);bar->setFixedHeight(4);box->addWidget(bar);hostResourceBars_[entry.first]=bar;
        resourceCards->addWidget(card,1);
    }
    resourceBox->addLayout(resourceCards);hostResourceDecision_=new QLabel;hostResourceDecision_->setObjectName("hostResourceDecision");hostResourceDecision_->setTextFormat(Qt::PlainText);hostResourceDecision_->setWordWrap(true);resourceBox->addWidget(hostResourceDecision_);
    hostResourceExtra_=new QLabel;hostResourceExtra_->setObjectName("hostResourceExtra");hostResourceExtra_->setTextFormat(Qt::PlainText);hostResourceExtra_->setWordWrap(true);hostResourceExtra_->setStyleSheet("font-size:11px;color:#72829a;");resourceBox->addWidget(hostResourceExtra_);rightBox->addWidget(resources);
    slotsTable_=table({QStringLiteral("Slot / Worker ID"),QStringLiteral("工作状态"),QStringLiteral("连接 / 调度 / 维护"),QStringLiteral("当前 testcase"),QStringLiteral("操作")});
    slotsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);slotsTable_->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);slotsTable_->verticalHeader()->setDefaultSectionSize(76);rightBox->addWidget(slotsTable_,1);
    slotsTable_->setObjectName("workerSlots");slotsTable_->horizontalHeader()->setSectionResizeMode(4,QHeaderView::Fixed);slotsTable_->setColumnWidth(4,260);
    auto hint=new QLabel(QStringLiteral("工作状态 OFFLINE 表示该 slot 不参与调度；连接状态单独显示真实心跳。下线中的 testcase 正常完成后转为已下线。通信失联时，保留的任务只代表最后已知状态，不代表进程已经停止。"));
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
    auto titleItem=layout->takeAt(0);auto titleRow=new QHBoxLayout;titleRow->addWidget(titleItem->widget(),1);delete titleItem;
    auto plans=new QPushButton(QStringLiteral("定时计划 / 每日开关"));plans->setObjectName("automationButton");
    connect(plans,&QPushButton::clicked,this,[this]{openAutomation();});
    titleRow->addWidget(plans);titleRow->addWidget(add);layout->insertLayout(0,titleRow);
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
    taskSource_=new QComboBox;taskSource_->addItem(QStringLiteral("全部来源"),"");taskSource_->addItem(QStringLiteral("每日定时"),"scheduled");taskSource_->addItem(QStringLiteral("每周计划"),"plan");taskSource_->addItem(QStringLiteral("人工提交"),"manual");taskSource_->addItem(QStringLiteral("来源未分类"),"unknown");
    taskStatus_=new QComboBox;taskStatus_->setObjectName("taskStatus");
    for(const auto& choice:QList<QPair<QString,QString>>{{QStringLiteral("全部结果"),""},{"PASS","PASS"},{"FAIL","FAIL"},{"TIMEOUT","TIMEOUT"},{"CRASH","CRASH"},{"ERROR","ERROR"},{"UNKNOWN","UNKNOWN"},{QStringLiteral("执行中"),"RUNNING"},{QStringLiteral("排队中"),"QUEUED"},{"CANCELLED","CANCELLED"}})taskStatus_->addItem(choice.first,choice.second);
    filters->addWidget(taskSearch_,1);filters->addWidget(taskSource_);filters->addWidget(taskStatus_);content->addLayout(filters);
    tasksTree_=new QTreeWidget;tasksTree_->setObjectName("batchTable");
    tasksTree_->setHeaderLabels({QStringLiteral("批次 / 模板"),QStringLiteral("版本 / 来源"),QStringLiteral("状态"),QStringLiteral("结果分布"),QStringLiteral("运行 / 排队"),QStringLiteral("完成 / 通过率"),QStringLiteral("创建时间"),QStringLiteral("操作")});
    tasksTree_->setRootIsDecorated(false);tasksTree_->setItemsExpandable(false);tasksTree_->setIndentation(0);tasksTree_->setAlternatingRowColors(false);tasksTree_->setMouseTracking(true);
    tasksTree_->setSelectionMode(QAbstractItemView::SingleSelection);tasksTree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);tasksTree_->header()->setStretchLastSection(false);
    tasksTree_->header()->setSectionResizeMode(QHeaderView::Fixed);tasksTree_->header()->setSectionResizeMode(0,QHeaderView::Interactive);tasksTree_->setColumnWidth(0,220);
    const QList<int> widths{0,85,108,220,75,180,110,220};for(int i=1;i<widths.size();++i)tasksTree_->setColumnWidth(i,widths[i]);
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
    auto widget=page(QStringLiteral("每日回归对比"),QStringLiteral("按日期与阶段查找新回退，只纳入 Clock 每日定时批次"));
    widget->setObjectName("dailyRegression");widget->setStyleSheet(dailyStyle());
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());
    auto filters=new QGridLayout;filters->setHorizontalSpacing(14);filters->setVerticalSpacing(6);
    dateA_=new QDateEdit(QDate::currentDate().addDays(-1));dateB_=new QDateEdit(QDate::currentDate());
    dateA_->setObjectName("dailyBaseline");dateB_->setObjectName("dailyComparison");
    for(auto date:{dateA_,dateB_}){date->setCalendarPopup(true);date->setDisplayFormat("yyyy-MM-dd");date->setMinimumWidth(140);}
    regressionStage_=new QComboBox;regressionStage_->setObjectName("dailyStage");regressionStage_->addItem(QStringLiteral("全部阶段"),"");regressionStage_->setMinimumWidth(190);
    filters->addWidget(new QLabel(QStringLiteral("基准日期")),0,0);filters->addWidget(new QLabel(QStringLiteral("对比日期")),0,1);
    filters->addWidget(new QLabel(QStringLiteral("阶段 · 每日任务模板")),0,2);
    filters->addWidget(dateA_,1,0);filters->addWidget(dateB_,1,1);filters->addWidget(regressionStage_,1,2);
    filters->setColumnStretch(2,1);layout->addLayout(filters);
    auto searchRow=new QHBoxLayout;regressionSearch_=new QLineEdit;regressionSearch_->setObjectName("dailySearch");
    regressionSearch_->setMaxLength(512);regressionSearch_->setClearButtonEnabled(true);regressionSearch_->setPlaceholderText(QStringLiteral("搜索 testcase 名称或相对路径…"));
    searchRow->addWidget(regressionSearch_,1);regressionSync_=new QLabel(QStringLiteral("等待查询"));regressionSync_->setObjectName("dailySync");searchRow->addWidget(regressionSync_);layout->addLayout(searchRow);
    auto metrics=new QHBoxLayout;metrics->setSpacing(12);
    const QList<QStringList> categories{
        {"REGRESSION",QStringLiteral("新回退"),"PASS → FAIL","#b53344"},
        {"IMPROVEMENT",QStringLiteral("新通过"),"FAIL → PASS","#187b59"},
        {"STABLE_PASS",QStringLiteral("持续通过"),QStringLiteral("两天均为 PASS"),"#3169e9"},
        {"STABLE_FAIL",QStringLiteral("持续失败"),QStringLiteral("两天均为 FAIL"),"#94620d"}};
    for(const auto& item:categories){
        auto button=new QPushButton;button->setObjectName("dailyMetric");button->setProperty("change",item[0]);button->setCheckable(true);button->setMinimumWidth(135);
        auto box=new QVBoxLayout(button);box->setContentsMargins(16,12,16,12);box->setSpacing(4);
        auto title=new QLabel(QStringLiteral("● ")+item[1]);title->setStyleSheet("color:"+item[3]+";font-size:12px;");
        auto value=new QLabel(QStringLiteral("—"));value->setObjectName("dailyMetricValue");auto detail=new QLabel(item[2]);detail->setObjectName("muted");
        for(auto label:{title,value,detail}){label->setAttribute(Qt::WA_TransparentForMouseEvents);box->addWidget(label);}
        metrics->addWidget(button,1);regressionCards_[item[0]]=button;
        connect(button,&QPushButton::clicked,this,[this,key=item[0]]{selectDailyChange(key);});
    }layout->addLayout(metrics);
    auto panel=new QFrame;panel->setObjectName("dailyPanel");auto body=new QVBoxLayout(panel);body->setContentsMargins(16,16,16,12);body->setSpacing(13);
    regressionTitle_=new QLabel(QStringLiteral("每日回归对比"));regressionTitle_->setObjectName("dailyHeading");body->addWidget(regressionTitle_);
    regressionChange_=new QComboBox;regressionChange_->hide();
    auto pills=new QHBoxLayout;
    for(const auto& item:QList<QPair<QString,QString>>{
        {QStringLiteral("全部"),""},{QStringLiteral("新回退"),"REGRESSION"},{QStringLiteral("新通过"),"IMPROVEMENT"},
        {QStringLiteral("持续通过"),"STABLE_PASS"},{QStringLiteral("持续失败"),"STABLE_FAIL"},{QStringLiteral("未完成 / 其他"),"OTHER"}}){
        regressionChange_->addItem(item.first,item.second);auto button=new QPushButton(item.first);button->setCheckable(true);button->setProperty("change",item.second);
        button->setObjectName("dailyCategory");button->setChecked(item.second.isEmpty());regressionPills_[item.second]=button;pills->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,key=item.second]{selectDailyChange(key);});
    }pills->addStretch();body->addLayout(pills);
    regressionTable_=table({QStringLiteral("testcase"),QStringLiteral("基准结果"),QStringLiteral("对比结果"),QStringLiteral("变化"),QStringLiteral("日志对比")});
    regressionTable_->setObjectName("dailyResults");regressionTable_->setWordWrap(true);regressionTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    regressionTable_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    for(int column=1;column<5;++column){regressionTable_->horizontalHeader()->setSectionResizeMode(column,QHeaderView::Fixed);regressionTable_->setColumnWidth(column,column==4?110:120);}
    regressionTable_->verticalHeader()->setDefaultSectionSize(72);body->addWidget(regressionTable_,1);
    regressionSummary_=new QLabel(QStringLiteral("请选择日期和阶段，数据会自动查询。"));regressionSummary_->setObjectName("dailySummary");regressionSummary_->setWordWrap(true);body->addWidget(regressionSummary_);
    auto footer=new QHBoxLayout;auto plan=new QPushButton(QStringLiteral("每日计划 / 完整性"));plan->setObjectName("dailyPlan");
    auto first=new QPushButton(QStringLiteral("刷新 / 首页"));first->setObjectName("dailyRefresh");
    regressionNext_=new QPushButton(QStringLiteral("下一页"));regressionNext_->setObjectName("dailyNext");regressionNext_->setEnabled(false);
    auto automation=new QPushButton(QStringLiteral("每日回归开关 / 定时计划"));automation->setObjectName("dailyAutomationButton");
    connect(automation,&QPushButton::clicked,this,[this]{openAutomation();});
    footer->addWidget(plan);footer->addWidget(automation);footer->addStretch();footer->addWidget(first);footer->addWidget(regressionNext_);body->addLayout(footer);layout->addWidget(panel,1);
    regressionDebounce_.setSingleShot(true);regressionDebounce_.setInterval(280);
    connect(&regressionDebounce_,&QTimer::timeout,this,[this]{queryDaily();});
    connect(dateA_,&QDateEdit::dateChanged,this,[this]{scheduleDailyQuery();});
    connect(dateB_,&QDateEdit::dateChanged,this,[this]{scheduleDailyQuery();});
    connect(regressionStage_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{scheduleDailyQuery();});
    connect(regressionSearch_,&QLineEdit::textChanged,this,[this]{scheduleDailyQuery();});
    connect(first,&QPushButton::clicked,this,[this]{queryDaily();});
    connect(regressionNext_,&QPushButton::clicked,this,[this]{queryDaily(true);});
    connect(plan,&QPushButton::clicked,this,[this]{showDailyPlan();});
    return widget;
}
