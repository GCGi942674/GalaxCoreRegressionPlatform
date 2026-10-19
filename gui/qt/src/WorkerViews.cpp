#include "ConsoleSupport.h"

namespace {
uint64_t occupiedSlots(const Json& worker) {
    return qMax(uint64_t(worker.value("used",0)),worker.value("current_tasks_total",uint64_t(0)));
}
bool dispatchEnabled(const Json& worker) {
    return text(worker,"status")=="ONLINE"&&worker.value("accepting_tasks",true);
}
QString workState(const Json& worker,bool stale) {
    if(stale)return QStringLiteral("UNKNOWN · 数据过期");
    const auto connection=text(worker,"status");const bool busy=occupiedSlots(worker)>0;
    if(connection=="OFFLINE")return busy?QStringLiteral("OFFLINE · 失联待处理"):QStringLiteral("OFFLINE · 连接中断");
    if(worker.contains("current_tasks")&&worker["current_tasks"].is_array())for(const auto& task:worker["current_tasks"])
        if(task.value("recovery_required",false))return QStringLiteral("UNKNOWN · 待恢复确认");
    if(connection=="DRAINING"||!worker.value("accepting_requested",worker.value("accepting_tasks",true)))
        return busy?QStringLiteral("DRAINING · 下线中"):QStringLiteral("OFFLINE · 已下线");
    const auto phase=text(worker.value("maintenance",Json::object()),"state");
    if(phase=="PENDING")return QStringLiteral("MAINTENANCE · 等待更新");
    if(phase=="RUNNING")return QStringLiteral("MAINTENANCE · 更新中");
    if(phase=="FAILED")return QStringLiteral("OFFLINE · 更新失败");
    if(!dispatchEnabled(worker))return QStringLiteral("OFFLINE · 暂不可调度");
    if(worker.contains("current_tasks")&&worker["current_tasks"].is_array())for(const auto& task:worker["current_tasks"]){
        if(text(task,"status")=="RUNNING")return QStringLiteral("ONLINE · 执行中");
    }
    return busy?QStringLiteral("ONLINE · 准备执行"):QStringLiteral("ONLINE · 已上线");
}
}

void MainWindow::showWorkers() {
    const auto selected=machines_->currentText();QSet<QString> machineNames;
    const auto visible=[this](const Json& w){return showOffline_->isChecked()||text(w,"status")!="OFFLINE"||occupiedSlots(w)>0;};
    for(const auto& w:workers_)if(visible(w))machineNames.insert(text(w,"machine_id"));
    {QSignalBlocker blocked(machines_);machines_->clear();auto list=machineNames.values();list.sort();machines_->addItems(list);if(list.contains(selected))machines_->setCurrentText(selected);}
    {QSignalBlocker blocked(hostsList_);hostsList_->clear();
        for(int i=0;i<machines_->count();++i){const auto name=machines_->itemText(i);int slotCount=0,busy=0,enabled=0;bool connected=false,maintenance=false;
            for(const auto& w:workers_)if(text(w,"machine_id")==name&&visible(w)){
                ++slotCount;busy+=occupiedSlots(w)>0;enabled+=dispatchEnabled(w);connected=connected||text(w,"status")=="ONLINE"||text(w,"status")=="DRAINING";
                const auto phase=text(w.value("maintenance",Json::object()),"state");maintenance=maintenance||phase=="PENDING"||phase=="RUNNING";}
            auto item=new QListWidgetItem(hostsList_);item->setData(Qt::UserRole,name);item->setSizeHint(QSize(180,90));
            const auto status=workersStale_&&!demo_?QStringLiteral("？ 数据过期"):
                enabled?QStringLiteral("<span style='color:#3b82f6'>●</span> 已上线"):
                !connected&&busy?QStringLiteral("<span style='color:#b58220'>●</span> 失联待处理"):
                busy||maintenance?QStringLiteral("<span style='color:#b58220'>●</span> 下线中 / 维护中"):
                connected?QStringLiteral("<span style='color:#94a3b8'>●</span> 已下线"):
                          QStringLiteral("<span style='color:#94a3b8'>●</span> 连接中断");
            auto label=new QLabel(name.toHtmlEscaped()+"<br><span style='color:#8493a8;font-size:11px'>"+status+QStringLiteral("<br>%1 slots · %2 已上线 · %3 执行/准备</span>").arg(slotCount).arg(enabled).arg(busy));label->setContentsMargins(14,8,8,8);label->setAttribute(Qt::WA_TransparentForMouseEvents);hostsList_->setItemWidget(item,label);
        }
        hostsList_->setCurrentRow(machines_->currentIndex());}
    slotsTable_->setRowCount(0);hostTitle_->setText(machines_->currentText().isEmpty()?QStringLiteral("暂无在线工作机"):machines_->currentText());
    bool anyOnline=false,anyRecoverable=false;int onlineCount=0,offlineCount=0,legacyCount=0,enabledCount=0,busyCount=0;
    for(const auto& w:workers_)if(text(w,"machine_id")==machines_->currentText()){
        if(text(w,"status")=="OFFLINE")++offlineCount;else ++onlineCount;
        enabledCount+=dispatchEnabled(w);busyCount+=occupiedSlots(w)>0;
        if(!w.value("control_supported",false)&&text(w,"status")!="OFFLINE")++legacyCount;
        if(!visible(w))continue;
        int row=slotsTable_->rowCount();slotsTable_->insertRow(row);cell(slotsTable_,row,0,text(w,"slot_id")+"\n"+text(w,"worker_id"));
        const auto status=workState(w,!demo_&&workersStale_);
        cell(slotsTable_,row,1,status);
        slotsTable_->item(row,1)->setForeground(QColor(status.startsWith("ONLINE")?"#3b82f6":status.startsWith("DRAINING")||status.startsWith("MAINTENANCE")?"#b58220":"#8795aa"));
        slotsTable_->item(row,1)->setToolTip(QStringLiteral("工作状态表示是否参与调度；右侧连接状态才表示实际通信连接。"));
        const bool online=text(w,"status")=="ONLINE";anyOnline=anyOnline||online;
        if(!demo_&&!w.value("control_supported",false))slotsTable_->item(row,0)->setToolTip(QStringLiteral("此进程未声明维护协议 V1，可能为旧版 Worker；槽位来自 Server 注册/心跳，不按磁盘目录推算。"));
        if(!w.value("registration_observed",true))slotsTable_->item(row,0)->setToolTip(QStringLiteral("Server 重启后保留的未确认执行。本进程尚未收到该 Worker 注册；暂按原 Worker ID 单独列出，不推测主机名。"));
        const bool accepting=w.value("accepting_tasks",true),requested=w.value("accepting_requested",accepting);
        const auto maintenance=w.value("maintenance",Json::object());
        const auto phase=QString::fromStdString(maintenance.value("state",std::string("IDLE")));
        const bool pending=phase=="PENDING"||phase=="RUNNING";
        cell(slotsTable_,row,2,QStringLiteral("连接：")+text(w,"status")+"\n"+
            (text(w,"status")=="OFFLINE"?QStringLiteral("调度：不可用 / 等待重连"):
             accepting?QStringLiteral("调度：已上线"):occupiedSlots(w)>0?QStringLiteral("调度：下线中 / 当前案例继续"):QStringLiteral("调度：已下线")));
        if(workersStale_&&!demo_)slotsTable_->item(row,2)->setText(QStringLiteral("上次快照 · ")+slotsTable_->item(row,2)->text());
        const auto maintenanceText=phase=="PENDING"?QStringLiteral("等待当前案例完成后更新"):phase=="RUNNING"?QStringLiteral("SVN 更新中"):phase=="FAILED"?QStringLiteral("更新失败 / 保持下线"):phase=="SUCCEEDED"?QStringLiteral("更新完成"):QString{};
        if(!maintenanceText.isEmpty())slotsTable_->item(row,2)->setText(slotsTable_->item(row,2)->text()+"\n"+maintenanceText);
        slotsTable_->item(row,2)->setToolTip(QString::fromStdString(maintenance.dump(2)));
        auto controls=new QWidget;auto actions=new QHBoxLayout(controls);actions->setContentsMargins(6,2,6,2);actions->setSpacing(8);
        const auto worker=text(w,"worker_id");
        auto toggle=new QPushButton(requested?QStringLiteral("下线"):QStringLiteral("上线"));toggle->setObjectName("slotAcceptButton");
        auto update=new QPushButton(QStringLiteral("更新"));update->setObjectName("slotUpdateButton");actions->addWidget(toggle);actions->addWidget(update);
        auto recover=new QPushButton(QStringLiteral("故障恢复…"));recover->setObjectName("slotRecoveryButton");actions->addWidget(recover);
        // Cell widgets are not reliably included by ResizeToContents on older Qt5.
        // Reserve both button widths explicitly; a narrow window scrolls instead of clipping.
        for(auto button:{toggle,update,recover})button->setMinimumWidth(qMax(112,button->sizeHint().width()));
        controls->setMinimumWidth(actions->minimumSize().width());
        slotsTable_->setColumnWidth(4,qMax(slotsTable_->columnWidth(4),controls->minimumWidth()+16));
        const bool enabled=(demo_||(!workersStale_&&operations_.contains("control_workers")))&&!managementPending_&&online;
        toggle->setToolTip(QStringLiteral("上线/下线只改变调度资格，不断开连接，也不终止正在运行的 testcase。"));
        update->setToolTip(QStringLiteral("排空当前任务后更新独立 SVN 副本；需要新版 Worker 支持。"));
        toggle->setEnabled(enabled&&(requested||!pending));
        update->setEnabled(enabled&&!pending&&(demo_||w.value("control_supported",false)));
        const bool recoverable=text(w,"status")=="OFFLINE"&&w.value("current_tasks_total",uint64_t(0))>0;
        anyRecoverable=anyRecoverable||recoverable;
        recover->setEnabled(!demo_&&!workersStale_&&!managementPending_&&operations_.contains("recover_offline_workers")&&recoverable);
        recover->setToolTip(QStringLiteral("仅处理失联 slot 的未完成任务；必须先确认原执行进程已停止。不是远程关机，也不会自动重新上线。"));
        connect(toggle,&QPushButton::clicked,this,[this,worker,requested]{controlWorkers(false,worker,requested?"pause":"resume");});
        connect(update,&QPushButton::clicked,this,[this,worker]{controlWorkers(false,worker,"update");});
        connect(recover,&QPushButton::clicked,this,[this,worker]{recoverOfflineWorkers(false,worker);});
        slotsTable_->setCellWidget(row,4,controls);
        QStringList execution;QString firstTask;
        if(w.contains("current_tasks")&&w["current_tasks"].is_array()){
            for(const auto& task:w["current_tasks"]){
                if(firstTask.isEmpty())firstTask=text(task,"task_id");
                auto phase=task.value("recovery_required",false)||text(w,"status")=="OFFLINE"?QStringLiteral("待确认"):
                    text(task,"status")=="RUNNING"?QStringLiteral("执行中"):QStringLiteral("待启动");
                auto path=text(task,"case");if(path.isEmpty()||path=="—")path=QStringLiteral("未提供 testcase 路径");
                execution<<phase+" · Task #"+text(task,"task_id")+"\n"+path;
            }
            if(w.value("current_tasks_truncated",false))execution<<QStringLiteral("另有活动任务，详见任务查询；本列最多显示 8 条。");
        }else execution<<QStringLiteral("Server 未提供当前任务明细，请升级 Server。");
        if(execution.isEmpty())execution<< (occupiedSlots(w)>0?QStringLiteral("任务状态待确认，请刷新。"):
            text(w,"status")=="OFFLINE"?QStringLiteral("连接中断，无可确认的运行案例"):
            accepting?QStringLiteral("等待任务"):QStringLiteral("当前无执行任务"));
        if(workersStale_&&!demo_)execution.prepend(QStringLiteral("上次任务快照，当前执行状态未确认"));
        cell(slotsTable_,row,3,execution.join("\n\n"));slotsTable_->item(row,3)->setToolTip(execution.join("\n\n"));
        if(!firstTask.isEmpty())slotsTable_->item(row,3)->setData(Qt::UserRole,firstTask);
        const auto details=execution.join("\n\n");const int lines=details.count('\n')+1;
        slotsTable_->setRowHeight(row,qBound(76,lines*slotsTable_->fontMetrics().lineSpacing()+20,180));
    }
    workerSummary_->setText((demo_?QStringLiteral("【演示数据】 "):workersStale_?QStringLiteral("【过期快照】 "):QStringLiteral("【Server 注册信息】 "))+
        QStringLiteral("%1：已连接/退出中 %2，连接中断 %3，已上线 %4，执行/准备 %5；%6 个已连接 slot 未声明维护协议。")
        .arg(machines_->currentText()).arg(onlineCount).arg(offlineCount).arg(enabledCount).arg(busyCount).arg(legacyCount));
    if(machines_->count()==0)workerSummary_->setText(workersStale_?QStringLiteral("尚未确认 Server 工作机信息，请检查连接。"):
        QStringLiteral("当前没有在线工作机。可勾选查看离线历史 slots；不会用演示数据填充。"));
    for(auto button:hostControlButtons_){button->setEnabled((demo_||(!workersStale_&&operations_.contains("control_workers")))&&anyOnline&&!managementPending_);button->setToolTip(QStringLiteral("整机操作仅针对当前已连接 slots；全部上线不远程启动失联机器。"));}
    hostRecoveryButton_->setEnabled(!demo_&&!workersStale_&&!managementPending_&&operations_.contains("recover_offline_workers")&&anyRecoverable);
    hostRecoveryButton_->setToolTip(QStringLiteral("只处理所选服务器中失联且有未完成任务的 slots，已连接 slots 不受影响。"));
}

void MainWindow::recoverOfflineWorkers(bool machine,const QString& target) {
    if(target.isEmpty()||managementPending_||workersStale_||!operations_.contains("recover_offline_workers"))return;
    Json expected=Json::array();QStringList details;
    for(const auto& w:workers_) {
        if(text(w,machine?"machine_id":"worker_id")!=target||text(w,"status")!="OFFLINE")continue;
        if(w.value("current_tasks_truncated",false)){
            note(QStringLiteral("当前任务明细被截断，不能安全确认故障恢复，请联系维护人员。"),true);return;}
        if(!w.contains("current_tasks")||!w["current_tasks"].is_array())continue;
        for(const auto& task:w["current_tasks"]) {
            expected.push_back({{"task_id",task["task_id"]},{"run_id",task["run_id"]},{"worker_id",w["worker_id"]}});
            details<<text(w,"worker_id")+" · Task #"+text(task,"task_id")+"\n"+text(task,"case")+"\nRun: "+text(task,"run_id");
        }
    }
    if(expected.empty()||expected.size()>256){note(QStringLiteral("没有可恢复的失联任务，或任务数超出单次安全上限（256）；请刷新。"),true);return;}
    QSignalBlocker blockRefresh(refreshTimer_),blockFreshness(freshnessTimer_);
    QDialog dialog(this);dialog.setObjectName("offlineRecoveryDialog");dialog.setWindowTitle(QStringLiteral("确认故障并重新排队"));dialog.resize(700,480);
    auto layout=new QVBoxLayout(&dialog);
    auto warning=new QLabel(QStringLiteral("目标：%1\n失联不代表执行已停止。断网时原机器上的 GalaxCore 可能仍在运行，重新排队可能造成重复执行。\n此操作不会关闭原进程！旧执行记录将保留，任务从头重跑；已取消批次不重排，超过重试上限的任务转为失败。故障 slot 保持下线。").arg(target));
    warning->setTextFormat(Qt::PlainText);warning->setWordWrap(true);layout->addWidget(warning);
    auto list=new QPlainTextEdit;list->setReadOnly(true);list->setPlainText(details.join("\n\n"));layout->addWidget(list,1);
    auto confirmed=new QCheckBox(QStringLiteral("我已确认原机器已关机，或上述任务的相关执行进程已经停止"));
    confirmed->setObjectName("confirmExecutionStopped");layout->addWidget(confirmed);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Cancel);auto apply=buttons->addButton(QStringLiteral("确认并重新排队"),QDialogButtonBox::AcceptRole);
    apply->setObjectName("confirmRecoveryButton");apply->setEnabled(false);apply->setAutoDefault(false);apply->setDefault(false);
    auto cancel=buttons->button(QDialogButtonBox::Cancel);cancel->setDefault(true);cancel->setFocus();layout->addWidget(buttons);
    connect(confirmed,&QCheckBox::toggled,apply,&QPushButton::setEnabled);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted||!confirmed->isChecked())return;
    Json request={{machine?"machine_id":"worker_id",target.toStdString()},{"confirm_stopped",true},{"tasks",expected}};
    managementPending_=true;showWorkers();
    rpc_.request("recover_offline_workers",request,[this](RpcReply r){
        managementPending_=false;
        if(!r.ok){showWorkers();note(QStringLiteral("故障恢复未确认：")+r.error+QStringLiteral("。请刷新核对；不要重复确认正在运行的新执行。"),true);refresh();return;}
        const auto result=r.result;
        int queued=0,cancelled=0,failed=0;
        for(const auto& item:result.value("items",Json::array())){
            const auto action=text(item,"action");queued+=action=="requeued";cancelled+=action=="cancelled";failed+=action=="failed";}
        const bool complete=result.value("complete",false);
        note(QStringLiteral("%1重新排队 %2，取消 %3，重试用尽转失败 %4。故障 slot 保持下线；旧执行记录已保留。")
            .arg(complete?QString():QStringLiteral("部分完成，请刷新核对！ ")).arg(queued).arg(cancelled).arg(failed)+
            (complete?QString():" "+text(result,"error")),!complete);
        refresh();
    });
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
    const auto explanation=action=="pause"?QStringLiteral("下线后不再派发新任务，已分配 / 运行中的 testcase 正常结束；通信连接保留。"):
        action=="resume"?QStringLiteral("仅将当前已连接、未退出的 slots 上线；失联机器不会被远程启动。维护失败后请先检查工作副本。"):
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
            QStringLiteral("上线/下线状态已保存，当前 testcase 不受影响。"));
        refresh();
    });
}
