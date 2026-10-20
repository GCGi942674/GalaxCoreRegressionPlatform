#include "WorkerServer.h"
#include "Logging.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include "scheduler/SchedulingDriver.h"
#include "config/Config.h"

using namespace GCRP::Protocol;

namespace {
struct PendingLogReply {
    std::shared_ptr<Connection> connection;
    explicit PendingLogReply(std::shared_ptr<Connection> value):connection(std::move(value)){connection->incPendingTasks();}
    ~PendingLogReply(){connection->decPendingTasks();}
};
void sendLogResponse(const std::shared_ptr<Connection>& conn,const ProtocolMessage& response){
    std::string encoded;ProtocolError error;
    if(!ProtocolCodec::encode(response,encoded,error))
        LOG_ERROR(GCRP::LogModule::NETWORK,"async response encoding failed, "+conn->logContext());
    else if(!conn->sendResponsePacket(MessageCodec::encode(encoded)))
        LOG_WARN(GCRP::LogModule::NETWORK,"async response queue rejected, "+conn->logContext());
}
ProtocolMessage logFailure(uint64_t id,const char* message){
    ProtocolMessage response;response.type=MessageType::RESPONSE;response.request_id=id;
    response.data={{"code",4},{"message",message},{"result",nullptr}};return response;
}
}

namespace GCRP {

WorkerServer::WorkerServer(bool persistent)
{
    if (persistent)
        store_ptr_ = std::make_unique<StateStore>(
            Config::instance().getString("database.path", ""),
            Config::instance().getInt("database.busy_timeout_ms", 5000));
    worker_manager_ptr_ = std::make_unique<WorkerManager>(store_ptr_.get());
    registry_ptr_ = std::make_unique<WorkerConnectionRegistry>();
    dispatcher_ptr_ = std::make_unique<NetworkWorkerDispatcher>(*registry_ptr_);
    const int max_running = Config::instance().getInt("scheduler.max_running_tasks", 32);
    if (max_running < 1 || max_running > 100000)
        throw std::runtime_error("invalid scheduler.max_running_tasks");
    scheduler_ptr_ = std::make_unique<Scheduler>(*worker_manager_ptr_, static_cast<size_t>(max_running));
    if (store_ptr_) scheduler_ptr_->restoreFrom(*store_ptr_);
    coordinator_ptr_ = std::make_unique<SchedulingCoordinator>(*scheduler_ptr_, *dispatcher_ptr_);
    scheduling_driver_ptr_ = std::make_unique<SchedulingDriver>(*coordinator_ptr_);
    task_service_ptr_ = std::make_unique<TaskService>(*scheduler_ptr_, *scheduling_driver_ptr_);
    if (store_ptr_) task_service_ptr_->setNextTaskId(scheduler_ptr_->nextTaskId());
    automation_ptr_=std::make_unique<AutomationService>(store_ptr_.get(),*scheduler_ptr_,*task_service_ptr_);
    router_ptr_ = std::make_unique<Protocol::ProtocolRouter>(*task_service_ptr_, *scheduler_ptr_, *worker_manager_ptr_, *scheduling_driver_ptr_,automation_ptr_.get());

    LOG_INFO(LogModule::SERVER, "WorkerServer initialized");
    if (persistent) {
        const int timeout = Config::instance().getInt("server.worker_timeout_seconds", 30);
        const int retries = Config::instance().getInt("server.recovery_max_retries", 1);
        if (timeout < 2 || timeout > 3600 || retries < 0 || retries > 100)
            throw std::runtime_error("invalid worker recovery configuration");
        const int retention = Config::instance().getInt("log.retention_days", 30);
        if (retention < 1 || retention > 3650) throw std::runtime_error("invalid log.retention_days");
        liveness_thread_ = std::thread([this, timeout, retention] {
            std::unique_lock<std::mutex> wait_lock(liveness_mutex_);
            auto next_prune = std::chrono::steady_clock::time_point::min();
            while (!liveness_cv_.wait_for(wait_lock, std::chrono::seconds(1), [this] { return stopping_; })) {
                wait_lock.unlock();
                try {
                    std::lock_guard<std::mutex> lock(session_mutex_);
                    worker_manager_ptr_->checkTimeouts(std::chrono::seconds(timeout));
                    if(worker_manager_ptr_->refreshRunPolicies())scheduling_driver_ptr_->requestSchedule();
                } catch (const std::exception& error) {
                    LOG_ERROR(LogModule::WORKER, std::string("liveness check failed: ") + error.what());
                }
                if (std::chrono::steady_clock::now() >= next_prune) {
                    next_prune = std::chrono::steady_clock::now() + std::chrono::hours(1);
                    try {
                        router_ptr_->pruneLogs(retention);
                        Logger::instance().pruneOldLogs(retention);
                    } catch (const std::exception& error) {
                        LOG_ERROR(LogModule::SERVER, std::string("log retention failed: ") + error.what());
                    }
                }
                wait_lock.lock();
            }
        });
    }
    try{
        for(int i=0;i<2;++i)log_threads_.emplace_back([this]{
            for(;;){std::function<void()> job;{
                std::unique_lock<std::mutex> lock(log_mutex_);log_cv_.wait(lock,[this]{return log_stopping_||!log_jobs_.empty();});
                if(log_jobs_.empty()&&log_stopping_)return;job=std::move(log_jobs_.front());log_jobs_.pop_front();
            }try{job();}catch(...){/* The requester times out and retries; keep the I/O worker alive. */}}
        });
        for(int i=0;i<2;++i)query_threads_.emplace_back([this]{
            for(;;){std::function<void()> job;{
                std::unique_lock<std::mutex> lock(query_mutex_);query_cv_.wait(lock,[this]{return query_stopping_||!query_jobs_.empty();});
                if(query_jobs_.empty()&&query_stopping_)return;job=std::move(query_jobs_.front());query_jobs_.pop_front();
            }try{job();}catch(...){/* A failed read cannot kill the query worker. */}}
        });
        automation_ptr_->start();
    }catch(...){
        {std::lock_guard<std::mutex> lock(query_mutex_);query_stopping_=true;}query_cv_.notify_all();
        for(auto& thread:query_threads_)if(thread.joinable())thread.join();
        {std::lock_guard<std::mutex> lock(log_mutex_);log_stopping_=true;}log_cv_.notify_all();
        for(auto& thread:log_threads_)if(thread.joinable())thread.join();
        {std::lock_guard<std::mutex> lock(liveness_mutex_);stopping_=true;}liveness_cv_.notify_all();if(liveness_thread_.joinable())liveness_thread_.join();throw;
    }
}

WorkerServer::~WorkerServer() {
    if(automation_ptr_)automation_ptr_->stop();
    {
        std::lock_guard<std::mutex> lock(liveness_mutex_);
        stopping_ = true;
    }
    liveness_cv_.notify_all();
    if (liveness_thread_.joinable()) liveness_thread_.join();
    {std::lock_guard<std::mutex> lock(query_mutex_);query_stopping_=true;}query_cv_.notify_all();
    for(auto& thread:query_threads_)if(thread.joinable())thread.join();
    {std::lock_guard<std::mutex> lock(log_mutex_);log_stopping_=true;}log_cv_.notify_all();
    for(auto& thread:log_threads_)if(thread.joinable())thread.join();
}

void WorkerServer::attach(EchoServer& transport) {
    transport.setConnectionCallbacks(
        [this](const std::shared_ptr<Connection>& conn) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto session = std::make_unique<ConnectionSession>(conn,
                *worker_manager_ptr_, *registry_ptr_, *router_ptr_, *scheduling_driver_ptr_);
            sessions_.emplace(conn.get(), std::move(session));
        },
        [this](const std::shared_ptr<Connection>& conn, const std::string& message) {
            ProtocolMessage request;ProtocolError error;
            const bool decoded=ProtocolCodec::decode(message,request,error);
            const bool history=decoded&&(request.type==MessageType::QUERY_REGRESSION||request.type==MessageType::QUERY_PROGRESS||request.type==MessageType::QUERY_ROLLBACK);
            if(decoded&&(history||request.type==MessageType::LOG_APPEND||request.type==MessageType::LOG_STATE||request.type==MessageType::QUERY_LOGS||request.type==MessageType::READ_LOG||request.type==MessageType::QUERY_DAILY_PLAN)){
                bool permitted=false;{std::lock_guard<std::mutex> lock(session_mutex_);auto it=sessions_.find(conn.get());if(it!=sessions_.end())permitted=it->second->authorizeLog(request);}
                if(permitted){
                    if(history){
                        bool queued=false;{std::lock_guard<std::mutex> lock(query_mutex_);
                            if(!query_stopping_&&query_jobs_.size()<32){auto lease=std::make_shared<PendingLogReply>(conn);
                                query_jobs_.push_back([this,conn,request,lease]{try{sendLogResponse(conn,router_ptr_->handle(request));}
                                    catch(...){sendLogResponse(conn,logFailure(request.request_id,"history query failed; retry later"));}});queued=true;}
                        }
                        if(queued)query_cv_.notify_one();else sendLogResponse(conn,logFailure(request.request_id,"history query queue full; retry later"));return;
                    }
                    bool queued=false;{std::lock_guard<std::mutex> lock(log_mutex_);
                        if(!log_stopping_&&log_jobs_.size()<64){auto lease=std::make_shared<PendingLogReply>(conn);log_jobs_.push_back([this,conn,request,lease]{try{sendLogResponse(conn,router_ptr_->handle(request));}catch(...){sendLogResponse(conn,logFailure(request.request_id,"log I/O failed; retry later"));}});queued=true;}
                    }
                    if(queued)log_cv_.notify_one();else sendLogResponse(conn,logFailure(request.request_id,"log I/O queue full; retry later"));return;
                }
            }
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto it = sessions_.find(conn.get());
            if (it != sessions_.end()) it->second->onMessage(message);
        },
        [this](const std::shared_ptr<Connection>& conn) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            auto it = sessions_.find(conn.get());
            if (it == sessions_.end()) return;
            it->second->handleClose();
            sessions_.erase(it);
        });
}

SchedulingResult WorkerServer::runOnce() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return coordinator_ptr_->runOnce();
}

} // namespace GCRP
