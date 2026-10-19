#include "ConsoleSupport.h"

MainWindow::MainWindow(GuiSettings settings)
    : settings_(std::move(settings)), rpc_(this) {
    setWindowTitle(QStringLiteral("GalaxCore Regression Platform 调度控制台 · UI v3")+
        (demo_?QStringLiteral("【离线演示 · 非真实工作机】"):QStringLiteral(" · ")+settings_.host+":"+QString::number(settings_.port))); resize(1280,820);
    setMinimumSize(1100,720);
    auto central = new QWidget; central->setObjectName("surface"); setCentralWidget(central);
    auto shell = new QHBoxLayout(central); shell->setContentsMargins(0,0,0,0); shell->setSpacing(0);
    auto side = new QWidget; side->setObjectName("sidebar"); side->setFixedWidth(196);
    auto sideLayout = new QVBoxLayout(side); sideLayout->setContentsMargins(16,28,16,22);
    auto brand = new QLabel(QStringLiteral("GCRP")); brand->setObjectName("brand");
    auto brandRow = new QHBoxLayout; auto badge = new QLabel("G");
    badge->setObjectName("brandBadge"); badge->setFixedSize(40,42); badge->setAlignment(Qt::AlignCenter);
    brandRow->addWidget(badge); brandRow->addWidget(brand,1); sideLayout->addLayout(brandRow);
    auto caption = new QLabel(QStringLiteral("Regression Platform")); caption->setWordWrap(true);caption->setObjectName("muted"); sideLayout->addWidget(caption);
    auto section=new QLabel(QStringLiteral("控制台"));section->setObjectName("navCaption");sideLayout->addSpacing(20);sideLayout->addWidget(section); pages_ = new QStackedWidget;
    auto transition = GcrpUi::pageTransition(pages_,this);
    const QStringList labels{QStringLiteral("工作机状态"),QStringLiteral("任务管理"),
                             QStringLiteral("添加任务"),QStringLiteral("每日回归")};
    for (int i=0;i<labels.size();++i) {
        auto button = new QPushButton(labels[i]); button->setCheckable(true);
        button->setObjectName("navigation"); button->setIcon(GcrpUi::navigationIcon(i));
        button->setIconSize(QSize(18,18)); sideLayout->addWidget(button);
        if(i==1){navCount_=new QLabel(button);navCount_->setObjectName("navCount");navCount_->setAlignment(Qt::AlignCenter);navCount_->setGeometry(130,12,22,20);}
        connect(button,&QPushButton::clicked,this,[this,side,i,transition]{
            if(pages_->currentIndex()!=i){transition->capture();pages_->setCurrentIndex(i);transition->start();}
            for (auto b:side->findChildren<QPushButton*>()) b->setChecked(false);
            side->findChildren<QPushButton*>()[i]->setChecked(true);
            const QStringList titles{QStringLiteral("工作机状态"),QStringLiteral("任务管理"),QStringLiteral("添加任务"),QStringLiteral("每日回归")};
            breadcrumb_->setText(QStringLiteral("工作空间　 /　 ")+titles[i]);
        });
        if (i==0) button->setChecked(true);
    }
    sideLayout->addStretch();
    auto footer = new QLabel(demo_ ? QStringLiteral("演示模式\n示例数据 · 不发送任务") :
        settings_.host+":"+QString::number(settings_.port)+QStringLiteral("\n日志保留 30 天"));
    footer->setObjectName("sidebarFooter"); footer->setWordWrap(true); sideLayout->addWidget(footer); shell->addWidget(side);
    auto body = new QWidget; body->setObjectName("workspace"); auto bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0,0,0,0);
    auto top=new QWidget;top->setObjectName("topbar");top->setFixedHeight(74);
    auto bar=new QHBoxLayout(top);bar->setContentsMargins(28,12,28,12);
    breadcrumb_=new QLabel(QStringLiteral("工作空间　 /　 任务管理"));breadcrumb_->setObjectName("breadcrumb");bar->addWidget(breadcrumb_);
    bar->addStretch();connection_=new QLabel;connection_->setObjectName("connection");connection_->setTextFormat(Qt::PlainText);
    connection_->setMaximumWidth(360);connection_->setWordWrap(true);bar->addWidget(connection_);
    auto refresh=new QPushButton(QStringLiteral("刷新快照"));bar->addWidget(refresh);
    auto role=new QLabel(QStringLiteral("受信任内网 · 未启用认证"));role->setToolTip(QStringLiteral("可连接 Server 的用户均可管理工作机和删除已结束的任务记录。"));bar->addWidget(role);
    connect(refresh,&QPushButton::clicked,this,[this]{this->refresh();});bodyLayout->addWidget(top);
    if(demo_){auto demoBanner=new QLabel(QStringLiteral("交互评审模式　 ·　 示例数据与真实 Server 完全隔离，不发送任何任务"));demoBanner->setObjectName("demoBanner");bodyLayout->addWidget(demoBanner);}
    freshness_=new QLabel;freshness_->setWordWrap(true);
    freshness_->setStyleSheet("color:#ad3442;padding:0px 22px 6px;");
    bodyLayout->addWidget(freshness_);
    pages_->addWidget(workersPage()); pages_->addWidget(tasksPage());
    pages_->addWidget(addPage()); pages_->addWidget(regressionPage());
    connect(pages_,&QStackedWidget::currentChanged,this,[this](int index){if(index==3)scheduleDailyQuery();});
    bodyLayout->addWidget(pages_,1); shell->addWidget(body,1);
    setStyleSheet(GcrpUi::styleSheet());
    new GcrpUi::InteractionFeedback(this);
    pages_->setCurrentIndex(1);
    const auto navigation=side->findChildren<QPushButton*>();for(int i=0;i<navigation.size();++i)navigation[i]->setChecked(i==1);
    for(auto button:findChildren<QPushButton*>())button->setCursor(button->isEnabled()?Qt::PointingHandCursor:Qt::ArrowCursor);
    {
        rpc_.configure(settings_.host,settings_.port,settings_.timeout);
        rpc_.request("query_capabilities",Json::object(),[this](RpcReply r){
            if (!r.ok) { note(r.error,true); return; }
            for (const auto& op:r.result.value("operations",Json::array()))
                if (op.is_string()) operations_.insert(QString::fromStdString(op.get<std::string>()));
            if(operations_.contains("query_catalog"))loadServerCatalog();
            else {
                loadCatalog();
                if(catalog_.is_null())catalogStatus_->setText(QStringLiteral("Server 版本不支持自动目录查询，请更新 Server；旧版可手动导入目录清单。"));
            }
            this->refresh();
            if(pages_->currentIndex()==3)scheduleDailyQuery();
        });
        connect(&freshnessTimer_,&QTimer::timeout,this,[this]{
            const qint64 maxAge=qMax(settings_.refreshSeconds*3000,settings_.timeout*2);
            if(workerUpdated_.isValid()&&workerUpdated_.elapsed()>maxAge)workersStale_=true;
            if(taskUpdated_.isValid()&&taskUpdated_.elapsed()>maxAge)tasksStale_=true;
            updateFreshness();showWorkers();
        });freshnessTimer_.start(1000);updateFreshness();
        refreshTimer_.setInterval(settings_.refreshSeconds*1000);
        connect(&refreshTimer_,&QTimer::timeout,this,[this]{this->refresh();}); refreshTimer_.start();
        note(QStringLiteral("正在连接 Server…"));
    }
}

void MainWindow::note(const QString& text, bool error) {
    connection_->setText(text);connection_->setToolTip(text);connection_->setStyleSheet(error?"color:#c14450":"color:#6d7d93");
    if(error||text.startsWith(QStringLiteral("接单策略"))||text.startsWith(QStringLiteral("更新请求")))GcrpUi::toast(this,text,error);
}

void MainWindow::fetchList(const std::string& type,const Json& filter,const Json& cursor,
    std::shared_ptr<Json> rows,std::string instance,std::function<void(bool,Json,QString)> done,int page,uint64_t generation) {
    Json data=filter;data["limit"]=50;if(!cursor.is_null())data[type=="query_workers"?"after":"before_task_id"]=cursor;
    if(type=="query_tasks"&&generation)data["snapshot_generation"]=generation;
    rpc_.request(type,data,[this,type,filter,rows,instance,done,page,generation](RpcReply r){
        if(!r.ok){done(false,{},r.error);return;}
        if(!instance.empty()&&instance!=r.instance){done(false,{},QStringLiteral("Server 已重启，请重新刷新；保留原数据"));return;}
        try {
            const auto current=r.result.at("snapshot_generation").get<uint64_t>();
            if(page&&generation!=current){done(false,{},QStringLiteral("分页期间数据已变化，保留旧快照并等待下一次刷新，不混合统计。"));return;}
            const auto& items=r.result.at("items");if(!items.is_array())throw std::runtime_error("items must be array");
            for(const auto& row:items)rows->push_back(row);
            if(r.result.value("has_more",false)){
                if(page>=9999){done(false,{},QStringLiteral("查询超过 500000 条，请缩小范围；未导出不完整结果"));return;}
                auto next=r.result.at(type=="query_workers"?"next_after":"next_before_task_id");
                if(next.is_null()||items.empty())throw std::runtime_error("invalid cursor");
                fetchList(type,filter,next,rows,r.instance,done,page+1,current);
            }else done(true,*rows,{});
        }catch(const std::exception& e){done(false,{},QString::fromUtf8(e.what()));}
    });
}

void MainWindow::invalidateSnapshots(bool workers,bool tasks) {
    if(workers)workersStale_=true;if(tasks)tasksStale_=true;
    updateFreshness();showWorkers();
}

void MainWindow::updateFreshness() {
    if(demo_){freshness_->hide();return;}
    QStringList missing;
    if(workersStale_)missing<<QStringLiteral("工作机状态");
    if(tasksStale_)missing<<QStringLiteral("任务状态");
    freshness_->setVisible(!missing.isEmpty());
    freshness_->setText(QStringLiteral("数据未确认 / 已过期：")+missing.join(QStringLiteral("、"))+
        QStringLiteral("。保留上次快照；不代表当前在线状态。请检查连接或刷新。"));
    if(workerUpdated_.isValid())freshness_->setText(freshness_->text()+
        QStringLiteral(" 工作机快照距今 %1 秒。").arg(workerUpdated_.elapsed()/1000));
}


void MainWindow::refresh() {
    if(demo_){showWorkers();showTasks();return;}if(loading_||submitting_||regressionLoading_)return;
    loading_=true;
    fetchList("query_workers",Json::object(),nullptr,std::make_shared<Json>(Json::array()),{},
      [this](bool ok,Json rows,QString warning){
        if(!ok){loading_=false;invalidateSnapshots(true,true);note(QStringLiteral("连接中断或查询失败：")+warning,true);return;}
        workers_=std::move(rows);workersStale_=false;workerUpdated_.restart();updateFreshness();showWorkers();
        fetchList("query_tasks",{{"view","groups"}},nullptr,std::make_shared<Json>(Json::array()),{},
          [this,warning](bool success,Json rows,QString taskWarning){loading_=false;if(!success){invalidateSnapshots(false,true);note(QStringLiteral("任务查询失败：")+taskWarning,true);return;}tasks_=std::move(rows);tasksStale_=false;taskUpdated_.restart();updateFreshness();showTasks();note(QStringLiteral("已连接 · 更新于 ")+QDateTime::currentDateTime().toString("HH:mm:ss")+" "+warning+taskWarning);});
    });
}

void MainWindow::filterCases() {
    const auto query=caseSearch_->text().trimmed();
    std::function<bool(QTreeWidgetItem*)> matches=[&](QTreeWidgetItem* item){
        bool visible=false;
        if(item->childCount()==0)visible=item->data(0,Qt::UserRole).toString().contains(query,Qt::CaseInsensitive);
        else for(int i=0;i<item->childCount();++i)visible=matches(item->child(i))||visible;
        item->setHidden(!visible);if(!query.isEmpty()&&visible)item->setExpanded(true);return visible;
    };
    for(int i=0;i<casesTree_->topLevelItemCount();++i)matches(casesTree_->topLevelItem(i));
}

void MainWindow::loadCatalog() {
    if(catalogLoading_||submitting_)return;
    try {
        if(!demo_){if(settings_.catalogFile.isEmpty()){submit_->setEnabled(false);return;}QFile file(settings_.catalogFile);if(!file.open(QIODevice::ReadOnly)||file.size()>16*1024*1024)throw std::runtime_error("cannot read catalog (maximum 16 MiB)");auto bytes=file.readAll();catalog_=Json::parse(bytes.constData(),bytes.constData()+bytes.size());}
        applyCatalog();
        catalogStatus_->setText(demo_?QStringLiteral("离线演示清单，不代表 Server 实际目录。"):
            QStringLiteral("已导入本地清单：")+settings_.catalogFile+QStringLiteral("；内容可能过期，优先使用 Server 刷新。"));
    }catch(const std::exception& e){catalog_=nullptr;submit_->setEnabled(false);catalogStatus_->setText(QStringLiteral("目录清单错误：")+QString::fromUtf8(e.what()));}
}

void MainWindow::loadServerCatalog(uint64_t offset,uint64_t generation,std::shared_ptr<Json> result,std::string instance,bool force) {
    if(!result){
        if(catalogLoading_||submitting_)return;
        catalogLoading_=true;submit_->setEnabled(false);
        result=std::make_shared<Json>(Json{{"cases",Json::array()}});
        catalogStatus_->setText(QStringLiteral("正在从 Server 读取 testcase、ZIP 和 flow 模板…"));
    }
    Json query=Json::object();
    if(generation){query["offset"]=offset;query["snapshot_generation"]=generation;}
    else if(force)query["refresh"]=true;
    rpc_.request("query_catalog",query,[this,result,instance,offset,generation](RpcReply r){
        auto failed=[this](const QString& message){catalogLoading_=false;submit_->setEnabled(false);
            catalogStatus_->setText(QStringLiteral("Server 目录加载失败，禁止提交；请检查配置后刷新：")+message);};
        if(!r.ok){failed(r.error);return;}
        try {
            if(!instance.empty()&&r.instance!=instance)throw std::runtime_error("Server restarted during catalog query; refresh");
            const auto& rows=r.result.at("cases");
            if(!rows.is_array()||rows.size()>200)throw std::runtime_error("invalid catalog page");
            const auto current=r.result.at("snapshot_generation").get<uint64_t>();
            if(!current||(generation&&current!=generation))throw std::runtime_error("catalog changed; refresh");
            if(!generation)for(const char* key:{"templates","revisions","protected_revisions","sources","warnings"})(*result)[key]=r.result.at(key);
            for(const auto& path:rows)(*result)["cases"].push_back(path);
            if((*result)["cases"].size()>50000)throw std::runtime_error("catalog exceeds 50000 cases");
            const auto next=r.result.at("next_offset");
            if(!next.is_null()){
                auto n=next.get<uint64_t>();if(n<=offset||n!=(*result)["cases"].size()||rows.empty())throw std::runtime_error("invalid catalog cursor");
                loadServerCatalog(n,current,result,r.instance);return;
            }
            if(r.result.at("total_cases")!=(*result)["cases"].size())throw std::runtime_error("incomplete catalog");
            catalog_=std::move(*result);catalogLoading_=false;applyCatalog();
            QStringList warnings;
            for(const auto& warning:catalog_.value("warnings",Json::array()))warnings<<QString::fromStdString(warning.get<std::string>());
            catalogStatus_->setText(QStringLiteral("来源：Server %1:%2\n%3 个 testcase · %4 个 ZIP 版本 · %5 个模板\n")
                .arg(settings_.host).arg(settings_.port).arg(qulonglong(catalog_["cases"].size())).arg(qulonglong(catalog_["revisions"].size())).arg(qulonglong(catalog_["templates"].size()))+warnings.join("\n"));
        }catch(const std::exception& e){failed(QString::fromUtf8(e.what()));}
    });
}

void MainWindow::applyCatalog() {
        if(!catalog_.at("cases").is_array()||catalog_["cases"].size()>50000||!catalog_.at("templates").is_object()||!catalog_.at("revisions").is_array())throw std::runtime_error("invalid catalog");
        for(const auto& version:catalog_["revisions"]){if(!version.is_string())throw std::runtime_error("revision must be text");const auto v=version.get<std::string>();if(v.empty()||v.size()>32||v.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("revision must be 1-32 decimal digits");}
        for(auto it=catalog_["templates"].begin();it!=catalog_["templates"].end();++it){if(!it.value().is_object()||!it.value().contains("flow_config")||!it.value()["flow_config"].is_object()||it.value()["flow_config"].empty())throw std::runtime_error("template requires nonempty flow_config");}
        for(const auto& value:catalog_["cases"]){if(!value.is_string())throw std::runtime_error("invalid case path");auto path=QString::fromStdString(value.get<std::string>());auto parts=path.split('/');if(path.startsWith('/')||path.contains('\\')||path.contains(':')||parts.contains("..")||parts.contains(".")||parts.contains("")||!path.endsWith("/run.tcl"))throw std::runtime_error("case must be a safe relative run.tcl path");}
        const auto selected=selectedCases(casesTree_);const auto previousRevision=revision_->currentText(),previousFlow=flow_->currentText();
        QSignalBlocker flowSignals(flow_);
        casesTree_->clear();revision_->clear();flow_->clear();auto root=new QTreeWidgetItem(casesTree_,{QStringLiteral("test2")});root->setFlags(root->flags()|Qt::ItemIsUserCheckable|Qt::ItemIsAutoTristate);root->setCheckState(0,Qt::Unchecked);
        QMap<QString,QTreeWidgetItem*> directories;directories[""]=root;
        for(const auto& value:catalog_["cases"]){auto path=QString::fromStdString(value.get<std::string>());auto parts=path.split('/');QString key;auto parent=root;
            for(int i=0;i<parts.size()-1;++i){key+=(key.isEmpty()?"":"/")+parts[i];auto item=directories.value(key,nullptr);if(!item){item=new QTreeWidgetItem(parent,{parts[i]});item->setFlags(item->flags()|Qt::ItemIsUserCheckable|Qt::ItemIsAutoTristate);item->setCheckState(0,Qt::Unchecked);directories[key]=item;}parent=item;}
            auto leaf=new QTreeWidgetItem(parent,{"run.tcl"});leaf->setFlags(leaf->flags()|Qt::ItemIsUserCheckable);leaf->setCheckState(0,selected.contains(path)?Qt::Checked:Qt::Unchecked);leaf->setData(0,Qt::UserRole,path);leaf->setToolTip(0,path);
        }casesTree_->expandToDepth(2);filterCases();
        for(const auto& r:catalog_["revisions"]){revision_->addItem(QString::fromStdString(r.get<std::string>()));
            if(catalog_.contains("protected_revisions"))for(const auto& protectedVersion:catalog_["protected_revisions"])
                if(r==protectedVersion)revision_->setItemData(revision_->count()-1,QStringLiteral("每日回归受保护版本"),Qt::ToolTipRole);
        }
        for(auto it=catalog_["templates"].begin();it!=catalog_["templates"].end();++it)flow_->addItem(QString::fromStdString(it.key()));
        if(revision_->findText(previousRevision)>=0)revision_->setCurrentText(previousRevision);
        if(flow_->findText(previousFlow)>=0)flow_->setCurrentText(previousFlow);
        resetFlowOptions();
        submit_->setEnabled((demo_||operations_.contains("submit_task"))&&!catalog_["cases"].empty()&&!catalog_["templates"].empty()&&!catalog_["revisions"].empty());
}

void MainWindow::resetFlowOptions() {
    flowOptions_->clear();
    const auto key=flow_->currentText().toStdString();
    batchName_->setText(key.empty()?QString{}:QStringLiteral("人工验证 + ")+QString::fromStdString(key));
    const auto defaults=catalog_.is_object()&&catalog_.contains("templates")&&catalog_["templates"].contains(key)?
        catalog_["templates"][key].at("flow_config"):Json::object();
    const QStringList standard{"read_edif","read_xdc","report_timing_summary","report_utilization",
        "opt_design","place_design","place_design_from_syn","phys_opt_design","route_design",
        "route_design_from_place","write_checkpoint","write_bitstream","bit_cmp","msk_cmp",
        "bgn_cmp","dcp_cmp","rpx_cmp","shape_cmp","checksum_cmp"};
    auto keys=standard;
    for(auto it=defaults.begin();it!=defaults.end();++it){auto name=QString::fromStdString(it.key());if(!keys.contains(name))keys<<name;}
    for(const auto& name:keys){const auto value=defaults.value(name.toStdString(),0);
        auto item=new QTreeWidgetItem(flowOptions_,{name});
        if(value==0||value==1){item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(0,value?Qt::Checked:Qt::Unchecked);}
        else {auto number=new QSpinBox;number->setRange(0,1000000);number->setValue(value);flowOptions_->setItemWidget(item,1,number);}
    }
    flowOptions_->resizeColumnToContents(0);
}

Json MainWindow::selectedFlowConfig() const {
    Json snapshot=Json::object();
    for(int i=0;i<flowOptions_->topLevelItemCount();++i){auto item=flowOptions_->topLevelItem(i);
        auto number=qobject_cast<QSpinBox*>(flowOptions_->itemWidget(item,1));
        snapshot[item->text(0).toStdString()]=number?number->value():(item->checkState(0)==Qt::Checked?1:0);
    }return snapshot;
}

void MainWindow::submitBatch() {
    if(submitting_)return;auto paths=selectedCases(casesTree_);
    if(paths.isEmpty()||batchName_->text().trimmed().isEmpty()||(!demo_&&target_->text().trimmed().isEmpty())||revision_->currentText().isEmpty()||flow_->currentText().isEmpty()){
        QMessageBox::warning(this,QStringLiteral("参数不完整"),QStringLiteral("请填写业务目标，并选择 testcase、ZIP 版本和执行模板。批次名称由模板自动生成。"));return;}
    if(QMessageBox::question(this,QStringLiteral("确认人工批次"),QStringLiteral("%1\n%2 个 testcase · r%3 · %4\n取消只处理未开始的 testcase。\n逐个提交，失败会保留已接收的任务，不自动重试。").arg(batchName_->text()).arg(paths.size()).arg(revision_->currentText()).arg(flow_->currentText()))!=QMessageBox::Yes)return;
    // The default UUID representation is {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}.
    // Remove its braces explicitly for compatibility with older Qt5 headers.
    manualBatch_="gui-"+QUuid::createUuid().toString().mid(1,36);pendingSubmissions_=Json::array();submitIndex_=0;
    const auto flowSnapshot=selectedFlowConfig();
    for(const auto& path:paths){pendingSubmissions_.push_back({{"task_type","REGRESSION"},{"target",target_->text().trimmed().toStdString()},{"revision",revision_->currentText().toStdString()},{"priority",priority_->value()},
        {"submission_key",(manualBatch_+"-"+QString::number(pendingSubmissions_.size())).toStdString()},
        {"payload",{{"spec_version",2},{"case",path.toStdString()},{"flow",flow_->currentText().toStdString()},{"flow_config",flowSnapshot},{"timeout_seconds",timeout_->value()},
        {"context",{{"batch_id",manualBatch_.toStdString()},{"name",batchName_->text().toStdString()},{"suite","manual"},
            {"source","manual"},{"template",flow_->currentText().toStdString()}}}}}});}
    if(demo_){note(QStringLiteral("演示：批次预览通过；未发送任何网络请求"));return;}
    submitting_=true;submit_->setEnabled(false);submitNext();
}

void MainWindow::submitNext() {
    if(submitIndex_==int(pendingSubmissions_.size())){submitting_=false;submit_->setEnabled(true);QMessageBox::information(this,QStringLiteral("提交完成"),QStringLiteral("已接收 %1 个任务\n批次 %2").arg(submitIndex_).arg(manualBatch_));refresh();return;}
    note(QStringLiteral("提交批次 %1：%2 / %3 已接收").arg(manualBatch_).arg(submitIndex_).arg(pendingSubmissions_.size()));
    rpc_.request("submit_task",pendingSubmissions_[submitIndex_],[this](RpcReply r){
        if(!r.ok){
            const auto retry=QMessageBox::question(this,QStringLiteral("部分提交 / 结果可能不确定"),
                QStringLiteral("已确认接收 %1 个案例。\n批次：%2\n%3\n是否使用原幂等键重试当前及后续案例？不会重复创建已接收的案例。\n选“否”将保留已接收部分，可在任务管理中查询。").arg(submitIndex_).arg(manualBatch_).arg(r.error));
            if(retry==QMessageBox::Yes){submitNext();return;}
            submitting_=false;submit_->setEnabled(true);refresh();return;
        }++submitIndex_;submitNext();
    });
}

void MainWindow::cancelSelection() {
    if(!demo_&&tasksStale_){note(QStringLiteral("任务快照已过期，请刷新确认后再取消。"),true);return;}
    auto item=tasksTree_->currentItem();if(!item)return;
    const bool one=item->data(0,Qt::UserRole).isValid();auto id=item->data(0,Qt::UserRole).toString();auto batch=item->data(0,Qt::UserRole+1).toString();
    if(!one&&batch==QStringLiteral("无批次 / 单任务"))return;
    if(QMessageBox::question(this,QStringLiteral("确认取消"),QStringLiteral("仅取消未开始的 testcase；ASSIGNED/RUNNING 不受影响。取消记录保留。"))!=QMessageBox::Yes)return;
    if(demo_){for(auto& t:tasks_)if(text(t,"status")=="QUEUED"&&(one?text(t,"task_id")==id:text(t,"batch_id")==batch))t["status"]="CANCELLED";showTasks();return;}
    auto op=one?"cancel_task":"cancel_batch";
    if(!operations_.contains(op)){note(QStringLiteral("Server 未声明此操作"),true);return;}
    rpc_.request(op,one?Json{{"task_id",id.toULongLong()}}:Json{{"batch_id",batch.toStdString()}},[this](RpcReply r){if(!r.ok)QMessageBox::warning(this,QStringLiteral("取消未完成"),r.error);refresh();});
}
