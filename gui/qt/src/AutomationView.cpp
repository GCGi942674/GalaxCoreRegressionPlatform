#include "ConsoleSupport.h"
#include <QTimeEdit>
#include <optional>

namespace {
QString weekdaysText(const Json& days){
    const QStringList names{QStringLiteral("一"),QStringLiteral("二"),QStringLiteral("三"),QStringLiteral("四"),QStringLiteral("五"),QStringLiteral("六"),QStringLiteral("日")};
    QStringList result;for(const auto& d:days)if(d.is_number_integer()&&d>=1&&d<=7)result<<names[d.get<int>()-1];
    return result.join(" ");
}
QString runState(const Json& run){
    const auto state=run.value("state",std::string());
    if(state=="SUBMITTED")return QStringLiteral("已入队 %1").arg(run.value("planned",0));
    if(state=="SUBMITTING")return QStringLiteral("提交中 %1/%2").arg(run.value("next_index",0)).arg(run.value("planned",0));
    if(state=="FAILED")return QStringLiteral("触发失败");
    if(state=="STOPPED")return QStringLiteral("批次已取消");
    return QStringLiteral("尚未触发");
}
std::optional<Json> editPlan(QWidget* parent,Json original,const Json& catalog){
    auto dialog=new QDialog(parent);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("automationEditor");
    dialog->setWindowTitle(QStringLiteral("编辑每周任务计划"));dialog->resize(640,650);
    auto box=new QVBoxLayout(dialog);auto explanation=new QLabel(QStringLiteral("Server 按本地时间触发，每个计划每天最多一次。目录相对于 Server 的 test2；不会纳入每日回归对比。"));
    explanation->setWordWrap(true);box->addWidget(explanation);
    auto form=new QFormLayout;box->addLayout(form);
    auto name=new QLineEdit(text(original,"name"));name->setObjectName("planName");form->addRow(QStringLiteral("任务名称"),name);
    auto enabled=new QCheckBox(QStringLiteral("启用此计划"));enabled->setObjectName("planEnabled");enabled->setChecked(original.value("enabled",false));form->addRow(enabled);
    auto days=new QWidget;auto dayRow=new QHBoxLayout(days);dayRow->setContentsMargins(0,0,0,0);
    QList<QCheckBox*> checks;const QStringList labels{QStringLiteral("一"),QStringLiteral("二"),QStringLiteral("三"),QStringLiteral("四"),QStringLiteral("五"),QStringLiteral("六"),QStringLiteral("日")};
    for(int i=1;i<=7;++i){auto check=new QCheckBox(labels[i-1]);check->setObjectName("planDay"+QString::number(i));check->setChecked(std::find(original["weekdays"].begin(),original["weekdays"].end(),Json(i))!=original["weekdays"].end());checks<<check;dayRow->addWidget(check);}form->addRow(QStringLiteral("星期"),days);
    auto time=new QTimeEdit(QTime::fromString(text(original,"time"),"HH:mm"));time->setObjectName("planTime");time->setDisplayFormat("HH:mm");form->addRow(QStringLiteral("触发时间"),time);
    auto stage=new QComboBox;stage->setObjectName("planTemplate");
    if(catalog.is_object()&&catalog.contains("templates")&&catalog["templates"].is_object())
        for(auto it=catalog["templates"].begin();it!=catalog["templates"].end();++it)stage->addItem(QString::fromStdString(it.key()));
    const auto oldStage=text(original,"template");if(stage->findText(oldStage)<0&&!oldStage.isEmpty()&&oldStage!=QStringLiteral("—"))stage->addItem(oldStage);
    stage->setCurrentText(oldStage);form->addRow(QStringLiteral("Server 模板"),stage);
    auto scope=new QLineEdit(text(original,"scope"));scope->setObjectName("planScope");scope->setPlaceholderText(QStringLiteral(". 表示全部；例如 kintexuplus/xcku3p-ffvd900-1-i"));form->addRow(QStringLiteral("testcase 目录范围"),scope);
    auto version=new QComboBox;version->setObjectName("planRevision");version->setEditable(true);version->addItem("latest");
    if(catalog.is_object())for(const auto& revision:catalog.value("revisions",Json::array()))if(revision.is_string())version->addItem(QString::fromStdString(revision.get<std::string>()));
    version->setCurrentText(text(original,"revision"));form->addRow(QStringLiteral("ZIP 版本 / latest"),version);
    auto priority=new QSpinBox;priority->setObjectName("planPriority");priority->setRange(-100,100);priority->setValue(original.value("priority",0));form->addRow(QStringLiteral("优先级"),priority);
    auto timeout=new QSpinBox;timeout->setObjectName("planTimeout");timeout->setRange(1,86400);timeout->setValue(original.value("timeout_seconds",5400));form->addRow(QStringLiteral("单例超时（秒）"),timeout);
    auto detail=new QLabel(QStringLiteral("latest 在每轮触发时固定为最新可用 ZIP。模板配置也会保存为本轮快照。未运行的批次可在任务管理中取消；关闭计划只暂停后续提交，不强杀运行中的 testcase。停机错过超过 5 分钟的触发点不会自动补跑。"));detail->setWordWrap(true);box->addWidget(detail);box->addStretch();
    auto error=new QLabel;error->setWordWrap(true);error->setStyleSheet("color:#ad3442");box->addWidget(error);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);box->addWidget(buttons);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    std::optional<Json> result;
    QObject::connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,dialog,[&]{
        auto selected=Json::array();for(int i=0;i<checks.size();++i)if(checks[i]->isChecked())selected.push_back(i+1);
        const auto path=scope->text().trimmed();
        if(name->text().trimmed().isEmpty()||selected.empty()||stage->currentText().isEmpty()||path.isEmpty()){
            error->setText(QStringLiteral("请填写名称、星期、模板及目录范围。"));return;
        }
        auto plan=original;plan["name"]=name->text().trimmed().toStdString();plan["enabled"]=enabled->isChecked();plan["weekdays"]=selected;
        plan["time"]=time->time().toString("HH:mm").toStdString();plan["template"]=stage->currentText().toStdString();plan["scope"]=path.toStdString();
        plan["revision"]=version->currentText().trimmed().toStdString();plan["priority"]=priority->value();plan["timeout_seconds"]=timeout->value();
        result=plan;dialog->accept();
    });
    dialog->exec();return result;
}
}
void MainWindow::openAutomation(){
    if(!operations_.contains("query_automation")){note(QStringLiteral("Server 尚不支持定时计划，请更新 Server。"),true);return;}
    rpc_.request("query_automation",Json::object(),[this](RpcReply reply){
        if(!reply.ok){note(reply.error,true);return;}
        auto dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("automationDialog");
        dialog->setWindowTitle(QStringLiteral("定时计划与每日回归开关"));dialog->resize(1120,680);
        auto layout=new QVBoxLayout(dialog);
        auto daily=new QCheckBox(QStringLiteral("开启每日回归（原有 crontab / Tasks.yaml）"));daily->setObjectName("dailyEnabled");daily->setChecked(reply.result.value("daily_enabled",true));layout->addWidget(daily);
        auto info=new QLabel(QStringLiteral("关闭仅阻止新一轮每日回归，不取消已提交或已开始提交的批次。每周计划独立运行，结果不混入每日回归。\nServer 时区：%1；编辑后点击“保存设置”生效。多人同时修改时会拒绝覆盖，请重新载入。").arg(text(reply.result,"server_timezone")));info->setWordWrap(true);layout->addWidget(info);
        auto rows=std::make_shared<Json>(reply.result.value("plans",Json::array()));
        auto saved=std::make_shared<Json>(reply.result);
        auto list=table({QStringLiteral("名称"),QStringLiteral("启用"),QStringLiteral("星期"),QStringLiteral("时间"),QStringLiteral("模板"),QStringLiteral("目录范围"),QStringLiteral("版本"),QStringLiteral("最近提交")});
        list->setObjectName("automationTable");list->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);layout->addWidget(list,1);
        list->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Interactive);list->setColumnWidth(0,150);
        list->horizontalHeader()->setSectionResizeMode(5,QHeaderView::Stretch);
        list->horizontalHeader()->setSectionResizeMode(7,QHeaderView::Interactive);list->setColumnWidth(7,190);
        auto paint=[rows,saved,list]{
            const int selected=list->currentRow();
            list->setRowCount(int(rows->size()));
            for(size_t i=0;i<rows->size();++i){
                const auto& p=(*rows)[i];Json latest;
                for(const auto& run:saved->value("runs",Json::array()))if(run["plan_id"]==p["id"])latest=run;
                const QStringList values{text(p,"name"),p.value("enabled",false)?QStringLiteral("是"):QStringLiteral("否"),weekdaysText(p["weekdays"]),text(p,"time"),text(p,"template"),text(p,"scope"),text(p,"revision"),latest.is_null()?QStringLiteral("尚未触发"):text(latest,"date")+" "+runState(latest)};
                for(int c=0;c<values.size();++c){auto item=new QTableWidgetItem(values[c]);item->setToolTip(values[c]);if(!latest.is_null()&&!latest.value("error",std::string()).empty())item->setToolTip(text(latest,"error"));list->setItem(int(i),c,item);}
            }
            if(selected>=0&&selected<list->rowCount())list->selectRow(selected);
        };paint();
        auto toolbar=new QHBoxLayout;auto add=new QPushButton(QStringLiteral("新增计划"));add->setObjectName("addPlan");
        auto edit=new QPushButton(QStringLiteral("编辑所选"));edit->setObjectName("editPlan");
        auto remove=new QPushButton(QStringLiteral("删除计划"));remove->setObjectName("deletePlan");
        auto toggle=new QPushButton(QStringLiteral("启用 / 暂停"));toggle->setObjectName("togglePlan");
        auto reload=new QPushButton(QStringLiteral("重新载入"));toolbar->addWidget(add);toolbar->addWidget(edit);toolbar->addWidget(toggle);toolbar->addWidget(remove);toolbar->addStretch();toolbar->addWidget(reload);layout->addLayout(toolbar);
        auto status=new QLabel;status->setObjectName("automationStatus");status->setWordWrap(true);layout->addWidget(status);
        if(!reply.result.value("runner_error",std::string()).empty())status->setText(text(reply.result,"runner_error"));
        auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Close);buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存设置"));layout->addWidget(buttons);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        QPointer<QDialog> guard(dialog);
        auto polling=std::make_shared<bool>(false);auto poll=new QTimer(dialog);poll->setInterval(5000);
        connect(poll,&QTimer::timeout,dialog,[this,guard,saved,status,paint,polling]{
            if(*polling)return;*polling=true;
            rpc_.request("query_automation",Json::object(),[guard,saved,status,paint,polling](RpcReply result){
                *polling=false;if(!guard)return;
                if(!result.ok){status->setText(QStringLiteral("计划状态未更新：")+result.error);return;}
                (*saved)["runs"]=result.result.value("runs",Json::array());paint();
                if(result.result["revision"]!=saved->at("revision"))status->setText(QStringLiteral("其他用户已修改设置，请重新载入后再保存。"));
                else if(!result.result.value("runner_error",std::string()).empty())status->setText(text(result.result,"runner_error"));
            });
        });poll->start();
        connect(add,&QPushButton::clicked,dialog,[this,dialog,rows,paint]{
            if(rows->size()>=32)return;
            Json p={{"id",QUuid::createUuid().toString().mid(1,36).toStdString()},{"name",QStringLiteral("白天验证").toStdString()},{"enabled",true},{"weekdays",{1,2,3,4,5}},{"time","10:00"},{"template",""},{"scope","."},{"revision","latest"},{"priority",0},{"timeout_seconds",5400}};
            auto result=editPlan(dialog,p,catalog_);if(result){rows->push_back(*result);paint();}
        });
        connect(edit,&QPushButton::clicked,dialog,[this,dialog,rows,list,paint]{const int i=list->currentRow();if(i<0)return;auto result=editPlan(dialog,(*rows)[i],catalog_);if(result){(*rows)[i]=*result;paint();}});
        connect(toggle,&QPushButton::clicked,dialog,[rows,list,paint]{const int i=list->currentRow();if(i>=0){(*rows)[i]["enabled"]=!(*rows)[i].value("enabled",true);paint();}});
        connect(remove,&QPushButton::clicked,dialog,[dialog,rows,list,paint]{const int i=list->currentRow();if(i>=0&&QMessageBox::question(dialog,QStringLiteral("删除计划"),QStringLiteral("仅删除定时规则，不删除已经提交的任务或结果。确定？"))==QMessageBox::Yes){rows->erase(rows->begin()+i);paint();}});
        connect(reload,&QPushButton::clicked,dialog,[this,guard]{if(guard){guard->close();openAutomation();}});
        connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::close);
        connect(buttons,&QDialogButtonBox::accepted,dialog,[this,guard,rows,saved,daily,buttons,status,paint]{
            buttons->button(QDialogButtonBox::Save)->setEnabled(false);
            Json data={{"expected_revision",saved->at("revision")},{"daily_enabled",daily->isChecked()},{"plans",*rows}};
            rpc_.request("update_automation",data,[guard,saved,buttons,status,paint](RpcReply result){
                if(!guard)return;buttons->button(QDialogButtonBox::Save)->setEnabled(true);
                if(!result.ok){status->setText(QStringLiteral("保存未确认：")+result.error+QStringLiteral("；请重新载入核对。"));return;}
                *saved=result.result;paint();status->setText(QStringLiteral("已保存；不影响正在运行的 testcase。"));
            });
        });
        dialog->show();
    });
}
