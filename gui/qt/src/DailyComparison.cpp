#include "ConsoleSupport.h"

namespace {
QWidget* dailyBadge(const QString& value,const QString& tone) {
    auto holder=new QWidget;holder->setObjectName("dailyCell");auto row=new QHBoxLayout(holder);row->setContentsMargins(8,4,8,4);
    auto label=new QLabel(value);label->setTextFormat(Qt::PlainText);label->setObjectName("dailyBadge");label->setProperty("tone",tone);
    label->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);row->addWidget(label);row->addStretch();return holder;
}
QString outcomeTone(const QString& value) {
    return value=="PASS"?"green":value=="FAIL"?"red":value=="TIMEOUT"||value=="ACTIVE"||value=="ERROR"||value=="CRASH"?"amber":"neutral";
}
QString dailyChangeName(const QString& key) {
    const QMap<QString,QString> names{{"REGRESSION",QStringLiteral("新回退")},{"IMPROVEMENT",QStringLiteral("新通过")},
        {"STABLE_PASS",QStringLiteral("持续通过")},{"STABLE_FAIL",QStringLiteral("持续失败")},{"OTHER",QStringLiteral("未完成 / 其他")}};
    return names.value(key,QStringLiteral("未完成 / 其他"));
}
}

void MainWindow::scheduleDailyQuery() {
    // Invalidate outstanding callbacks immediately, not after the debounce expires.
    ++regressionRequestEpoch_;regressionLoading_=false;regressionCursor_.clear();regressionNext_->setEnabled(false);
    regressionRows_=Json::array();regressionTable_->setRowCount(0);
    for(auto button:regressionCards_)button->findChild<QLabel*>("dailyMetricValue")->setText(QStringLiteral("—"));
    regressionSync_->setText(QStringLiteral("等待自动更新…"));regressionSummary_->setText(QStringLiteral("筛选已改变，正在获取对应结果。"));
    regressionDebounce_.start();
}

void MainWindow::selectDailyChange(const QString& change) {
    const auto current=regressionChange_->currentData().toString();const auto chosen=current==change?QString():change;
    const auto index=regressionChange_->findData(chosen);if(index<0)return;
    regressionChange_->setCurrentIndex(index);
    for(auto it=regressionCards_.begin();it!=regressionCards_.end();++it)it.value()->setChecked(it.key()==chosen);
    for(auto it=regressionPills_.begin();it!=regressionPills_.end();++it)it.value()->setChecked(it.key()==chosen);
    scheduleDailyQuery();
}

void MainWindow::queryDaily(bool next) {
    regressionDebounce_.stop();
    if(next&&regressionLoading_)return;
    if(!operations_.contains("query_regression")){
        regressionSync_->setText(QStringLiteral("未连接 / 接口不支持"));return;
    }
    const auto dayA=dateA_->date().toString("yyyy-MM-dd"),dayB=dateB_->date().toString("yyyy-MM-dd");
    Json request={{"baseline_date",dayA.toStdString()},{"date",dayB.toStdString()},{"limit",50}};
    const auto stage=regressionStage_->currentData().toString(),change=regressionChange_->currentData().toString();
    if(!stage.isEmpty())request["template"]=stage.toStdString();if(!change.isEmpty())request["change"]=change.toStdString();
    const auto search=regressionSearch_->text().trimmed();if(!search.isEmpty())request["search"]=search.toStdString();
    const auto signature=request.dump();
    if(next&&(signature!=regressionFilter_||regressionCursor_.isEmpty())){scheduleDailyQuery();return;}
    if(!next){regressionFilter_=signature;regressionCursor_.clear();regressionRows_=Json::array();regressionTable_->setRowCount(0);}
    else request["after_key"]=regressionCursor_.toStdString();
    const auto epoch=++regressionRequestEpoch_;regressionLoading_=true;regressionNext_->setEnabled(false);
    regressionSync_->setText(QStringLiteral("正在更新对比…"));
    rpc_.request("query_regression",request,[this,next,stage,epoch,dayA,dayB](RpcReply reply){
        if(epoch!=regressionRequestEpoch_)return;
        regressionLoading_=false;
        if(!reply.ok){
            regressionSync_->setText(QStringLiteral("查询失败 · 请重试"));regressionSummary_->setText(QStringLiteral("数据未确认：")+reply.error);
            regressionRows_=Json::array();regressionTable_->setRowCount(0);return;
        }
        try {
            const auto generation=reply.result.at("snapshot_generation").get<uint64_t>();
            if(next&&(generation!=regressionGeneration_||reply.instance!=regressionInstance_)){
                scheduleDailyQuery();regressionSync_->setText(QStringLiteral("数据已变化 · 自动回到首页"));return;
            }
            const auto& counts=reply.result.at("summary_counts");
            if(!counts.is_object())throw std::runtime_error("Server lacks complete daily summary; upgrade Server with GUI");
            for(const auto key:{"ALL","REGRESSION","IMPROVEMENT","STABLE_PASS","STABLE_FAIL","OTHER"})counts.at(key).get<uint64_t>();
            regressionGeneration_=generation;regressionInstance_=reply.instance;
            regressionRows_=reply.result.at("items");if(!regressionRows_.is_array())throw std::runtime_error("daily items must be an array");
            // The server supplies all available templates for these dates, not just
            // the current page. Never turn a missing stage into a silent ALL query.
            {QSignalBlocker block(regressionStage_);regressionStage_->clear();regressionStage_->addItem(QStringLiteral("全部阶段"),"");
                for(const auto& name:reply.result.at("templates")){auto value=QString::fromStdString(name.get<std::string>());regressionStage_->addItem(value,value);}
                if(!stage.isEmpty()&&regressionStage_->findData(stage)<0)regressionStage_->addItem(stage+QStringLiteral(" · 暂无结果"),stage);
                regressionStage_->setCurrentIndex(qMax(0,regressionStage_->findData(stage)));}
            for(auto it=regressionCards_.begin();it!=regressionCards_.end();++it)
                it.value()->findChild<QLabel*>("dailyMetricValue")->setText(QString::number(counts.at(it.key().toStdString()).get<uint64_t>()));
            for(auto it=regressionPills_.begin();it!=regressionPills_.end();++it){
                const auto key=it.key().isEmpty()?"ALL":it.key().toStdString();const auto name=it.key().isEmpty()?QStringLiteral("全部"):dailyChangeName(it.key());
                it.value()->setText(name+" "+QString::number(counts.at(key).get<uint64_t>()));
            }
            regressionTitle_->setText((stage.isEmpty()?QStringLiteral("全部阶段"):stage)+QStringLiteral(" · 每日回归对比"));
            regressionTable_->setHorizontalHeaderLabels({QStringLiteral("testcase"),dayA,dayB,QStringLiteral("变化"),QStringLiteral("日志对比")});
            regressionTable_->setRowCount(0);
            for(const auto& item:regressionRows_){
                int row=regressionTable_->rowCount();regressionTable_->insertRow(row);const auto path=text(item,"case");
                auto parts=path.split('/');if(parts.size()>1)parts.removeLast();const auto name=parts.isEmpty()?path:parts.last();
                const auto device=parts.size()>1?parts[parts.size()-2]:QString();cell(regressionTable_,row,0,name+"\n"+device);
                regressionTable_->item(row,0)->setForeground(QColor("#2459c8"));regressionTable_->item(row,0)->setToolTip(path+"\n"+QStringLiteral("模板：")+text(item,"template"));
                for(int side=0;side<2;++side){const auto& task=item[side==0?"baseline":"comparison"];
                    const auto outcome=task.is_null()?QStringLiteral("缺少结果"):text(task,"outcome");
                    regressionTable_->setCellWidget(row,side+1,dailyBadge(outcome,outcomeTone(outcome)));
                }
                const auto change=text(item,"display_change");
                regressionTable_->setCellWidget(row,3,dailyBadge(dailyChangeName(change),change=="REGRESSION"?"red":change=="IMPROVEMENT"?"green":change=="OTHER"?"amber":"neutral"));
                auto link=new QPushButton(QStringLiteral("两天日志 ↗"));link->setObjectName("dailyLogLink");link->setToolTip(QStringLiteral("仅比较两天的原始 run 与 flow_config"));
                connect(link,&QPushButton::clicked,this,[this,item,dayA,dayB]{openDailyEvidence(item,dayA,dayB);});regressionTable_->setCellWidget(row,4,link);
            }
            regressionCursor_=reply.result["next_after_key"].is_null()?QString():QString::fromStdString(reply.result["next_after_key"].get<std::string>());
            regressionNext_->setEnabled(reply.result.at("has_more").get<bool>());
            regressionSync_->setText(QStringLiteral("● 已自动更新"));
            regressionSummary_->setText(QStringLiteral("匹配 %1 条 · 本页 %2 条 · 汇总覆盖当前阶段和搜索的全部结果，不只统计当前页。\nTIMEOUT / CRASH / 未完成 / 缺失结果单独归入“未完成 / 其他”；同日同阶段同案例取最新记录，重复记录 %3 条。")
                .arg(reply.result.at("total").get<uint64_t>()).arg(regressionRows_.size()).arg(reply.result.value("duplicate_records",uint64_t(0))));
        }catch(const std::exception& e){
            regressionRows_=Json::array();regressionTable_->setRowCount(0);regressionNext_->setEnabled(false);
            for(auto button:regressionCards_)button->findChild<QLabel*>("dailyMetricValue")->setText(QStringLiteral("—"));
            regressionSync_->setText(QStringLiteral("响应不兼容 / 无效"));regressionSummary_->setText(QStringLiteral("请一起更新 Server 与 GUI：")+QString::fromUtf8(e.what()));
        }
    });
}
