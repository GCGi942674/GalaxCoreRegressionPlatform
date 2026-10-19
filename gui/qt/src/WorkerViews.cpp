#include "ConsoleSupport.h"

void MainWindow::showWorkers() {
    const auto selected=machines_->currentText();QSet<QString> machineNames;
    const auto visible=[this](const Json& w){return showOffline_->isChecked()||text(w,"status")!="OFFLINE"||w.value("used",0)>0;};
    for(const auto& w:workers_)if(visible(w))machineNames.insert(text(w,"machine_id"));
    {QSignalBlocker blocked(machines_);machines_->clear();auto list=machineNames.values();list.sort();machines_->addItems(list);if(list.contains(selected))machines_->setCurrentText(selected);}
    {QSignalBlocker blocked(hostsList_);hostsList_->clear();
        for(int i=0;i<machines_->count();++i){const auto name=machines_->itemText(i);int slotCount=0,busy=0;bool online=false;
            for(const auto& w:workers_)if(text(w,"machine_id")==name&&visible(w)){++slotCount;busy+=w.value("used",0)>0;online=online||text(w,"status")=="ONLINE";}
            auto item=new QListWidgetItem(hostsList_);item->setData(Qt::UserRole,name);item->setSizeHint(QSize(180,90));
            const auto status=workersStale_&&!demo_?QStringLiteral("？ 数据过期"):
                online?QStringLiteral("<span style='color:#3b82f6'>●</span> 在线"):
                       QStringLiteral("<span style='color:#94a3b8'>●</span> 离线");
            auto label=new QLabel(name.toHtmlEscaped()+"<br><span style='color:#8493a8;font-size:11px'>"+status+QStringLiteral("<br>%1 slots · %2 运行</span>").arg(slotCount).arg(busy));label->setContentsMargins(14,8,8,8);label->setAttribute(Qt::WA_TransparentForMouseEvents);hostsList_->setItemWidget(item,label);
        }
        hostsList_->setCurrentRow(machines_->currentIndex());}
    slotsTable_->setRowCount(0);hostTitle_->setText(machines_->currentText().isEmpty()?QStringLiteral("暂无在线工作机"):machines_->currentText());
    bool anyOnline=false;int onlineCount=0,offlineCount=0,legacyCount=0;
    for(const auto& w:workers_)if(text(w,"machine_id")==machines_->currentText()){
        if(text(w,"status")=="OFFLINE")++offlineCount;else ++onlineCount;
        if(!w.value("control_supported",false)&&text(w,"status")!="OFFLINE")++legacyCount;
        if(!visible(w))continue;
        int row=slotsTable_->rowCount();slotsTable_->insertRow(row);cell(slotsTable_,row,0,text(w,"slot_id")+"\n"+text(w,"worker_id"));
        auto status=text(w,"status");if(status=="ONLINE")status+=w.value("used",0)>0?QStringLiteral(" · 忙碌"):QStringLiteral(" · 空闲");
        if(!demo_&&workersStale_)status=QStringLiteral("未知 · 数据过期（上次：")+status+QStringLiteral("）");
        cell(slotsTable_,row,1,status);
        const bool online=text(w,"status")=="ONLINE";anyOnline=anyOnline||online;
        if(!demo_&&!w.value("control_supported",false))slotsTable_->item(row,0)->setToolTip(QStringLiteral("此进程未声明维护协议 V1，可能为旧版 Worker；槽位来自 Server 注册/心跳，不按磁盘目录推算。"));
        const bool accepting=w.value("accepting_tasks",true),requested=w.value("accepting_requested",accepting);
        const auto maintenance=w.value("maintenance",Json::object());
        const auto phase=QString::fromStdString(maintenance.value("state",std::string("IDLE")));
        const bool pending=phase=="PENDING"||phase=="RUNNING";
        cell(slotsTable_,row,2,text(w,"status")=="OFFLINE"?QStringLiteral("离线 / 不接单（保留策略）"):
            text(w,"status")=="DRAINING"?QStringLiteral("退出排空中 / 不接新任务"):
            accepting?QStringLiteral("允许接单"):w.value("used",0)>0?QStringLiteral("排空中 / 不接新任务"):QStringLiteral("已停止接单"));
        const auto maintenanceText=phase=="PENDING"?QStringLiteral("等待排空"):phase=="RUNNING"?QStringLiteral("SVN 更新中"):phase=="FAILED"?QStringLiteral("更新失败 / 保持停派"):phase=="SUCCEEDED"?QStringLiteral("更新完成"):QString{};
        if(!maintenanceText.isEmpty())slotsTable_->item(row,2)->setText(slotsTable_->item(row,2)->text()+"\n"+maintenanceText);
        slotsTable_->item(row,2)->setToolTip(QString::fromStdString(maintenance.dump(2)));
        auto controls=new QWidget;auto actions=new QHBoxLayout(controls);actions->setContentsMargins(6,2,6,2);actions->setSpacing(8);
        const auto worker=text(w,"worker_id");
        auto toggle=new QPushButton(requested?QStringLiteral("下线"):QStringLiteral("上线"));toggle->setObjectName("slotAcceptButton");
        auto update=new QPushButton(QStringLiteral("更新"));update->setObjectName("slotUpdateButton");actions->addWidget(toggle);actions->addWidget(update);
        // Cell widgets are not reliably included by ResizeToContents on older Qt5.
        // Reserve both button widths explicitly; a narrow window scrolls instead of clipping.
        for(auto button:{toggle,update})button->setMinimumWidth(qMax(112,button->sizeHint().width()));
        controls->setMinimumWidth(actions->minimumSize().width());
        slotsTable_->setColumnWidth(4,qMax(slotsTable_->columnWidth(4),controls->minimumWidth()+16));
        const bool enabled=(demo_||(!workersStale_&&operations_.contains("control_workers")))&&!managementPending_&&online;
        toggle->setToolTip(QStringLiteral("仅改变接单策略，不终止正在运行的 testcase。"));
        update->setToolTip(QStringLiteral("排空当前任务后更新独立 SVN 副本；需要新版 Worker 支持。"));
        toggle->setEnabled(enabled&&(requested||!pending));
        update->setEnabled(enabled&&!pending&&(demo_||w.value("control_supported",false)));
        connect(toggle,&QPushButton::clicked,this,[this,worker,requested]{controlWorkers(false,worker,requested?"pause":"resume");});
        connect(update,&QPushButton::clicked,this,[this,worker]{controlWorkers(false,worker,"update");});
        slotsTable_->setCellWidget(row,4,controls);
        cell(slotsTable_,row,3,(w.value("used",0)>0?QStringLiteral("已占用"):QStringLiteral("空闲"))+"\n"+QStringLiteral("容量 ")+text(w,"used")+"/"+text(w,"capacity"));for(const auto& t:tasks_)if(text(t,"worker_id")==text(w,"worker_id")&&(text(t,"status")=="RUNNING"||text(t,"status")=="ASSIGNED")){
            cell(slotsTable_,row,3,"Task #"+text(t,"task_id")+"\n"+text(t.value("payload",Json::object()),"case"));slotsTable_->item(row,3)->setData(Qt::UserRole,text(t,"task_id"));break;
        }
    }
    workerSummary_->setText((demo_?QStringLiteral("【演示数据】 "):workersStale_?QStringLiteral("【过期快照】 "):QStringLiteral("【Server 注册信息】 "))+
        QStringLiteral("%1：在线/排空 %2，离线历史 %3；其中 %4 个在线 slot 未声明新维护协议。目录数量不等于运行进程数量。")
        .arg(machines_->currentText()).arg(onlineCount).arg(offlineCount).arg(legacyCount));
    if(machines_->count()==0)workerSummary_->setText(workersStale_?QStringLiteral("尚未确认 Server 工作机信息，请检查连接。"):
        QStringLiteral("当前没有在线工作机。可勾选查看离线历史 slots；不会用演示数据填充。"));
    for(auto button:hostControlButtons_){button->setEnabled((demo_||(!workersStale_&&operations_.contains("control_workers")))&&anyOnline&&!managementPending_);button->setToolTip(QStringLiteral("管理当前整机 slots；恢复接单仅影响 ONLINE slots。"));}
}

void MainWindow::controlWorkers(bool machine,const QString& target,const QString& action) {
    if(target.isEmpty()||managementPending_)return;
    if(!demo_&&(workersStale_||!operations_.contains("control_workers"))){
        note(QStringLiteral("状态过期或 Server 不支持管理接口，请先刷新 / 升级。"),true);return;}
    QSignalBlocker blockRefresh(refreshTimer_),blockFreshness(freshnessTimer_);
    QString scope;
    if(action=="update"){
        bool ok=false;const auto choice=QInputDialog::getItem(this,QStringLiteral("SVN 更新范围"),
            QStringLiteral("仅更新工作机本地的独立 SVN 副本"),
            {QStringLiteral("galaxcore/test2"),QStringLiteral("完整 GalaxCore 副本")},0,false,&ok);
        if(!ok)return;scope=choice==QStringLiteral("galaxcore/test2")?"test2":"all";
    }
    const auto explanation=action=="pause"?QStringLiteral("停止接收新任务，已分配 / 运行中的 testcase 正常结束。"):
        action=="resume"?QStringLiteral("仅恢复当前 ONLINE slots；OFFLINE / DRAINING 的策略保持不变，不会远程启动机器。维护失败后请先检查工作副本。"):
        QStringLiteral("先停止派发，等待当前执行和结果上报结束，再更新 SVN。不会 revert 或删除本地修改；冲突会保持停派。成功后恢复更新前的接单意向。");
    if(QMessageBox::question(this,QStringLiteral("确认工作机管理操作"),target+"\n"+explanation)!=QMessageBox::Yes)return;
    if(demo_){
        for(auto& w:workers_)if(text(w,machine?"machine_id":"worker_id")==target){
            if(action=="resume"&&text(w,"status")!="ONLINE")continue;
            if(action=="update")w["maintenance"]={{"state","PENDING"},{"scope",scope.toStdString()}};
            w["accepting_tasks"]=action=="resume";w["accepting_requested"]=action=="update"?w.value("accepting_requested",true):action=="resume";
        }showWorkers();note(QStringLiteral("仅演示：已修改本地示例，未向 Server 或 SVN 发送命令。"));return;
    }
    Json request={{machine?"machine_id":"worker_id",target.toStdString()},{"action",action.toStdString()},
        {"operation_id",QUuid::createUuid().toString().mid(1,36).toStdString()}};
    if(action=="update")request["scope"]=scope.toStdString();
    managementPending_=true;showWorkers();
    rpc_.request("control_workers",request,[this,action](RpcReply r){
        managementPending_=false;
        if(!r.ok){showWorkers();note(QStringLiteral("管理请求未确认：")+r.error+QStringLiteral("。请刷新核对，不自动重复操作。"),true);return;}
        note(action=="update"?QStringLiteral("更新请求已接受；请查看维护列，只有 Worker 确认后才算完成。"):
            QStringLiteral("接单策略已保存，当前 testcase 不受影响。"));
        refresh();
    });
}
