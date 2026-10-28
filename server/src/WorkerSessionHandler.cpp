#include "WorkerSessionHandler.h"
#include "worker/NetworkWorkerDispatcher.h"
#include "Logging.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"

using namespace GCRP::Protocol;

namespace GCRP {

WorkerSessionHandler::WorkerSessionHandler(WorkerManager& worker_manager,
    WorkerConnectionRegistry& registry,
    Scheduler& scheduler,
    TaskService& task_service,
    SchedulingCoordinator& coordinator,
    Protocol::ProtocolRouter& router)
    : worker_manager_(worker_manager)
    , registry_(registry)
    , dispatcher_(registry)
    , scheduler_(scheduler)
    , task_service_(task_service)
    , coordinator_(coordinator)
    , router_(router) {
    LOG_INFO(LogModule::SERVER, "WorkerSessionHandler initialized");
}

WorkerSessionHandler::~WorkerSessionHandler() {
}

std::string WorkerSessionHandler::onMessage(const std::string& msg) {
    // Legacy handler for Message interface compatibility.
    // Actual worker protocol handling would be done in WorkerSession if directly integrated.

    if (msg == "__ping__") {
        return "__pong__";
    }

    return msg;
}

} // namespace GCRP
