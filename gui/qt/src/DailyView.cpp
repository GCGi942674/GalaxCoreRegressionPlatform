#include "ConsoleSupport.h"

void MainWindow::showDailyPlan() {
    if(demo_){note(QStringLiteral("演示模式没有真实 Clock 计划；请连接 Server 查看。"));return;}
    if(!operations_.contains("query_daily_plan")){note(QStringLiteral("Server 尚不支持每日计划，请先升级。"),true);return;}
    auto dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("dailyPlanDialog");
    dialog->setWindowTitle(QStringLiteral("每日回归完整性"));dialog->resize(1180,620);auto box=new QVBoxLayout(dialog);
    auto message=new QLabel;message->setWordWrap(true);message->setTextFormat(Qt::PlainText);box->addWidget(message);
    auto rows=table({"Tasks.yaml 任务","模板 / 版本","计划 / 已提交 / 未提交","排队或运行 / 已结束 / 取消","日志已校验 / 已过期","状态"});
    box->addWidget(rows,1);auto buttons=new QHBoxLayout;auto refresh=new QPushButton(QStringLiteral("刷新 / 首页"));
    auto next=new QPushButton(QStringLiteral("下一页"));next->setEnabled(false);buttons->addWidget(refresh);buttons->addWidget(next);box->addLayout(buttons);
    auto offset=std::make_shared<uint64_t>(0);const auto day=dateB_->date().toString("yyyy-MM-dd").toStdString();
    QPointer<QDialog> guard(dialog);
    auto load=[this,guard,day,rows,message,refresh,next,offset](bool first){
        if(first)*offset=0;refresh->setEnabled(false);next->setEnabled(false);
        rpc_.request("query_daily_plan",{{"date",day},{"offset",*offset},{"limit",50}},
            [this,guard,rows,message,refresh,next,offset,day](RpcReply r){
                if(!guard)return;refresh->setEnabled(true);
                if(!r.ok){message->setText(QStringLiteral("查询失败：")+r.error);return;}
                try{
                    rows->setRowCount(0);
                    if(!r.result.at("plan_available").get<bool>()){
                        message->setText(QStringLiteral("%1 没有发布计划。可能 Clock 尚未运行，或是旧版本历史；不能据此判断今日全部完成。").arg(QString::fromStdString(day)));return;
                    }
                    const QMap<QString,QString> labels{{"NOT_SUBMITTED",QStringLiteral("未提交")},{"PARTIALLY_SUBMITTED",QStringLiteral("部分提交 / 请检查 Clock")},
                        {"RUNNING",QStringLiteral("执行中")},{"COMPLETE",QStringLiteral("执行完成")},{"COUNT_MISMATCH",QStringLiteral("数量异常")},
                        {"STOPPED",QStringLiteral("已停止后续")},{"STOPPING",QStringLiteral("停止中 · 等待在途完成")},{"DELETED",QStringLiteral("任务记录已删除")},{"PARTIALLY_DELETED",QStringLiteral("部分案例记录已删除")}};
                    for(const auto& item:r.result.at("items")){
                        const int row=rows->rowCount();rows->insertRow(row);
                        cell(rows,row,0,text(item,"name"));rows->item(row,0)->setToolTip(text(item,"batch_id"));
                        cell(rows,row,1,text(item,"template")+" / "+text(item,"revision"));
                        cell(rows,row,2,text(item,"planned")+" / "+text(item,"submitted")+" / "+text(item,"not_submitted"));
                        cell(rows,row,3,text(item,"active")+" / "+text(item,"finished")+" / "+text(item,"cancelled"));
                        cell(rows,row,4,text(item,"logs_verified")+" / "+text(item,"logs_expired"));cell(rows,row,5,labels.value(text(item,"state"),text(item,"state")));
                    }
                    message->setText(QStringLiteral("%1 · %2 个计划批次，本页 %3 个。\n“执行完成”要求计划数量与实际提交数量一致且无在途案例；日志校验是另一条独立状态。未提交项保留，即使尚无 testcase 记录。\n日志统计只计入当前 run 的完整文件集校验；旧 Worker 无清单时不推断。分页为实时数据，请刷新确认。")
                        .arg(QString::fromStdString(day)).arg(r.result.at("total").get<uint64_t>()).arg(rows->rowCount()));
                    next->setEnabled(r.result.value("has_more",false));if(next->isEnabled())*offset=r.result.at("next_offset").get<uint64_t>();
                }catch(const std::exception& e){message->setText(QString::fromUtf8(e.what()));}
            });
    };
    connect(refresh,&QPushButton::clicked,dialog,[load]{load(true);});connect(next,&QPushButton::clicked,dialog,[load]{load(false);});
    dialog->show();load(true);
}

void MainWindow::queryDaily(bool next) {
    if(regressionLoading_||loading_||submitting_){note(QStringLiteral("正在执行其他请求，请稍后再查询"));return;}
    if(!operations_.contains("query_regression")){note(QStringLiteral("Server 未声明回归查询能力"),true);return;}
    Json request={{"baseline_date",dateA_->date().toString("yyyy-MM-dd").toStdString()},
        {"date",dateB_->date().toString("yyyy-MM-dd").toStdString()},{"limit",50}};
    const auto stage=regressionStage_->currentData().toString(),change=regressionChange_->currentData().toString();
    if(!stage.isEmpty())request["template"]=stage.toStdString();if(!change.isEmpty())request["change"]=change.toStdString();
    if(!regressionSearch_->text().trimmed().isEmpty())request["search"]=regressionSearch_->text().trimmed().toStdString();
    const auto signature=request.dump();
    if(next&&signature!=regressionFilter_){regressionSummary_->setText(QStringLiteral("筛选已改变，请点击比较 / 首页重新查询。"));return;}
    if(!next)regressionFilter_=signature;
    if(next&&!regressionCursor_.isEmpty())request["after_key"]=regressionCursor_.toStdString();
    regressionLoading_=true;regressionNext_->setEnabled(false);
    dateA_->setEnabled(false);dateB_->setEnabled(false);regressionStage_->setEnabled(false);
    regressionChange_->setEnabled(false);regressionSearch_->setEnabled(false);
    rpc_.request("query_regression",request,[this,next,stage](RpcReply reply){
        regressionLoading_=false;
        dateA_->setEnabled(true);dateB_->setEnabled(true);regressionStage_->setEnabled(true);
        regressionChange_->setEnabled(true);regressionSearch_->setEnabled(true);
        if(!reply.ok){regressionSummary_->setText(QStringLiteral("查询失败，原回归数据已过期：")+reply.error);note(reply.error,true);return;}
        try {
            const auto generation=reply.result.at("snapshot_generation").get<uint64_t>();
            if(next&&(generation!=regressionGeneration_||reply.instance!=regressionInstance_)){
                regressionSummary_->setText(QStringLiteral("数据已变化，请点击比较 / 首页重新查询，避免跨页混用。"));return;}
            regressionGeneration_=generation;regressionInstance_=reply.instance;
            regressionRows_=reply.result.at("items");regressionTable_->setRowCount(0);
            QSignalBlocker block(regressionStage_);regressionStage_->clear();regressionStage_->addItem(QStringLiteral("全部阶段"),"");
            for(const auto& name:reply.result.at("templates")){auto s=QString::fromStdString(name.get<std::string>());regressionStage_->addItem(s,s);}
            const auto index=regressionStage_->findData(stage);if(index>=0)regressionStage_->setCurrentIndex(index);
            for(const auto& item:regressionRows_){int row=regressionTable_->rowCount();regressionTable_->insertRow(row);
                cell(regressionTable_,row,0,text(item,"template"));cell(regressionTable_,row,1,text(item,"case"));
                cell(regressionTable_,row,2,item["baseline"].is_null()?QStringLiteral("缺少结果"):text(item["baseline"],"outcome"));
                cell(regressionTable_,row,3,item["comparison"].is_null()?QStringLiteral("缺少结果"):text(item["comparison"],"outcome"));
                const QMap<QString,QString> changes{{"REGRESSION",QStringLiteral("新回退")},{"IMPROVEMENT",QStringLiteral("新通过")},
                    {"MISSING",QStringLiteral("缺少结果")},{"EXCEPTION",QStringLiteral("异常 / 未完成")},{"UNCHANGED",QStringLiteral("无确认回退")}};
                cell(regressionTable_,row,4,changes.value(text(item,"change")));
                auto controls=new QWidget;auto actions=new QHBoxLayout(controls);actions->setContentsMargins(2,2,2,2);
                for(const auto& side:QList<QPair<QString,QString>>{{QStringLiteral("基准 run"),"baseline"},{QStringLiteral("对比 run"),"comparison"}}){auto button=new QPushButton(side.first);actions->addWidget(button);const auto task=item[side.second.toStdString()];button->setEnabled(!task.is_null());
                    connect(button,&QPushButton::clicked,this,[this,task]{rpc_.request("query_task",{{"task_id",task.at("task_id")}},[this](RpcReply reply){if(reply.ok)openLogs(reply.result);else note(reply.error,true);});});}
                regressionTable_->setCellWidget(row,5,controls);
            }
            regressionCursor_=reply.result["next_after_key"].is_null()?QString{}:QString::fromStdString(reply.result["next_after_key"].get<std::string>());
            regressionNext_->setEnabled(reply.result.at("has_more").get<bool>());
            const auto& counts=reply.result.at("counts");
            QString summary=QStringLiteral("匹配 %1 条 · 本页 %2 条 · 新回退 %3 · 新通过 %4 · 缺少 %5 · 异常 %6")
                .arg(reply.result["total"].get<uint64_t>()).arg(regressionRows_.size())
                .arg(counts["REGRESSION"].get<uint64_t>()).arg(counts["IMPROVEMENT"].get<uint64_t>())
                .arg(counts["MISSING"].get<uint64_t>()).arg(counts["EXCEPTION"].get<uint64_t>());
            for(auto it=reply.result["stages"].begin();it!=reply.result["stages"].end();++it){const auto& stats=it.value();
                summary+=QStringLiteral("\n%1：共 %2 · 比较日 PASS %3 / FAIL %4 / TIMEOUT %5 · 新回退 %6 · 新通过 %7")
                    .arg(QString::fromStdString(it.key())).arg(stats["total"].get<uint64_t>())
                    .arg(stats["comparison"].value("PASS",uint64_t(0))).arg(stats["comparison"].value("FAIL",uint64_t(0)))
                    .arg(stats["comparison"].value("TIMEOUT",uint64_t(0))).arg(stats["changes"]["REGRESSION"].get<uint64_t>())
                    .arg(stats["changes"]["IMPROVEMENT"].get<uint64_t>());}
            summary+=QStringLiteral("\n同日重复记录 %1（取最新 task_id）；阶段汇总不受搜索 / 变化筛选影响。")
                .arg(reply.result["duplicate_records"].get<uint64_t>());regressionSummary_->setText(summary);
        }catch(const std::exception& error){regressionSummary_->setText(QStringLiteral("回归响应无效，请更新 Server：")+QString::fromUtf8(error.what()));}
    });
}
