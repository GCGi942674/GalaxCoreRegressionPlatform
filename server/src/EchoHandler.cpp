#include "EchoHandler.h"
#include "Logging.h"

using namespace GCRP;

std::string EchoHandler::onMessage(const std::string& msg) {
    if (msg == "__ping__") {
        LOG_DEBUG(LogModule::NETWORK, "heartbeat ping received");
        return "__pong__";
    }
    return msg;
}
