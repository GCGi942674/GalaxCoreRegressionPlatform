#include "ConsoleSupport.h"
#include <cmath>
#include <optional>

namespace {
std::optional<double> number(const Json& j,const char* key){
    if(!j.is_object()||!j.contains(key)||!j[key].is_number())return {};
    const auto n=j[key].get<double>();if(!std::isfinite(n)||n<0)return {};return n;
}
QString bytes(double n){
    const bool large=n>=1099511627776.0;
    return QString::number(n/(large?1099511627776.0:1073741824.0),'f',1)+(large?" TiB":" GiB");
}
QString rate(const Json& j,const char* key){
    const auto n=number(j,key);return n?QString::number(*n/1048576.0,'f',2)+" MiB/s":QStringLiteral("未提供");
}
QString psi(const Json& j,const char* key){
    const auto n=number(j,key);return n?QString::number(*n,'f',1)+"%":QStringLiteral("未提供");
}
QString reasonText(const std::string& reason){
    const QMap<QString,QString> labels{
        {"normal",QStringLiteral("资源正常，按当前模式与并发目标派发")},
        {"disabled",QStringLiteral("自动运行策略已关闭；当前仅受人工上线/下线控制")},
        {"adaptive_disabled",QStringLiteral("自适应调节已关闭；严重内存/磁盘不足仍停止新派发")},
        {"upgrade_worker",QStringLiteral("该 Worker 未声明资源能力，请升级后再启用自动策略")},
        {"telemetry_missing",QStringLiteral("等待有效资源采样，暂不派发新任务")},
        {"telemetry_stale",QStringLiteral("资源采样过期，暂不派发新任务")},
        {"disk_low",QStringLiteral("工作盘空间不足，已停止新派发")},
        {"memory_critical",QStringLiteral("可用内存严重不足，已停止新派发")},
        {"memory_pressure",QStringLiteral("内存持续紧张，按策略逐步收缩并发")},
        {"swap_pressure",QStringLiteral("持续换页且可用内存偏低，按策略逐步收缩并发")},
        {"memory_stall",QStringLiteral("内存访问持续阻塞，按策略逐步收缩并发")},
        {"io_pressure",QStringLiteral("IO 持续阻塞，按策略逐步收缩并发")},
        {"cpu_warmup",QStringLiteral("CPU 采样预热中，暂不扩容")},
        {"cpu_busy",QStringLiteral("CPU 繁忙，暂不扩容；不会仅因 CPU 高而强制缩容")},
        {"gradual_recovery",QStringLiteral("资源恢复观察中，满足稳定时间后逐步增加并发")}};
    return labels.value(QString::fromStdString(reason),QStringLiteral("服务端策略：")+QString::fromStdString(reason));
}
}
void MainWindow::showHostResources(){
    if(!hostResourceStatus_)return;
    Json policy=Json::object();bool connected=false;int best=-1;
    for(const auto& w:workers_)if(text(w,"machine_id")==machines_->currentText()){
        const bool live=text(w,"status")=="ONLINE"||text(w,"status")=="DRAINING";connected=connected||live;
        const auto p=w.value("run_policy",Json::object());if(!p.is_object())continue;
        const int rank=(p.value("supported",false)?2:0)+(live?1:0);
        if(rank>best){best=rank;policy=p;}
    }
    auto raw=policy.value("resources",Json::object());if(!raw.is_object())raw=Json::object();
    const auto thresholds=policy.value("resource_thresholds",Json::object());
    const auto state=policy.value("resource_status",std::string());
    const bool supported=policy.value("supported",false),valid=supported&&raw.value("valid",false);
    const auto baseAge=number(policy,"sample_age_seconds");
    const auto age=baseAge?std::optional<double>(*baseAge+(workerUpdated_.isValid()?workerUpdated_.elapsed()/1000.0:0)):std::optional<double>();
    const auto staleAfter=number(thresholds,"stale_seconds");
    const bool stale=workersStale_||state=="stale"||(age&&staleAfter&&*age>*staleAfter);
    const bool fresh=connected&&!stale&&supported&&valid&&state=="fresh";
    QString status;
    if(machines_->currentText().isEmpty())status=QStringLiteral("尚未选择工作服务器");
    else if(workersStale_)status=QStringLiteral("连接未确认 · 保留上次资源快照");
    else if(!connected)status=QStringLiteral("工作机失联 · 仅为最后已知采样");
    else if(!supported)status=QStringLiteral("旧版未上报 · 需升级 Worker");
    else if(!valid)status=state=="invalid"?QStringLiteral("资源采样无效"):QStringLiteral("等待资源采样");
    else if(stale)status=QStringLiteral("资源采样已过期");
    else status=QStringLiteral("资源采样有效");
    if(age)status+=QStringLiteral(" · %1 秒前").arg(qulonglong(*age));
    else if(valid)status+=QStringLiteral(" · Server 未提供采样时间");
    hostResourceStatus_->setText(status);
    hostResourceStatus_->setStyleSheet(fresh?"font-size:11px;color:#288b77;":"font-size:11px;color:#8a6c36;");
    auto metric=[&](const QString& key,const QString& value,const QString& detail,std::optional<double> percent,const QString& color){
        hostResourceValues_[key]->setText(value);hostResourceDetails_[key]->setText(detail);
        hostResourceValues_[key]->setStyleSheet(QString("font-size:19px;font-weight:600;color:%1;").arg(fresh?QString("#172b48"):QString("#8191a7")));
        auto bar=hostResourceBars_[key];bar->setValue(percent?qBound(0,int(*percent*10),1000):0);
        bar->setStyleSheet(QString("QProgressBar{border:0;border-radius:2px;background:#edf1f7;}QProgressBar::chunk{border-radius:2px;background:%1;}").arg(fresh&&percent?color:QString("#c3cddb")));
        bar->setToolTip(percent?detail:QStringLiteral("未提供有效采样；空条不表示资源为零"));
    };
    const auto cpu=valid?number(raw,"cpu_percent"):std::optional<double>();
    metric("cpu",cpu&&*cpu<=100?QString::number(*cpu,'f',1)+"%":QStringLiteral("—"),
        cpu?QStringLiteral("整机 CPU；高负载不等于故障"):QStringLiteral("采样预热 / 未提供"),cpu&&*cpu<=100?cpu:std::optional<double>(),"#4d82ee");
    const auto available=valid?number(raw,"mem_available_bytes"):std::optional<double>(),total=valid?number(raw,"mem_total_bytes"):std::optional<double>();
    const bool memory=available&&total&&*total>0&&*available<=*total;const double memoryPercent=memory?100.0 * (*available) / *total:0;
    const auto warn=number(thresholds,"memory_warn_percent"),critical=number(thresholds,"memory_critical_percent");
    const QString memoryColor=critical&&memoryPercent<*critical?"#d75b6c":warn&&memoryPercent<*warn?"#d6a044":"#45a18b";
    metric("memory",memory?bytes(*available):QStringLiteral("—"),memory?QStringLiteral("总计 %1 · 可用 %2%").arg(bytes(*total)).arg(memoryPercent,0,'f',1):QStringLiteral("未提供可用内存"),memory?std::optional<double>(memoryPercent):std::optional<double>(),memoryColor);
    const auto free=valid?number(raw,"disk_free_bytes"):std::optional<double>(),diskTotal=valid?number(raw,"disk_total_bytes"):std::optional<double>();
    const bool disk=free&&diskTotal&&*diskTotal>0&&*free<=*diskTotal;const auto reserve=number(thresholds,"disk_reserve_bytes");
    metric("disk",disk?bytes(*free):QStringLiteral("—"),disk?QStringLiteral("总计 %1 · 执行盘最低余量").arg(bytes(*diskTotal)):QStringLiteral("未提供执行盘空间"),disk?std::optional<double>(100.0 * (*free) / *diskTotal):std::optional<double>(),reserve&&free&&*free<*reserve?"#d75b6c":"#4d82ee");
    const auto active=number(policy,"active_tasks"),target=number(policy,"target_limit"),limit=number(policy,"mode_limit"),registered=number(policy,"reported_slots");
    metric("concurrency",active&&target?QStringLiteral("%1 / %2").arg(qulonglong(*active)).arg(qulonglong(*target)):QStringLiteral("—"),
        limit&&registered?QStringLiteral("模式上限 %1 · 注册 %2").arg(qulonglong(*limit)).arg(qulonglong(*registered)):QStringLiteral("Server 未提供并发预算"),
        active&&target&&*target>0?std::optional<double>(qMin(100.0,100.0 * (*active) / *target)):std::optional<double>(),active&&target&&*active>*target?"#d6a044":"#8a72cf");
    QString decision;
    if(machines_->currentText().isEmpty())decision=QStringLiteral("选择工作服务器后显示真实资源，不填充模拟数据。");
    else if(!fresh)decision=QStringLiteral("当前资源状态未确认；以上数值不代表实时可用量。");
    else decision=reasonText(policy.value("reason",std::string("unknown")));
    if(active&&target&&*active>*target)decision+=QStringLiteral(" 当前活动任务超出目标，等待正常结束，不强杀 testcase。");
    hostResourceDecision_->setText(decision);hostResourceDecision_->setToolTip(QStringLiteral("活动任务计入重启后等待恢复确认的执行；目标并发不是当前已上线 slot 数，也不保证立即满载。"));
    hostResourceExtra_->setText(valid?QStringLiteral("换入 %1 · 换出 %2　|　PSI 60s：内存 %3 / IO %4%5")
        .arg(rate(raw,"swap_in_bytes_per_sec")).arg(rate(raw,"swap_out_bytes_per_sec")).arg(psi(raw,"psi_memory_some_avg60")).arg(psi(raw,"psi_io_some_avg60"))
        .arg(fresh?QString():QStringLiteral("（最后采样）")):QStringLiteral("换页 / PSI：未提供；缺少 PSI 不代表没有资源压力。"));
    QStringList rules;
    if(warn&&critical)rules<<QStringLiteral("内存预警低于 %1%，严重不足低于 %2%").arg(*warn).arg(*critical);
    if(reserve)rules<<QStringLiteral("工作盘保留 %1").arg(bytes(*reserve));
    const auto expand=number(thresholds,"cpu_expand_percent");if(expand)rules<<QStringLiteral("扩容要求 CPU 低于 %1%").arg(*expand);
    hostResourceStatus_->setToolTip(rules.isEmpty()?QStringLiteral("Server 未提供阈值；本界面不自行猜测策略。"):rules.join(QStringLiteral("；")));
}
