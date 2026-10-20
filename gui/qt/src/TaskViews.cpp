#include "ConsoleSupport.h"
#include "RerunTask.h"
#include "RerunSelectionDialog.h"
#include "ArchiveDownload.h"

void MainWindow::showTasks() {
    const auto current=tasksTree_->currentItem();const auto selected=current?current->data(0,Qt::UserRole+1).toString():QString{};
    tasksTree_->clear();QMap<QString,QTreeWidgetItem*> batches;QMap<QString,Json> metadata;
    int runningAll=0,queuedAll=0,failAll=0;
    for(const auto& t:tasks_){
        auto batch=text(t,"batch_id");if(batch.isEmpty()||batch=="—")batch=QStringLiteral("无批次 / 单任务");
        auto parent=batches.value(batch,nullptr);
        if(!parent){parent=new QTreeWidgetItem(tasksTree_,{batch});parent->setData(0,Qt::UserRole+1,batch);parent->setExpanded(false);batches[batch]=parent;metadata[batch]=t;}
        const auto payload=t.value("payload",Json::object());auto path=text(payload,"case");
        auto item=new QTreeWidgetItem(parent,{path,text(t,"status"),text(t,"target"),text(t,"revision"),text(t,"worker_id")});
        item->setData(0,Qt::UserRole,text(t,"task_id"));item->setData(0,Qt::UserRole+2,outcome(t));
        const auto status=text(t,"status");if(status=="RUNNING"||status=="ASSIGNED")++runningAll;else if(status=="QUEUED"||status=="PENDING")++queuedAll;else if(status=="FAILED"||status=="TIMEOUT")++failAll;
    }
    for(auto it=batches.begin();it!=batches.end();++it){auto parent=it.value();const auto batch=it.key();const auto& t=metadata[batch];
        const auto payload=t.value("payload",Json::object()),context=payload.value("context",Json::object());
        QMap<QString,int> totals,business;QStringList revisions;
        for(int i=0;i<parent->childCount();++i){auto item=parent->child(i);totals[item->text(1)]++;business[item->data(0,Qt::UserRole+2).toString()]++;if(!revisions.contains(item->text(3)))revisions<<item->text(3);}
        int total=parent->childCount();
        if(t.contains("group_summary")){totals.clear();business.clear();const auto& summary=t["group_summary"];for(auto it=summary["counts"].begin();it!=summary["counts"].end();++it)totals[QString::fromStdString(it.key())]=it.value().get<int>();total=summary["total"].get<int>();
            for(auto it=summary["outcomes"].begin();it!=summary["outcomes"].end();++it)business[QString::fromStdString(it.key())]=it.value().get<int>();
            parent->setData(0,Qt::UserRole+6,QString::fromStdString(summary.dump()));
        }
        const int pass=business["PASS"],fail=business["FAIL"],timeout=totals["TIMEOUT"],running=totals["RUNNING"]+totals["ASSIGNED"],queued=totals["QUEUED"]+totals["PENDING"],cancelled=totals["CANCELLED"],done=total-running-queued;
        if(t.contains("group_summary")){const auto old=text(t,"status");runningAll+=running-((old=="RUNNING"||old=="ASSIGNED")?1:0);queuedAll+=queued-((old=="QUEUED"||old=="PENDING")?1:0);failAll+=totals["FAILED"]+timeout-((old=="FAILED"||old=="TIMEOUT")?1:0);}
        const auto status=running?"RUNNING":queued?"QUEUED":fail||timeout||business["CRASH"]||business["ERROR"]?"FAIL":cancelled?"CANCELLED":business["UNKNOWN"]?"UNKNOWN":"PASS";
        const auto source=QString::fromStdString(context.value("source",std::string("unknown")));parent->setData(0,Qt::UserRole+2,source);
        const auto name=QString::fromStdString(context.value("name",batch.toStdString())),flow=QString::fromStdString(context.value("template",payload.value("flow",std::string())));
        auto title=new QLabel(name.toHtmlEscaped()+"<br><span style='color:#8493a8;font-size:10px'>"+batch.toHtmlEscaped()+"<br>"+flow.toHtmlEscaped()+"</span>");title->setContentsMargins(12,10,8,10);title->setTextFormat(Qt::RichText);title->setWordWrap(true);tasksTree_->setItemWidget(parent,0,title);
        parent->setText(0,QString{});parent->setData(0,Qt::UserRole+4,name+" "+batch+" "+flow);
        parent->setText(1,revisions.join(", ")+"\n"+(source=="scheduled"?QStringLiteral("每日定时"):source=="manual"?QStringLiteral("人工提交"):QStringLiteral("来源未分类")));
        auto badge=GcrpUi::badge(status);tasksTree_->setItemWidget(parent,2,badge);
        auto counts=new QLabel(QStringLiteral("<span style='color:#188365'>%1 PASS</span>　<span style='color:#c14450'>%2 FAIL</span>　<span style='color:#a36c12'>%3 TIMEOUT</span><br><span style='color:#8493a8;font-size:10px'>CRASH %4 · ERROR %5 · 取消 %6</span>").arg(pass).arg(fail).arg(timeout).arg(business["CRASH"]).arg(business["ERROR"]).arg(cancelled));counts->setTextFormat(Qt::RichText);counts->setContentsMargins(8,8,8,8);tasksTree_->setItemWidget(parent,3,counts);
        parent->setText(4,QStringLiteral("%1 / %2").arg(running).arg(queued));
        auto progress=new QWidget;auto progressBox=new QVBoxLayout(progress);progressBox->setContentsMargins(8,10,8,10);progressBox->setSpacing(5);
        auto finished=new QLabel(QStringLiteral("%1/%2 · 完成 %3%").arg(done).arg(total).arg(total?100*done/total:0));auto bar=new QProgressBar;bar->setRange(0,qMax(1,total));bar->setValue(done);bar->setTextVisible(false);bar->setFixedHeight(5);
        auto rate=new QLabel(done>cancelled?QStringLiteral("通过率 %1%").arg(100.0*pass/(done-cancelled),0,'f',1):QStringLiteral("通过率 —"));rate->setObjectName("muted");progressBox->addWidget(finished);progressBox->addWidget(bar);progressBox->addWidget(rate);tasksTree_->setItemWidget(parent,5,progress);
        const auto created=t.value("created_at_ms",int64_t(0));parent->setText(6,created?QDateTime::fromMSecsSinceEpoch(created).toString("yyyy-MM-dd\nHH:mm:ss"):QStringLiteral("—"));
        auto controls=new QWidget;auto actions=new QHBoxLayout(controls);actions->setContentsMargins(4,4,4,4);actions->setSpacing(5);
        auto details=new QPushButton(QStringLiteral("详情"));auto cancel=new QPushButton(QStringLiteral("停止后续"));auto deletion=new QPushButton(QStringLiteral("删除"));
        deletion->setEnabled(!demo_&&!tasksStale_&&operations_.contains("delete_batch")&&running==0&&queued==0&&batch!=QStringLiteral("无批次 / 单任务"));
        deletion->setProperty("kind","danger");deletion->setToolTip(QStringLiteral("仅删除已结束批次的记录；运行或排队中的批次不可删除。日志和防重复提交信息保留。单任务请在详情中删除。"));
        cancel->setEnabled(queued>0&&(demo_||(!tasksStale_&&operations_.contains("cancel_batch")))&&batch!=QStringLiteral("无批次 / 单任务"));
        actions->addWidget(details);actions->addWidget(cancel);actions->addWidget(deletion);
        connect(details,&QPushButton::clicked,this,[this,batch]{openBatch(batch);});
        connect(cancel,&QPushButton::clicked,this,[this,parent]{tasksTree_->setCurrentItem(parent);cancelSelection();});
        connect(deletion,&QPushButton::clicked,this,[this,batch]{
            if(tasksStale_||!operations_.contains("delete_batch"))return;
            if(QMessageBox::warning(this,QStringLiteral("确认删除任务记录"),QStringLiteral("删除批次 %1 的全部已结束记录？\n记录将从查询页面移除，不能从 GUI 撤销。原始日志、防重复提交信息和每日计划保留。\n服务端会再次检查整个批次，含活动任务则整批拒绝。").arg(batch),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
            rpc_.request("delete_batch",{{"batch_id",batch.toStdString()},{"confirm",true}},[this](RpcReply r){
                if(!r.ok){note(QStringLiteral("删除未确认，请刷新核对：")+r.error,true);return;}
                note(QStringLiteral("已删除 %1 条任务记录，日志保留。").arg(r.result.value("deleted",uint64_t(0))));refresh();});
        });
        tasksTree_->setItemWidget(parent,7,controls);parent->setSizeHint(0,QSize(180,92));
        if(batch==selected)tasksTree_->setCurrentItem(parent);
    }
    const QList<int> values{batches.size(),runningAll,queuedAll,failAll};for(int i=0;i<taskMetrics_.size();++i)taskMetrics_[i]->setText(QString::number(values[i]));
    if(navCount_)navCount_->setText(QString::number(batches.size()));
    filterTasks();showWorkers();
}

void MainWindow::filterTasks() {
    const auto query=taskSearch_->text().trimmed(),status=taskStatus_->currentData().toString(),source=taskSource_->currentData().toString();
    QList<QTreeWidgetItem*> matched;int matchedCases=0;
    for(int i=0;i<tasksTree_->topLevelItemCount();++i){auto batch=tasksTree_->topLevelItem(i);int count=0;
        const auto group=batch->data(0,Qt::UserRole+6).toString();
        if(!group.isEmpty()){
            const auto summary=Json::parse(group.toStdString());auto n=[&](const char* key){return summary["outcomes"].value(key,0);};
            const bool state=status.isEmpty()||(status=="RUNNING"?(n("RUNNING")+n("ASSIGNED")>0):status=="QUEUED"?(n("QUEUED")+n("PENDING")>0):summary["outcomes"].value(status.toStdString(),0)>0);
            const bool match=state&&(source.isEmpty()||batch->data(0,Qt::UserRole+2).toString()==source)&&(batch->data(0,Qt::UserRole+4).toString()+" "+batch->text(1)).contains(query,Qt::CaseInsensitive);
            batch->setHidden(true);if(match){matched<<batch;matchedCases+=summary["total"].get<int>();}continue;
        }
        for(int j=0;j<batch->childCount();++j){auto item=batch->child(j);QStringList fields{batch->text(0)};
            fields<<batch->data(0,Qt::UserRole+4).toString();for(int c=0;c<5;++c)fields<<item->text(c);
            const auto value=item->data(0,Qt::UserRole+2).toString();
            const bool match=(source.isEmpty()||batch->data(0,Qt::UserRole+2).toString()==source)&&(status.isEmpty()||value==status||(status=="RUNNING"&&value=="ASSIGNED")||(status=="QUEUED"&&value=="PENDING"))&&fields.join(" ").contains(query,Qt::CaseInsensitive);
            item->setHidden(!match);if(match)++count;
        }batch->setHidden(true);if(count){matched<<batch;matchedCases+=count;}
    }
    const int pages=qMax(1,(matched.size()+19)/20);taskPage_=qBound(0,taskPage_,pages-1);
    for(int i=taskPage_*20;i<qMin(matched.size(),(taskPage_+1)*20);++i)matched[i]->setHidden(false);
    taskPrev_->setEnabled(taskPage_>0);taskNext_->setEnabled(taskPage_+1<pages);
    taskSummary_->setText(QStringLiteral("%1 个任务 · %2 个 testcase · 第 %3/%4 页\n每日任务对应 Tasks.yaml 条目；打开详情查看完整案例列表").arg(matched.size()).arg(matchedCases).arg(taskPage_+1).arg(pages));
    bool omitted=false;for(const auto& task:tasks_)omitted=omitted||task.value("summary_truncated",false);
    if(omitted)taskSummary_->setText(taskSummary_->text()+QStringLiteral("\n部分摘要超限已省略；请打开任务详情。缺少路径的记录不会导出到路径清单。"));
}


void MainWindow::openBatch(const QString& batch) {
    if(demo_){openBatchRows(batch,tasks_);return;}
    note(QStringLiteral("正在加载该任务的完整测试案例…"));
    fetchList("query_tasks",{{"batch_id",batch.toStdString()}},nullptr,std::make_shared<Json>(Json::array()),{},
        [this,batch](bool ok,Json rows,QString error){if(!ok){note(QStringLiteral("任务详情加载失败：")+error,true);return;}openBatchRows(batch,rows);});
}

void MainWindow::openBatchRows(const QString& batch,const Json& records) {
    auto dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("batchDialog");dialog->resize(1120,740);
    dialog->setWindowTitle(QStringLiteral("批次详情 · ")+batch);auto layout=new QVBoxLayout(dialog);layout->setContentsMargins(24,22,24,20);layout->setSpacing(16);
    auto top=new QHBoxLayout;auto heading=new QLabel(batch);heading->setObjectName("dialogTitle");heading->setTextFormat(Qt::PlainText);heading->setWordWrap(true);top->addWidget(heading,1);
    auto close=new QPushButton(QStringLiteral("关闭"));top->addWidget(close);connect(close,&QPushButton::clicked,dialog,&QDialog::close);layout->addLayout(top);
    auto snapshot=std::make_shared<Json>(Json::array());QMap<QString,int> totals;
    for(const auto& task:records){auto id=text(task,"batch_id");if(id.isEmpty()||id=="—")id=QStringLiteral("无批次 / 单任务");if(id==batch){snapshot->push_back(task);totals[text(task,"status")]++;}}
    auto stats=new QLabel(QStringLiteral("总例数 %1　 ·　 PASS %2　 FAIL %3　 TIMEOUT %4　 ·　 运行 %5 / 排队 %6").arg(snapshot->size()).arg(totals["SUCCEEDED"]).arg(totals["FAILED"]).arg(totals["TIMEOUT"]).arg(totals["RUNNING"]+totals["ASSIGNED"]).arg(totals["QUEUED"]+totals["PENDING"]));stats->setWordWrap(true);stats->setObjectName("notice");layout->addWidget(stats);
    auto hint=new QLabel(QStringLiteral("已加载该任务全部案例的查询快照；筛选 / 导出覆盖此任务，不限于当前页。缺少日志不代表 testcase 失败。"));hint->setObjectName("muted");hint->setWordWrap(true);layout->addWidget(hint);
    auto toolbar=new QHBoxLayout;auto status=new QComboBox;for(int i=0;i<taskStatus_->count();++i)status->addItem(taskStatus_->itemText(i),taskStatus_->itemData(i));
    auto search=new QLineEdit;search->setPlaceholderText(QStringLiteral("搜索 testcase 路径 / Worker"));search->setClearButtonEnabled(true);
    auto exportButton=new QPushButton(QStringLiteral("导出匹配清单…"));
    auto rerun=new QPushButton(QStringLiteral("复测筛选中的失败 / 超时"));rerun->setObjectName("rerunFailures");
    toolbar->addWidget(status);toolbar->addWidget(search,1);toolbar->addWidget(rerun);toolbar->addWidget(exportButton);layout->addLayout(toolbar);
    auto rows=table({QStringLiteral("Testcase"),QStringLiteral("状态"),QStringLiteral("阶段 / 原因"),QStringLiteral("操作")});rows->setObjectName("caseResults");rows->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);rows->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);rows->setColumnWidth(1,90);rows->setColumnWidth(2,235);rows->setColumnWidth(3,165);rows->verticalHeader()->setDefaultSectionSize(66);layout->addWidget(GcrpUi::card(rows),1);
    auto footer=new QHBoxLayout;auto count=new QLabel;count->setObjectName("muted");footer->addWidget(count,1);auto prev=new QPushButton(QStringLiteral("上一页"));auto next=new QPushButton(QStringLiteral("下一页"));footer->addWidget(prev);footer->addWidget(next);layout->addLayout(footer);
    auto pageIndex=std::make_shared<int>(0);auto matches=std::make_shared<Json>(Json::array());
    connect(rerun,&QPushButton::clicked,dialog,[this,matches]{rerunFailures(*matches);});
    auto paint=[this,rows,search,status,snapshot,count,pageIndex,matches,prev,next]{
        *matches=Json::array();rows->setRowCount(0);
        for(const auto& task:*snapshot){const auto state=text(task,"status"),filter=status->currentData().toString();const auto payload=task.value("payload",Json::object());const auto path=text(payload,"case");
            if(!filter.isEmpty()&&outcome(task)!=filter&&!(filter=="RUNNING"&&state=="ASSIGNED")&&!(filter=="QUEUED"&&state=="PENDING"))continue;
            if(!(path+" "+text(task,"worker_id")).contains(search->text().trimmed(),Qt::CaseInsensitive))continue;matches->push_back(task);
        }
        const int pages=qMax(1,(int(matches->size())+9)/10);*pageIndex=qBound(0,*pageIndex,pages-1);
        for(int i=*pageIndex*10;i<qMin(int(matches->size()),(*pageIndex+1)*10);++i){const auto task=(*matches)[i];const auto state=text(task,"status"),path=text(task.value("payload",Json::object()),"case");const auto execution=task.value("execution",Json::object());const int row=rows->rowCount();rows->insertRow(row);
            cell(rows,row,0,path);rows->item(row,0)->setToolTip(path+"\nTask #"+text(task,"task_id")+" · "+text(task,"worker_id"));
            rows->setCellWidget(row,1,GcrpUi::badge(outcome(task)));
            cell(rows,row,2,text(execution,"case_reason")+"\n"+text(execution,"reason"));
            auto controls=new QWidget;auto actions=new QHBoxLayout(controls);actions->setContentsMargins(4,4,4,4);auto logs=new QPushButton(QStringLiteral("原始 run"));auto details=new QPushButton(QStringLiteral("详情"));actions->addWidget(logs);actions->addWidget(details);rows->setCellWidget(row,3,controls);
            connect(logs,&QPushButton::clicked,rows,[this,task]{openLogs(task);});connect(details,&QPushButton::clicked,rows,[this,task]{openTask(task["task_id"].get<uint64_t>());});
        }
        count->setText(QStringLiteral("匹配 %1 / 快照 %2 条　 ·　 第 %3/%4 页").arg(matches->size()).arg(snapshot->size()).arg(*pageIndex+1).arg(pages));prev->setEnabled(*pageIndex>0);next->setEnabled(*pageIndex+1<pages);
    };
    connect(search,&QLineEdit::textChanged,dialog,[paint,pageIndex]{*pageIndex=0;paint();});connect(status,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[paint,pageIndex]{*pageIndex=0;paint();});
    connect(prev,&QPushButton::clicked,dialog,[paint,pageIndex]{--*pageIndex;paint();});connect(next,&QPushButton::clicked,dialog,[paint,pageIndex]{++*pageIndex;paint();});
    connect(exportButton,&QPushButton::clicked,dialog,[dialog,matches]{
        QDialog popup(dialog);popup.setObjectName("exportDialog");popup.setWindowTitle(QStringLiteral("导出 testcase 清单"));popup.resize(720,610);
        auto box=new QVBoxLayout(&popup);box->setContentsMargins(24,22,24,20);box->setSpacing(14);
        auto title=new QLabel(QStringLiteral("导出 testcase 清单"));title->setObjectName("dialogTitle");box->addWidget(title);
        auto notice=new QLabel(QStringLiteral("覆盖当前筛选的所有匹配记录，不只本页。仅限当前已加载快照，不保证包含整个历史批次。"));notice->setWordWrap(true);notice->setObjectName("notice");box->addWidget(notice);
        auto formats=new QComboBox;formats->setObjectName("exportFormat");formats->addItem(QStringLiteral("TXT · run.tcl 相对路径"));formats->addItem(QStringLiteral("CSV · Task / Run / 路径 / 结果"));box->addWidget(formats);
        auto grid=new QGridLayout;QMap<QString,QCheckBox*> checks;const QStringList states{"PASS","FAIL","TIMEOUT","CRASH","ERROR","UNKNOWN","CANCELLED","RUNNING","QUEUED"};
        for(int i=0;i<states.size();++i){auto check=new QCheckBox(states[i]);check->setChecked(i<6);checks[states[i]]=check;grid->addWidget(check,i/5,i%5);}box->addLayout(grid);
        auto zip=new QCheckBox(QStringLiteral("同时打包执行证据 ZIP · 后端待接入"));zip->setEnabled(false);box->addWidget(zip);
        auto count=new QLabel;count->setObjectName("exportCount");box->addWidget(count);auto preview=new QPlainTextEdit;preview->setObjectName("exportPreview");preview->setReadOnly(true);box->addWidget(preview,1);
        Json filtered=Json::array();const auto source=*matches;
        auto paint=[&filtered,source,checks,count,preview]{filtered=Json::array();QStringList paths;
            for(const auto& task:source){auto state=outcome(task);if(state=="ASSIGNED")state="RUNNING";if(state=="PENDING")state="QUEUED";
                const auto path=text(task.value("payload",Json::object()),"case");if(checks.contains(state)&&checks[state]->isChecked()&&!path.isEmpty()&&path!="—"){filtered.push_back(task);if(paths.size()<12)paths<<path;}}
            count->setText(QStringLiteral("将导出 %1 条记录 · 下方仅预览前 12 条").arg(filtered.size()));preview->setPlainText(paths.join("\n"));};
        for(auto check:checks)QObject::connect(check,&QCheckBox::toggled,&popup,[paint]{paint();});
        auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("选择位置并导出"));buttons->button(QDialogButtonBox::Save)->setProperty("kind","primary");buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));box->addWidget(buttons);
        QObject::connect(buttons,&QDialogButtonBox::accepted,&popup,&QDialog::accept);QObject::connect(buttons,&QDialogButtonBox::rejected,&popup,&QDialog::reject);paint();
        if(popup.exec()!=QDialog::Accepted)return;if(filtered.empty()){QMessageBox::information(dialog,QStringLiteral("没有匹配记录"),QStringLiteral("请先勾选需要导出的结果类型。"));return;}
        QString format=formats->currentIndex()==0?QStringLiteral("路径清单 (*.txt)"):QStringLiteral("结果明细 (*.csv)");auto path=QFileDialog::getSaveFileName(dialog,QStringLiteral("导出所有匹配行（当前快照，不只本页）"),formats->currentIndex()==0?"testcases.txt":"testcases.csv","路径清单 (*.txt);;结果明细 (*.csv)",&format);if(path.isEmpty())return;
        const bool csv=format.contains("csv");QByteArray output=csv?QByteArray("\xEF\xBB\xBFtask_id,run_id,testcase,status,outcome,worker_id,revision,reason\r\n"):QByteArray{};
        int exported=0;
        for(const auto& task:filtered){auto path=text(task.value("payload",Json::object()),"case");if(path.isEmpty()||path=="—")continue;
            if(csv){QStringList columns{text(task,"task_id"),text(task,"run_id"),path,text(task,"status"),outcome(task),text(task,"worker_id"),text(task,"revision"),text(task.value("execution",Json::object()),"reason")};for(auto& column:columns)column=csvCell(column);output+=columns.join(",").toUtf8()+"\r\n";}
            else output+=path.toUtf8()+"\n";++exported;
        }
        QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(output)!=output.size()||!file.commit())QMessageBox::warning(dialog,QStringLiteral("导出失败"),file.errorString());
        else QMessageBox::information(dialog,QStringLiteral("已导出"),QStringLiteral("保存 %1 条匹配记录；仅覆盖已加载快照，非全库导出。").arg(exported));
    });paint();dialog->show();
}

void MainWindow::rerunFailures(const Json& rows) {
    if(submitting_||loading_){note(QStringLiteral("当前窗口有提交 / 查询操作，请稍后再试。"));return;}
    auto ids=std::make_shared<Json>(Json::array());
    for(const auto& task:rows)if(text(task,"status")=="FAILED"||text(task,"status")=="TIMEOUT")ids->push_back(task.at("task_id"));
    if(ids->empty()){note(QStringLiteral("当前筛选中没有失败或超时案例。"));return;}
    if(demo_){note(QStringLiteral("演示：将创建独立人工复测批次，不修改原每日回归。"));return;}
    if(!operations_.contains("submit_task")||!operations_.contains("query_task"))return;
    if(!operations_.contains("query_catalog")){note(QStringLiteral("Server 不支持 ZIP 目录查询，请更新 Server 后复测。"),true);return;}
    submitting_=true;submit_->setEnabled(false);pendingSubmissions_=Json::array();submitIndex_=0;
    manualBatch_="rerun-"+QUuid::createUuid().toString().mid(1,36);
    loadRerun(ids,0);
}
void MainWindow::loadRerun(std::shared_ptr<Json> ids,size_t index) {
    if(index==ids->size()){confirmRerun();return;}
    rpc_.request("query_task",{{"task_id",(*ids)[index]}},[this,ids,index](RpcReply reply){
        try{
            if(!reply.ok)throw std::runtime_error(reply.error.toStdString());
            (void)rerunTask(reply.result,manualBatch_.toStdString(),index);
            pendingSubmissions_.push_back(reply.result);
            loadRerun(ids,index+1);
        }catch(const std::exception& e){
            submitting_=false;submit_->setEnabled(true);pendingSubmissions_=Json::array();
            QMessageBox::warning(this,QStringLiteral("复测准备失败 · 未提交任何案例"),QString::fromUtf8(e.what()));
        }
    });
}

void MainWindow::confirmRerun() {
    // Refresh the Server-owned ZIP list; never trust a stale local catalog.
    rpc_.request("query_catalog",{{"refresh",true}},[this](RpcReply reply){
        auto release=[this]{submitting_=false;submit_->setEnabled(true);pendingSubmissions_=Json::array();};
        try{
            if(!reply.ok)throw std::runtime_error(reply.error.toStdString());
            if(reply.result.at("sources").at("server.artifact_root")!="READY")throw std::runtime_error("Server ZIP directory is not readable");
            QStringList available,originals;
            for(const auto& value:reply.result.at("revisions")){
                const auto revision=value.get<std::string>();
                if(revision.empty()||revision.size()>32||revision.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("invalid ZIP revision list");
                available<<QString::fromStdString(revision);
            }
            for(const auto& source:pendingSubmissions_){auto revision=text(source,"revision");if(!originals.contains(revision))originals<<revision;}
            RerunSelectionDialog dialog(int(pendingSubmissions_.size()),originals,available,this);
            if(dialog.exec()!=QDialog::Accepted){release();return;}
            const auto revision=dialog.selectedRevision().toStdString();Json requests=Json::array();
            for(size_t i=0;i<pendingSubmissions_.size();++i)requests.push_back(rerunTask(pendingSubmissions_[i],manualBatch_.toStdString(),i,revision));
            if(originals.size()>1)for(auto& request:requests)request["payload"]["context"]["name"]=revision.empty()?"人工复测 · 保留各自原版本":"人工复测 · 多版本 -> r"+revision;
            pendingSubmissions_=std::move(requests);submitNext();
        }catch(const std::exception& e){release();QMessageBox::warning(this,QStringLiteral("复测准备失败 · 未提交任何案例"),QString::fromUtf8(e.what()));}
    });
}

void MainWindow::openTask(uint64_t id) {
    auto display=[this](RpcReply r){if(!r.ok){QMessageBox::warning(this,QStringLiteral("查询失败"),r.error);return;}
        auto dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(QStringLiteral("Task 详情与日志"));dialog->resize(850,650);auto layout=new QVBoxLayout(dialog);
        auto body=new QPlainTextEdit(QString::fromStdString(r.result.dump(2)));body->setReadOnly(true);layout->addWidget(body,1);
        const auto state=text(r.result,"status");auto deletion=new QPushButton(QStringLiteral("删除此 testcase 记录"));deletion->setProperty("kind","danger");
        deletion->setEnabled(!demo_&&operations_.contains("delete_task")&&(state=="SUCCEEDED"||state=="FAILED"||state=="TIMEOUT"||state=="CANCELLED"));layout->addWidget(deletion);
        connect(deletion,&QPushButton::clicked,dialog,[this,dialog,id=r.result.at("task_id")]{
            if(QMessageBox::warning(dialog,QStringLiteral("确认删除记录"),QStringLiteral("仅删除该 testcase 的查询记录，保留日志与防重复提交信息。此操作不能从 GUI 撤销。"),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes)return;
            rpc_.request("delete_task",{{"task_id",id},{"confirm",true}},[this,dialog=QPointer<QDialog>(dialog)](RpcReply reply){
                if(!reply.ok){note(QStringLiteral("删除未确认，请刷新核对：")+reply.error,true);return;}if(dialog)dialog->close();refresh();});
        });
        auto logs=new QPushButton(QStringLiteral("查看 Server 归档日志"));layout->addWidget(logs);connect(logs,&QPushButton::clicked,this,[this,task=r.result]{openLogs(task);});dialog->show();};
    if(demo_){Json t={{"task_id",id},{"run_id","demo-run"},{"status","SUCCEEDED"},{"payload",{{"case","convergentRoundingEven/run.tcl"},{"flow","route"}}}};display({true,{},t,{}});}
    else rpc_.request("query_task",{{"task_id",id}},display);
}

void MainWindow::openLogs(const Json& task) {
    if(!task.contains("run_id")||text(task,"run_id").isEmpty()||text(task,"run_id")=="—"||text(task,"status")=="RUNNING"||text(task,"status")=="ASSIGNED"||text(task,"status")=="QUEUED"||text(task,"status")=="PENDING"){
        QMessageBox::information(this,QStringLiteral("日志待生成 / 待上传"),QStringLiteral("当前 API 提供执行结束后的归档证据。正在执行或缺少 run_id 时不能读取；缺少文件不代表失败。"));return;
    }
    auto dialog=new QDialog(this);dialog->setObjectName("logDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(QStringLiteral("原始 run · GalaxCore 执行证据"));dialog->resize(1100,740);
    auto layout=new QVBoxLayout(dialog);layout->setContentsMargins(24,22,24,20);layout->setSpacing(14);
    auto titleRow=new QHBoxLayout;auto title=new QLabel(QStringLiteral("原始 run / 执行证据"));title->setObjectName("dialogTitle");titleRow->addWidget(title,1);auto close=new QPushButton(QStringLiteral("关闭"));titleRow->addWidget(close);connect(close,&QPushButton::clicked,dialog,&QDialog::close);layout->addLayout(titleRow);
    auto description=new QLabel(text(task.value("payload",Json::object()),"case")+"\nTask #"+text(task,"task_id")+" · "+text(task,"status")+" · "+text(task,"run_id"));description->setTextFormat(Qt::PlainText);description->setWordWrap(true);description->setObjectName("muted");layout->addWidget(description);
    auto warning=new QLabel(QStringLiteral("优先查看原始 run。run.log 是执行器日志，不能替代 GalaxCore 输出。下载仅包含 Server 已采集的归档，不能保证源文件未被截断。"));warning->setObjectName("notice");warning->setWordWrap(true);layout->addWidget(warning);
    auto lifecycle=new QLabel(QStringLiteral("正在查询归档状态…"));lifecycle->setWordWrap(true);lifecycle->setObjectName("archiveLifecycle");layout->addWidget(lifecycle);
    auto reload=new QPushButton(QStringLiteral("刷新归档状态"));titleRow->insertWidget(1,reload);
    auto split=new QSplitter;auto files=new QListWidget;files->setObjectName("logFiles");files->setMinimumWidth(165);files->setMaximumWidth(220);split->addWidget(files);
    auto content=new QWidget;auto box=new QVBoxLayout(content);box->setContentsMargins(0,0,0,0);box->setSpacing(0);
    auto toolbar=new QHBoxLayout;auto search=new QLineEdit;search->setPlaceholderText(QStringLiteral("查找当前片段 · 回车跳到下一处"));auto next=new QPushButton(QStringLiteral("下一段"));next->setEnabled(false);auto download=new QPushButton(QStringLiteral("下载已归档文件"));download->setEnabled(false);toolbar->addWidget(search,1);toolbar->addWidget(next);toolbar->addWidget(download);box->addLayout(toolbar);
    auto body=new GcrpUi::NumberedLog;body->setReadOnly(true);body->setObjectName("logBody");box->addWidget(body,1);split->addWidget(content);split->setSizes({175,850});layout->addWidget(split,1);
    auto status=new QLabel(QStringLiteral("读取当前 run_id 的归档清单…"));status->setTextFormat(Qt::PlainText);status->setWordWrap(true);status->setObjectName("muted");layout->addWidget(status);
    connect(search,&QLineEdit::textChanged,dialog,[body](const QString& query){QList<QTextEdit::ExtraSelection> hits;if(!query.isEmpty()){QTextCursor cursor(body->document());for(int i=0;i<1000;++i){cursor=body->document()->find(query,cursor);if(cursor.isNull())break;QTextEdit::ExtraSelection hit;hit.cursor=cursor;hit.format.setBackground(QColor("#d2b25a"));hit.format.setForeground(QColor("#15243c"));hits<<hit;}}body->setExtraSelections(hits);});
    connect(search,&QLineEdit::returnPressed,dialog,[body,search]{if(!body->find(search->text())){auto cursor=body->textCursor();cursor.movePosition(QTextCursor::Start);body->setTextCursor(cursor);body->find(search->text());}});
    auto offset=std::make_shared<uint64_t>(0);auto reading=std::make_shared<bool>(false);auto downloading=std::make_shared<bool>(false);QPointer<QDialog> guard(dialog);
    auto read=std::make_shared<std::function<void()>>();
    *read=[this,guard,task,files,next,download,status,body,offset,reading,downloading,search]{
        if(!guard||*reading||*downloading||!files->currentItem())return;const auto name=files->currentItem()->data(Qt::UserRole).toString();
        if(demo_){const auto state=text(task,"status");QString value;
            if(name=="evidence.run")value=QStringLiteral("# 原始 run 示例（非真实业务证据）\nGalaxCore Regression Platform · demo\nCASE=")+text(task.value("payload",Json::object()),"case")+QStringLiteral("\n[PHASE] read_edif\n[PHASE] place_design\n[PHASE] route_design\n")+(state=="FAILED"?QStringLiteral("[ERROR] route_design: example routing failure\nlast confirmed phase: route_design\n"):QStringLiteral("[INFO] route_design finished\n[PHASE] write_bitstream\n[COMPARE] PASS\n"));
            else if(name=="result.env")value="STATUS="+QString(state=="SUCCEEDED"?"PASS":"FAIL")+"\nREASON=DEMO_ONLY\n";
            else if(name=="flow_config")value=QString::fromStdString(task.value("payload",Json::object()).value("flow_config",Json::object()).dump(2));
            else value=QStringLiteral("# 执行器日志示例（不是原始 run）\n[RUNNER] prepare dedicated workspace\n[RUNNER] launch GalaxCore\n[RESULT] ")+state;
            body->setPlainText(value);status->setText(QStringLiteral("离线示例 · 与真实 Server 隔离"));download->setEnabled(false);return;
        }
        *reading=true;files->setEnabled(false);next->setEnabled(false);download->setEnabled(false);const auto requestOffset=*offset;
        rpc_.request("read_log",{{"task_id",task.at("task_id")},{"run_id",task.at("run_id")},{"name",name.toStdString()},{"offset",requestOffset},{"limit",settings_.logPageBytes}},
          [guard,files,next,download,status,body,offset,reading,name,requestOffset,search](RpcReply r){if(!guard)return;*reading=false;files->setEnabled(true);if(!r.ok){status->setText(r.error);return;}try{
            const auto hex=r.result.at("data_hex").get<std::string>();const auto bytes=QByteArray::fromHex(QByteArray(hex.data(),int(hex.size())));const auto size=r.result.at("size").get<uint64_t>(),end=r.result.at("next_offset").get<uint64_t>();
            if(end<requestOffset||end>size||end-requestOffset!=uint64_t(bytes.size()))throw std::runtime_error("invalid log offset");
            *offset=end;body->setPlainText(QString::fromUtf8(bytes));search->clear();
            status->setText(QStringLiteral("%1 · 片段 [%2, %3) / %4 字节 · %5\n仅搜索当前片段；UTF-8 边界/非法字节可能替换显示，下载保留原始归档字节").arg(name).arg(qulonglong(requestOffset)).arg(qulonglong(end)).arg(qulonglong(size)).arg(r.result.value("complete",false)?QStringLiteral("归档上传完成"):QStringLiteral("上传中 / 非完整证据")));
            next->setEnabled(end<size);download->setEnabled(r.result.value("complete",false));
          }catch(const std::exception& e){status->setText(QString::fromUtf8(e.what()));}});
    };
    connect(files,&QListWidget::currentRowChanged,dialog,[offset,reading,read,body]{*offset=0;body->clear();if(!*reading)(*read)();});
    connect(next,&QPushButton::clicked,dialog,[read]{(*read)();});
    connect(download,&QPushButton::clicked,dialog,[this,guard,task,files,next,download,status,reading,downloading,offset]{
        if(*reading||*downloading||!files->currentItem()||demo_)return;const auto name=files->currentItem()->data(Qt::UserRole).toString();
        const auto path=QFileDialog::getSaveFileName(guard,QStringLiteral("保存 Server 已归档文件（不是未采集的完整源文件）"),name);if(path.isEmpty())return;
        const auto digest=files->currentItem()->data(Qt::UserRole+1).toString();
        if(digest.isEmpty()&&QMessageBox::question(guard,QStringLiteral("历史归档 · 未校验"),
            QStringLiteral("此文件没有 Server SHA-256 收据。可下载已有归档字节，但不能证明源文件采集完整或提供校验续传。\n仍要完整下载吗？"))!=QMessageBox::Yes)return;
        *downloading=true;files->setEnabled(false);next->setEnabled(false);download->setEnabled(false);
        auto transfer=new ArchiveDownload(rpc_,{{"task_id",task.at("task_id")},{"run_id",task.at("run_id")},{"name",name.toStdString()}},path,digest,
            [guard,status,digest](uint64_t at,uint64_t total){if(guard)status->setText((digest.isEmpty()?
                QStringLiteral("完整下载历史归档 %1 / %2 字节；无 Server 校验收据，不支持续传。"):
                QStringLiteral("下载并校验 %1 / %2 字节；中断后选择相同目标文件继续。")).arg(qulonglong(at)).arg(qulonglong(total)));},
            [guard,status,files,next,download,downloading](bool ok,QString message){
                if(!guard)return;*downloading=false;files->setEnabled(true);download->setEnabled(true);next->setEnabled(false);
                status->setText((ok?QStringLiteral("下载完成："):QStringLiteral("下载未完成："))+message);
            },guard);
        transfer->start();
    });
    dialog->show();
    if(demo_){files->addItem(QStringLiteral("原始 run"));files->item(0)->setData(Qt::UserRole,"evidence.run");for(const auto& name:QStringList{"result.env","flow_config","run.log","stdout.log","stderr.log"}){auto item=new QListWidgetItem(name,files);item->setData(Qt::UserRole,name);}files->setCurrentRow(0);(*read)();return;}
    auto load=[this,guard,task,files,status,lifecycle,read,reading,downloading,offset]{
      if(!guard||*reading||*downloading)return;
      rpc_.request("query_logs",{{"task_id",task.at("task_id")},{"run_id",task.at("run_id")}},[guard,files,status,lifecycle,read,offset](RpcReply r){if(!guard)return;if(!r.ok){status->setText(r.error);return;}try{
        const QMap<QString,QString> states{{"NOT_REPORTED",QStringLiteral("未收到归档清单：可能待上传或旧 Worker 未上报，不能判断已过期")},
            {"UPLOADING",QStringLiteral("上传中 / 文件集尚未全部校验")},{"UPLOADED_UNVERIFIED",QStringLiteral("已有归档，但缺少完整文件集校验")},
            {"SERVER_VERIFIED",QStringLiteral("Server 已校验完整文件集；Worker 清理尚未确认")},
            {"LOCAL_CLEANED",QStringLiteral("Server 已校验完整文件集；Worker 已确认本地任务日志清理")},
            {"EXPIRED",QStringLiteral("归档已按保留期清理；任务结果和到期标记保留")}};
        const auto state=text(r.result,"archive_state");lifecycle->setText(states.value(state,QStringLiteral("旧 Server 未提供归档状态，缺少文件不能推断原因。")));
        auto items=r.result.at("items");std::stable_sort(items.begin(),items.end(),[](const Json& a,const Json& b){return (text(a,"name")=="evidence.run")>(text(b,"name")=="evidence.run");});
        {QSignalBlocker block(files);files->clear();*offset=0;for(const auto& file:items){const auto name=text(file,"name");auto item=new QListWidgetItem((name=="evidence.run"?QStringLiteral("原始 run"):name)+(file.value("complete",false)?"":QStringLiteral(" · 上传中")),files);item->setData(Qt::UserRole,name);item->setData(Qt::UserRole+1,QString::fromStdString(file.value("sha256",std::string{})));item->setToolTip(name);}if(files->count())files->setCurrentRow(0);}
        if(items.empty())status->setText(QStringLiteral("当前没有可读取文件。请查看上方归档状态。"));else (*read)();
    }catch(const std::exception& e){status->setText(QString::fromUtf8(e.what()));}});
    };
    connect(reload,&QPushButton::clicked,dialog,load);load();
}
