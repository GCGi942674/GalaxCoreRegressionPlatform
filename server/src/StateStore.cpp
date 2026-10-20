#include "persistence/StateStore.h"
#include "task/TaskStatus.h"
#include "task/TaskType.h"
#include <sqlite3.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <limits>
#include <map>

namespace GCRP {
namespace {
void check(int code, sqlite3* db) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw std::runtime_error(std::string("SQLite: ") + sqlite3_errmsg(db));
}
struct Statement {
    sqlite3_stmt* value{nullptr};
    sqlite3* db;
    Statement(sqlite3* d, const char* sql) : db(d) {
        check(sqlite3_prepare_v2(db, sql, -1, &value, nullptr), db);
    }
    ~Statement() { sqlite3_finalize(value); }
    void text(int index, const std::string& data) {
        check(sqlite3_bind_text(value, index, data.c_str(), static_cast<int>(data.size()), SQLITE_TRANSIENT), db);
    }
    void number(int index, int64_t data) { check(sqlite3_bind_int64(value, index, data), db); }
    void nullable(int index, const std::string& data) {
        if (data.empty()) check(sqlite3_bind_null(value, index), db);
        else text(index, data);
    }
    void run() { check(sqlite3_step(value), db); }
    void reset() {
        check(sqlite3_reset(value), db);
        check(sqlite3_clear_bindings(value), db);
    }
};
std::string column(sqlite3_stmt* statement, int index) {
    const auto* data = sqlite3_column_text(statement, index);
    return data ? reinterpret_cast<const char*>(data) : std::string();
}
int64_t millis(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}
std::chrono::system_clock::time_point fromMillis(int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}
TaskStatus statusFrom(const std::string& value) {
    for (auto status : {TaskStatus::PENDING, TaskStatus::QUEUED, TaskStatus::ASSIGNED,
                        TaskStatus::RUNNING, TaskStatus::SUCCEEDED, TaskStatus::FAILED,
                        TaskStatus::TIMEOUT, TaskStatus::CANCELLED})
        if (toString(status) == value) return status;
    throw std::runtime_error("invalid persisted task status");
}
std::string batchId(const Task& task) {
    const auto& payload = task.getPayload();
    if (payload.is_object() && payload.contains("context") &&
        payload["context"].is_object() &&
        payload["context"].contains("batch_id") &&
        payload["context"]["batch_id"].is_string())
        return payload["context"]["batch_id"].get<std::string>();
    return {};
}
int64_t checkedId(TaskId id) {
    if (id == 0 || id > static_cast<TaskId>(std::numeric_limits<int64_t>::max()))
        throw std::runtime_error("task id outside SQLite range");
    return static_cast<int64_t>(id);
}
void bindQueryIndex(Statement& statement,const Task& task){
    statement.number(1,checkedId(task.getId()));statement.text(2,taskBusinessOutcome(task));
    const auto source=taskContext(task,"source"),day=taskContext(task,"date"),stage=taskContext(task,"template");
    statement.text(3,source);statement.text(4,day);statement.text(5,stage);
    const auto& p=task.getPayload();const bool has_case=p.is_object()&&p.contains("case")&&p["case"].is_string()&&!p["case"].get_ref<const std::string&>().empty();
    statement.number(6,has_case?1:0);statement.number(7,source=="scheduled"&&!stage.empty()&&validQueryDate(day)?1:0);
    statement.run();statement.reset();
}
uint64_t versionAt(sqlite3* db,const std::vector<std::string>& dates) {
    std::string sql="SELECT COALESCE(MAX(generation),0) FROM query_versions WHERE scope IN (";
    for(size_t i=0;i<(dates.empty()?size_t(1):dates.size());++i)sql+=i?",?":"?";
    sql+=")";Statement q(db,sql.c_str());
    if(dates.empty())q.text(1,"*");else for(size_t i=0;i<dates.size();++i)q.text(int(i+1),"d:"+dates[i]);
    check(sqlite3_step(q.value),db);return static_cast<uint64_t>(sqlite3_column_int64(q.value,0));
}
Task historyRow(sqlite3_stmt* row) {
    if(column(row,1)!="REGRESSION")throw std::runtime_error("invalid persisted task type");
    Task task(static_cast<TaskId>(sqlite3_column_int64(row,0)),TaskType::REGRESSION,column(row,2),column(row,3));
    task.setPriority(sqlite3_column_int(row,4));
    task.restore(statusFrom(column(row,5)),column(row,8),sqlite3_column_int(row,9),
        nlohmann::json::parse(column(row,6)),sqlite3_column_type(row,7)==SQLITE_NULL?nlohmann::json(nullptr):nlohmann::json::parse(column(row,7)),
        static_cast<uint64_t>(sqlite3_column_int64(row,10)),column(row,11),sqlite3_column_int(row,12)!=0,
        fromMillis(sqlite3_column_int64(row,13)),fromMillis(sqlite3_column_int64(row,14)),fromMillis(sqlite3_column_int64(row,15)));
    return task;
}
} // namespace

void StateStore::exec(const char* sql) const {
    char* message = nullptr;
    int result = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        std::string error = message ? message : sqlite3_errmsg(db_);
        sqlite3_free(message);
        throw std::runtime_error("SQLite: " + error);
    }
}

StateStore::StateStore(const std::string& path, int busy_timeout_ms) {
    if (path.empty() || path == ":memory:") throw std::runtime_error("persistent database path required");
    if (busy_timeout_ms < 1 || busy_timeout_ms > 60000)
        throw std::runtime_error("invalid SQLite busy timeout");
    const std::filesystem::path db_path(path);
    if (!db_path.parent_path().empty()) std::filesystem::create_directories(db_path.parent_path());
    lock_fd_ = ::open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
        throw std::runtime_error("database already used by another server");
    try {
        check(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), db_);
        check(sqlite3_busy_timeout(db_, busy_timeout_ms), db_);
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=FULL");
        exec("PRAGMA foreign_keys=ON");
        int schema_version = 0;
        {
            Statement version(db_, "PRAGMA user_version");
            check(sqlite3_step(version.value), db_);
            schema_version = sqlite3_column_int(version.value, 0);
        }
        if (schema_version < 0 || schema_version > 6)
            throw std::runtime_error("unsupported GCRP database schema version");
        exec("BEGIN IMMEDIATE");
        try {
            exec("CREATE TABLE IF NOT EXISTS batches (batch_id TEXT PRIMARY KEY, stop_requested INTEGER NOT NULL DEFAULT 0 CHECK(stop_requested IN (0,1)))");
            exec("CREATE TABLE IF NOT EXISTS tasks (task_id INTEGER PRIMARY KEY AUTOINCREMENT, batch_id TEXT REFERENCES batches(batch_id), task_type TEXT NOT NULL CHECK(task_type='REGRESSION'), target TEXT NOT NULL, revision TEXT NOT NULL, priority INTEGER NOT NULL CHECK(priority BETWEEN -100 AND 100), status TEXT NOT NULL CHECK(status IN ('PENDING','QUEUED','ASSIGNED','RUNNING','SUCCEEDED','FAILED','TIMEOUT','CANCELLED')), payload_json TEXT NOT NULL, execution_json TEXT, worker_id TEXT, retry_count INTEGER NOT NULL DEFAULT 0 CHECK(retry_count>=0), queue_sequence INTEGER NOT NULL CHECK(queue_sequence>=0), run_id TEXT, recovery_required INTEGER NOT NULL DEFAULT 0 CHECK(recovery_required IN (0,1)), created_at_ms INTEGER NOT NULL, started_at_ms INTEGER, finished_at_ms INTEGER)");
            exec("CREATE TABLE IF NOT EXISTS task_runs (run_id TEXT PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(task_id), attempt_no INTEGER NOT NULL CHECK(attempt_no>0), worker_id TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('ASSIGNED','RUNNING','SUCCEEDED','FAILED','TIMEOUT','DISPATCH_FAILED')), assigned_at_ms INTEGER NOT NULL, started_at_ms INTEGER, finished_at_ms INTEGER, execution_json TEXT, UNIQUE(task_id,attempt_no))");
            exec("CREATE TABLE IF NOT EXISTS submission_keys (submission_key TEXT PRIMARY KEY, request_json TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(task_id))");
            exec("CREATE TABLE IF NOT EXISTS recovery_decisions (run_id TEXT PRIMARY KEY REFERENCES task_runs(run_id), task_id INTEGER NOT NULL REFERENCES tasks(task_id), worker_id TEXT NOT NULL, decision TEXT NOT NULL CHECK(decision IN ('requeued','failed','cancelled')))");
            exec("CREATE INDEX IF NOT EXISTS tasks_queue ON tasks(status,recovery_required,priority,queue_sequence)");
            exec("CREATE INDEX IF NOT EXISTS tasks_batch ON tasks(batch_id,status)");
            exec("CREATE TABLE IF NOT EXISTS worker_management (name TEXT PRIMARY KEY, value TEXT NOT NULL)");
            exec("CREATE TABLE IF NOT EXISTS task_query_index (task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id), outcome TEXT NOT NULL, source TEXT NOT NULL, day TEXT NOT NULL, template TEXT NOT NULL, has_case INTEGER NOT NULL, classified INTEGER NOT NULL)");
            exec("CREATE INDEX IF NOT EXISTS query_day ON task_query_index(day,template,task_id)");
            exec("CREATE INDEX IF NOT EXISTS query_classified ON task_query_index(classified,has_case)");
            exec("CREATE INDEX IF NOT EXISTS tasks_target_revision ON tasks(target,revision,task_id)");
            exec("CREATE INDEX IF NOT EXISTS tasks_revision ON tasks(revision,task_id)");
            // Additive metadata: no task schema changes and no async write-behind.
            exec("CREATE TABLE IF NOT EXISTS query_versions (scope TEXT PRIMARY KEY,generation INTEGER NOT NULL)");
            exec("INSERT OR IGNORE INTO query_versions VALUES('*',0)");
            exec("CREATE TABLE IF NOT EXISTS deleted_tasks (task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id))");
            exec("CREATE TABLE IF NOT EXISTS deleted_batches (batch_id TEXT PRIMARY KEY REFERENCES batches(batch_id))");
            if(schema_version<4){
                Statement index(db_,"INSERT OR REPLACE INTO task_query_index VALUES(?,?,?,?,?,?,?)");
                visitTasks([&](Task&& task,uint64_t){bindQueryIndex(index,task);});
            }
            exec("CREATE TABLE IF NOT EXISTS daily_plans (day TEXT PRIMARY KEY, manifest_json TEXT NOT NULL)");
            exec("PRAGMA user_version=6");
            exec("UPDATE tasks SET recovery_required=1 WHERE status IN ('ASSIGNED','RUNNING')");
            exec("COMMIT");
        } catch (...) { exec("ROLLBACK"); throw; }
        {
            Statement check_db(db_, "PRAGMA quick_check");
            check(sqlite3_step(check_db.value), db_);
            if (column(check_db.value, 0) != "ok")
                throw std::runtime_error("SQLite integrity check failed");
        }
        check(sqlite3_open_v2(path.c_str(),&history_db_,SQLITE_OPEN_READONLY,nullptr),history_db_);
        check(sqlite3_busy_timeout(history_db_,busy_timeout_ms),history_db_);
        check(sqlite3_exec(history_db_,"PRAGMA query_only=ON",nullptr,nullptr,nullptr),history_db_);
    } catch (...) {
        if(history_db_)sqlite3_close(history_db_);history_db_=nullptr;
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }
        throw;
    }
}

StateStore::~StateStore() {
    if(history_db_)sqlite3_close(history_db_);
    if (db_) sqlite3_close(db_);
    if (lock_fd_ >= 0) ::close(lock_fd_);
}

std::pair<TaskId,uint64_t> StateStore::highWaterMarks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_,"SELECT COALESCE(MAX(task_id),0),COALESCE(MAX(queue_sequence),0) FROM tasks");
    check(sqlite3_step(query.value),db_);
    return {static_cast<TaskId>(sqlite3_column_int64(query.value,0)),static_cast<uint64_t>(sqlite3_column_int64(query.value,1))};
}

nlohmann::json StateStore::statusCounts() const {
    std::lock_guard<std::mutex> lock(mutex_);nlohmann::json result=nlohmann::json::object();
    Statement query(db_,"SELECT status,COUNT(*) FROM tasks WHERE task_id NOT IN (SELECT task_id FROM deleted_tasks) GROUP BY status");
    for(;;){auto step=sqlite3_step(query.value);if(step==SQLITE_DONE)break;check(step,db_);result[column(query.value,0)]=sqlite3_column_int64(query.value,1);}
    return result;
}

void StateStore::publishDailyPlan(const nlohmann::json& manifest) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto day=manifest.at("date").get<std::string>(), body=manifest.dump();
    exec("BEGIN IMMEDIATE");
    try{
        Statement insert(db_,"INSERT OR IGNORE INTO daily_plans VALUES(?,?)");
        insert.text(1,day);insert.text(2,body);insert.run();
        Statement query(db_,"SELECT manifest_json FROM daily_plans WHERE day=?");query.text(1,day);
        check(sqlite3_step(query.value),db_);
        if(column(query.value,0)!=body)throw std::invalid_argument("daily plan already published; resume saved Clock plan");
        Statement batch(db_,"INSERT OR IGNORE INTO batches(batch_id) VALUES(?)");
        for(const auto& item:manifest.at("batches")){batch.text(1,item.at("batch_id").get<std::string>());batch.run();batch.reset();}
        exec("COMMIT");
    }catch(...){exec("ROLLBACK");throw;}
}
bool StateStore::hasBatch(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_,"SELECT 1 FROM batches WHERE batch_id=? AND batch_id NOT IN (SELECT batch_id FROM deleted_batches)");query.text(1,id);
    const auto step=sqlite3_step(query.value);if(step==SQLITE_DONE)return false;
    check(step,db_);return true;
}

bool StateStore::batchDeleted(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_,"SELECT 1 FROM deleted_batches WHERE batch_id=?");query.text(1,id);
    const auto step=sqlite3_step(query.value);if(step==SQLITE_DONE)return false;
    check(step,db_);return true;
}

size_t StateStore::deletedCount(const std::string& batch_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_,"SELECT COUNT(*) FROM tasks t JOIN deleted_tasks d ON d.task_id=t.task_id WHERE t.batch_id=?");query.text(1,batch_id);
    check(sqlite3_step(query.value),db_);return static_cast<size_t>(sqlite3_column_int64(query.value,0));
}

size_t StateStore::deleteRecords(TaskId id,const std::string& batch_id) {
    if((id==0)==batch_id.empty())throw std::invalid_argument("choose task_id or batch_id");
    std::lock_guard<std::mutex> lock(mutex_);
    exec("BEGIN IMMEDIATE");
    try {
        const std::string where=id?"task_id=?":"batch_id=?";
        Statement active(db_,("SELECT 1 FROM tasks WHERE "+where+" AND status IN ('PENDING','QUEUED','ASSIGNED','RUNNING') LIMIT 1").c_str());
        if(id)active.number(1,checkedId(id));else active.text(1,batch_id);
        const auto step=sqlite3_step(active.value);
        if(step==SQLITE_ROW)throw std::logic_error("active tasks cannot be deleted; cancel queued cases and wait for running cases to finish");
        check(step,db_);
        std::set<std::string> days;
        Statement affected(db_,("SELECT DISTINCT q.day FROM task_query_index q JOIN tasks t ON t.task_id=q.task_id WHERE t."+where).c_str());
        if(id)affected.number(1,checkedId(id));else affected.text(1,batch_id);
        for(;;){const auto step=sqlite3_step(affected.value);if(step==SQLITE_DONE)break;check(step,db_);days.insert(column(affected.value,0));}
        Statement mark(db_,("INSERT OR IGNORE INTO deleted_tasks SELECT task_id FROM tasks WHERE "+where).c_str());
        if(id)mark.number(1,checkedId(id));else mark.text(1,batch_id);mark.run();
        const auto count=static_cast<size_t>(sqlite3_changes(db_));
        if(!batch_id.empty()){
            Statement batch(db_,"INSERT OR IGNORE INTO deleted_batches SELECT batch_id FROM batches WHERE batch_id=?");batch.text(1,batch_id);batch.run();
            Statement stop(db_,"UPDATE batches SET stop_requested=1 WHERE batch_id=?");stop.text(1,batch_id);stop.run();
        }
        if(count)bumpHistory(days);
        exec("COMMIT");return count;
    }catch(...){exec("ROLLBACK");throw;}
}

nlohmann::json StateStore::dailyPlan(const std::string& day) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_,"SELECT manifest_json FROM daily_plans WHERE day=?");query.text(1,day);
    const auto step=sqlite3_step(query.value);if(step==SQLITE_DONE)return nullptr;
    check(step,db_);return nlohmann::json::parse(column(query.value,0));
}

std::vector<PersistedTask> StateStore::loadTasks(TaskId id, bool active_only,const std::string& batch_id,bool include_deleted) const {
    std::vector<PersistedTask> result;
    visitTasks([&](Task&& task,uint64_t sequence){result.push_back({std::move(task),sequence});},id,active_only,batch_id,{},include_deleted);
    return result;
}

void StateStore::visitTasks(const std::function<void(Task&&,uint64_t)>& visitor,TaskId id,bool active_only,const std::string& batch_id,const std::vector<std::string>& dates,bool include_deleted) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sql="SELECT task_id,task_type,target,revision,priority,status,payload_json,execution_json,COALESCE(worker_id,''),retry_count,queue_sequence,COALESCE(run_id,''),recovery_required,created_at_ms,COALESCE(started_at_ms,0),COALESCE(finished_at_ms,0) FROM tasks WHERE 1=1";
    if(!include_deleted)sql+=" AND task_id NOT IN (SELECT task_id FROM deleted_tasks)";
    if(id)sql+=" AND task_id=?";
    if(active_only)sql+=" AND status IN ('PENDING','QUEUED','ASSIGNED','RUNNING')";
    if(!batch_id.empty())sql+=" AND batch_id=?";
    if(!dates.empty()){
        sql+=" AND task_id IN (SELECT task_id FROM task_query_index WHERE day IN (";
        for(size_t i=0;i<dates.size();++i)sql+=i?",?":"?";
        sql+="))";
    }
    sql+=" ORDER BY task_id";Statement rows(db_,sql.c_str());int parameter=1;
    if(id)rows.number(parameter++,checkedId(id));
    if(!batch_id.empty())rows.text(parameter++,batch_id);
    for(const auto& day:dates)rows.text(parameter++,day);
    while (true) {
        int step = sqlite3_step(rows.value);
        if (step == SQLITE_DONE) break;
        check(step, db_);
        auto type = column(rows.value, 1) == "REGRESSION" ? TaskType::REGRESSION : TaskType::UNKNOWN;
        if (type == TaskType::UNKNOWN) throw std::runtime_error("invalid persisted task type");
        Task task(static_cast<TaskId>(sqlite3_column_int64(rows.value, 0)), type,
                  column(rows.value, 2), column(rows.value, 3));
        auto payload = nlohmann::json::parse(column(rows.value, 6));
        auto execution = sqlite3_column_type(rows.value, 7) == SQLITE_NULL ?
            nlohmann::json(nullptr) : nlohmann::json::parse(column(rows.value, 7));
        auto sequence = static_cast<uint64_t>(sqlite3_column_int64(rows.value, 10));
        task.setPriority(sqlite3_column_int(rows.value, 4));
        task.restore(statusFrom(column(rows.value, 5)), column(rows.value, 8),
                     sqlite3_column_int(rows.value, 9), std::move(payload), std::move(execution),
                     sequence, column(rows.value, 11), sqlite3_column_int(rows.value, 12) != 0,
                     fromMillis(sqlite3_column_int64(rows.value, 13)),
                     fromMillis(sqlite3_column_int64(rows.value, 14)),
                     fromMillis(sqlite3_column_int64(rows.value, 15)));
        visitor(std::move(task), sequence);
    }
}

uint64_t StateStore::excludedRegressionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_,"SELECT COUNT(*) FROM task_query_index WHERE classified=0 AND has_case=1 AND task_id NOT IN (SELECT task_id FROM deleted_tasks)");
    check(sqlite3_step(query.value),db_);return static_cast<uint64_t>(sqlite3_column_int64(query.value,0));
}

std::vector<TaskGroupSnapshot> StateStore::taskGroups(const std::string& target,const std::string& revision,const std::string& batch_id) const {
    std::map<std::string,nlohmann::json> groups;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string sql="SELECT COALESCE(t.batch_id,''),CASE WHEN t.batch_id IS NULL THEN t.task_id ELSE 0 END,MAX(t.task_id),MIN(t.created_at_ms),t.status,q.outcome,COUNT(*) FROM tasks t JOIN task_query_index q ON q.task_id=t.task_id WHERE 1=1";
        sql+=" AND t.task_id NOT IN (SELECT task_id FROM deleted_tasks)";
        if(!target.empty())sql+=" AND t.target=?";if(!revision.empty())sql+=" AND t.revision=?";if(!batch_id.empty())sql+=" AND t.batch_id=?";
        sql+=" GROUP BY t.batch_id,CASE WHEN t.batch_id IS NULL THEN t.task_id ELSE 0 END,t.status,q.outcome";
        Statement query(db_,sql.c_str());int p=1;
        for(const auto* value:{&target,&revision,&batch_id})if(!value->empty())query.text(p++,*value);
        for(;;){auto step=sqlite3_step(query.value);if(step==SQLITE_DONE)break;check(step,db_);
            const auto batch=column(query.value,0);const auto key=batch.empty()?"single:"+std::to_string(sqlite3_column_int64(query.value,1)):"batch:"+batch;
            auto& group=groups[key];const auto id=static_cast<TaskId>(sqlite3_column_int64(query.value,2));const auto created=sqlite3_column_int64(query.value,3);
            if(group.is_null())group={{"id",id},{"created",created},{"counts",nlohmann::json::object()},{"outcomes",nlohmann::json::object()},{"total",0}};
            group["id"]=std::max(group["id"].get<TaskId>(),id);group["created"]=std::min(group["created"].get<int64_t>(),static_cast<int64_t>(created));
            const auto n=static_cast<uint64_t>(sqlite3_column_int64(query.value,6));
            for(const auto& bucket:{std::make_pair("counts",column(query.value,4)),std::make_pair("outcomes",column(query.value,5))})
                group[bucket.first][bucket.second]=group[bucket.first].value(bucket.second,uint64_t(0))+n;
            group["total"]=group["total"].get<uint64_t>()+n;
        }
    }
    std::vector<TaskGroupSnapshot> result;result.reserve(groups.size());
    // Scheduler holds its mutation lock across this operation: representatives
    // and aggregated counters come from the same authoritative task state.
    for(const auto& pair:groups){const auto& g=pair.second;auto row=loadTasks(g["id"].get<TaskId>());
        if(row.size()!=1)throw std::runtime_error("task group representative missing");
        result.push_back({std::move(row.front().task),{{"counts",g["counts"]},{"outcomes",g["outcomes"]},{"total",g["total"]}},g["created"].get<int64_t>()});}
    return result;
}

std::vector<std::string> StateStore::loadStoppedBatches() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement rows(db_, "SELECT batch_id FROM batches WHERE stop_requested=1");
    std::vector<std::string> result;
    while (true) {
        int step = sqlite3_step(rows.value);
        if (step == SQLITE_DONE) break;
        check(step, db_);
        result.push_back(column(rows.value, 0));
    }
    return result;
}

std::optional<std::pair<TaskId, std::string>> StateStore::findSubmission(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT task_id,request_json FROM submission_keys WHERE submission_key=?");
    query.text(1, key);
    int step = sqlite3_step(query.value);
    if (step == SQLITE_DONE) return std::nullopt;
    check(step, db_);
    return std::make_pair(static_cast<TaskId>(sqlite3_column_int64(query.value, 0)),
                          column(query.value, 1));
}

std::optional<std::string> StateStore::recoveryDecision(const std::string& run_id,
        TaskId task_id, const std::string& worker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT decision FROM recovery_decisions WHERE run_id=? AND task_id=? AND worker_id=?");
    query.text(1, run_id);
    query.number(2, checkedId(task_id));
    query.text(3, worker_id);
    const int step = sqlite3_step(query.value);
    if (step == SQLITE_DONE) return std::nullopt;
    check(step, db_);
    return column(query.value, 0);
}

std::optional<std::string> StateStore::archiveOwner(TaskId task_id, const std::string& run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT worker_id FROM task_runs WHERE task_id=? AND run_id=? "
                         "AND status IN ('SUCCEEDED','FAILED','TIMEOUT')");
    query.number(1, checkedId(task_id));
    query.text(2, run_id);
    const int step = sqlite3_step(query.value);
    if (step == SQLITE_DONE) return std::nullopt;
    check(step, db_);
    return column(query.value, 0);
}

void StateStore::save(const std::vector<Task>& tasks,
                      const std::optional<std::string>& stopped_batch,
                      const std::optional<std::string>& run_status_override,
                      const std::optional<std::pair<std::string, std::string>>& submission,
                      const std::optional<std::pair<std::string, std::string>>& recovery) {
    std::lock_guard<std::mutex> lock(mutex_);
    exec("BEGIN IMMEDIATE");
    try {
        Statement batch(db_, "INSERT OR IGNORE INTO batches(batch_id) VALUES(?)");
        Statement stop(db_, "UPDATE batches SET stop_requested=1 WHERE batch_id=?");
        Statement insert(db_, "INSERT OR IGNORE INTO tasks(task_id,batch_id,task_type,target,revision,priority,status,payload_json,execution_json,worker_id,retry_count,queue_sequence,run_id,recovery_required,created_at_ms,started_at_ms,finished_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        Statement update(db_, "UPDATE tasks SET batch_id=?,task_type=?,target=?,revision=?,priority=?,status=?,payload_json=?,execution_json=?,worker_id=?,retry_count=?,queue_sequence=?,run_id=?,recovery_required=?,created_at_ms=?,started_at_ms=?,finished_at_ms=? WHERE task_id=?");
        Statement run_insert(db_, "INSERT OR IGNORE INTO task_runs(run_id,task_id,attempt_no,worker_id,status,assigned_at_ms) VALUES(?,?,(SELECT COALESCE(MAX(attempt_no),0)+1 FROM task_runs WHERE task_id=?),?,?,?)");
        Statement run_update(db_, "UPDATE task_runs SET status=?,started_at_ms=?,finished_at_ms=?,execution_json=? WHERE run_id=?");
        Statement submit(db_, "INSERT INTO submission_keys(submission_key,request_json,task_id) VALUES(?,?,?)");
        Statement query_index(db_,"INSERT OR REPLACE INTO task_query_index VALUES(?,?,?,?,?,?,?)");
        if (stopped_batch) {
            batch.text(1, *stopped_batch); batch.run(); batch.reset();
            stop.text(1, *stopped_batch); stop.run();
        }
        std::set<std::string> affected_days;
        Statement previous_day(db_,"SELECT day FROM task_query_index WHERE task_id=?");
        for (const auto& task : tasks) {
            previous_day.number(1,checkedId(task.getId()));const auto old_step=sqlite3_step(previous_day.value);
            if(old_step==SQLITE_ROW)affected_days.insert(column(previous_day.value,0));else check(old_step,db_);
            previous_day.reset();affected_days.insert(taskContext(task,"date"));
            const auto batch_id = batchId(task);
            if (!batch_id.empty()) {
                batch.text(1, batch_id); batch.run(); batch.reset();
            }
            auto bind = [&](Statement& statement, int offset, bool with_id) {
                int i = offset;
                if (with_id) statement.number(i++, checkedId(task.getId()));
                statement.nullable(i++, batch_id);
                statement.text(i++, toString(task.getType()));
                statement.text(i++, task.getTarget());
                statement.text(i++, task.getRevision());
                statement.number(i++, task.getPriority());
                statement.text(i++, toString(task.getStatus()));
                statement.text(i++, task.getPayload().dump());
                statement.nullable(i++, task.getExecutionSummary().is_null() ? "" : task.getExecutionSummary().dump());
                statement.nullable(i++, task.getWorkerId());
                statement.number(i++, task.getRetryCount());
                statement.number(i++, static_cast<int64_t>(task.getQueueSequence()));
                statement.nullable(i++, task.getRunId());
                statement.number(i++, task.recoveryRequired() ? 1 : 0);
                statement.number(i++, millis(task.getCreatedAt()));
                statement.number(i++, millis(task.getStartedAt()));
                statement.number(i++, millis(task.getFinishedAt()));
            };
            bind(insert, 1, true); insert.run(); insert.reset();
            bind(update, 1, false); update.number(17, checkedId(task.getId())); update.run(); update.reset();
            bindQueryIndex(query_index,task);
            if (!task.getRunId().empty() &&
                (run_status_override || !task.getWorkerId().empty())) {
                run_insert.text(1, task.getRunId());
                run_insert.number(2, checkedId(task.getId()));
                run_insert.number(3, checkedId(task.getId()));
                run_insert.text(4, task.getWorkerId());
                run_insert.text(5, "ASSIGNED");
                run_insert.number(6, millis(std::chrono::system_clock::now()));
                run_insert.run(); run_insert.reset();
                run_update.text(1, run_status_override.value_or(toString(task.getStatus())));
                run_update.number(2, millis(task.getStartedAt()));
                run_update.number(3, recovery ? millis(std::chrono::system_clock::now()) : millis(task.getFinishedAt()));
                run_update.nullable(4, task.getExecutionSummary().is_null() ? "" : task.getExecutionSummary().dump());
                run_update.text(5, task.getRunId());
                run_update.run(); run_update.reset();
            }
        }
        if (submission) {
            if (tasks.size() != 1) throw std::runtime_error("submission needs one task");
            submit.text(1, submission->first);
            submit.text(2, submission->second);
            submit.number(3, checkedId(tasks.front().getId()));
            submit.run();
        }
        if (recovery) {
            if (tasks.size() != 1) throw std::runtime_error("recovery needs one task");
            Statement decision(db_, "INSERT INTO recovery_decisions(run_id,task_id,worker_id,decision) VALUES(?,?,?,?)");
            decision.text(1, tasks.front().getRunId());
            decision.number(2, checkedId(tasks.front().getId()));
            decision.text(3, recovery->first);
            decision.text(4, recovery->second);
            decision.run();
        }
        if(!tasks.empty())bumpHistory(affected_days);
        exec("COMMIT");
    } catch (...) { exec("ROLLBACK"); throw; }
}
nlohmann::json StateStore::loadWorkerControls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT value FROM worker_management WHERE name='controls'");
    int step=sqlite3_step(query.value);
    if(step==SQLITE_DONE)return {{"machines",nlohmann::json::object()},{"workers",nlohmann::json::object()}};
    check(step,db_);return nlohmann::json::parse(column(query.value,0));
}

void StateStore::bumpHistory(const std::set<std::string>& dates) {
    // Caller owns the writer mutex AND the surrounding task transaction.
    exec("UPDATE query_versions SET generation=generation+1 WHERE scope='*'");
    Statement update(db_,"INSERT OR REPLACE INTO query_versions SELECT ?,generation FROM query_versions WHERE scope='*'");
    for(const auto& day:dates)if(!day.empty()){update.text(1,"d:"+day);update.run();update.reset();}
}

uint64_t StateStore::historyVersion(const std::vector<std::string>& dates) const {
    const auto start=std::chrono::steady_clock::now();std::lock_guard<std::mutex> lock(history_mutex_);const auto wait=queryMicros(start);
    try{const auto version=versionAt(history_db_,dates);version_metrics_.record(queryMicros(start),wait);return version;}
    catch(...){version_metrics_.record(queryMicros(start),wait,0,true);throw;}
}

HistorySnapshot StateStore::historySnapshot(const std::string& batch,const std::vector<std::string>& dates,
        const std::string& stage,const std::string& target,const std::vector<std::string>& revisions,
        bool scheduled_only,size_t max_rows) const {
    const auto start=std::chrono::steady_clock::now();std::unique_lock<std::mutex> lock(history_mutex_);
    const auto wait=queryMicros(start);size_t count=0,estimated_bytes=0;bool transaction=false;
    auto execute=[this](const char* sql){check(sqlite3_exec(history_db_,sql,nullptr,nullptr,nullptr),history_db_);};
    try {
        execute("BEGIN");transaction=true;HistorySnapshot result;result.generation=versionAt(history_db_,scheduled_only?dates:std::vector<std::string>{});
        if(scheduled_only){
            std::string sql="SELECT DISTINCT template FROM task_query_index WHERE classified=1 AND task_id NOT IN (SELECT task_id FROM deleted_tasks)";
            if(!dates.empty()){sql+=" AND day IN (";for(size_t i=0;i<dates.size();++i)sql+=i?",?":"?";sql+=")";}
            Statement names(history_db_,sql.c_str());for(size_t i=0;i<dates.size();++i)names.text(int(i+1),dates[i]);
            for(;;){auto step=sqlite3_step(names.value);if(step==SQLITE_DONE)break;check(step,history_db_);result.templates.insert(column(names.value,0));}
            Statement excluded(history_db_,"SELECT COUNT(*) FROM task_query_index WHERE classified=0 AND has_case=1 AND task_id NOT IN (SELECT task_id FROM deleted_tasks)");
            check(sqlite3_step(excluded.value),history_db_);result.excluded=static_cast<uint64_t>(sqlite3_column_int64(excluded.value,0));
        }
        std::string sql="SELECT t.task_id,t.task_type,t.target,t.revision,t.priority,t.status,t.payload_json,t.execution_json,COALESCE(t.worker_id,''),t.retry_count,t.queue_sequence,COALESCE(t.run_id,''),t.recovery_required,t.created_at_ms,COALESCE(t.started_at_ms,0),COALESCE(t.finished_at_ms,0) FROM tasks t WHERE t.task_id NOT IN (SELECT task_id FROM deleted_tasks)";
        std::vector<std::string> parameters;
        if(!batch.empty()){sql+=" AND t.batch_id=?";parameters.push_back(batch);}
        if(!target.empty()){sql+=" AND t.target=?";parameters.push_back(target);}
        if(!revisions.empty()){sql+=" AND t.revision IN (";for(size_t i=0;i<revisions.size();++i){sql+=i?",?":"?";parameters.push_back(revisions[i]);}sql+=")";}
        if(!dates.empty()||!stage.empty()||scheduled_only){
            sql+=" AND t.task_id IN (SELECT task_id FROM task_query_index WHERE 1=1";
            if(scheduled_only)sql+=" AND classified=1";
            if(!dates.empty()){sql+=" AND day IN (";for(size_t i=0;i<dates.size();++i){sql+=i?",?":"?";parameters.push_back(dates[i]);}sql+=")";}
            if(!stage.empty()){sql+=" AND template=?";parameters.push_back(stage);}sql+=")";
        }
        sql+=" ORDER BY t.task_id";Statement rows(history_db_,sql.c_str());
        for(size_t i=0;i<parameters.size();++i)rows.text(int(i+1),parameters[i]);
        for(;;){auto step=sqlite3_step(rows.value);if(step==SQLITE_DONE)break;check(step,history_db_);
            if(count>=max_rows)throw std::invalid_argument("history query too broad; narrow dates, stage or target");
            size_t raw=0;for(int column=0;column<16;++column)raw+=static_cast<size_t>(sqlite3_column_bytes(rows.value,column));
            estimated_bytes+=raw*4+512;
            if(estimated_bytes>128*1024*1024)throw std::invalid_argument("history snapshot exceeds memory budget; narrow dates, stage or target");
            result.tasks.push_back(historyRow(rows.value));++count;}
        execute("COMMIT");transaction=false;history_metrics_.record(queryMicros(start),wait,count);return result;
    }catch(...){if(transaction)sqlite3_exec(history_db_,"ROLLBACK",nullptr,nullptr,nullptr);history_metrics_.record(queryMicros(start),wait,count,true);throw;}
}
void StateStore::saveWorkerControls(const nlohmann::json& controls) {
    const auto serialized=controls.dump();
    std::lock_guard<std::mutex> lock(mutex_);
    Statement update(db_, "INSERT OR REPLACE INTO worker_management(name,value) VALUES('controls',?)");
    update.text(1,serialized);update.run();
}
} // namespace GCRP
