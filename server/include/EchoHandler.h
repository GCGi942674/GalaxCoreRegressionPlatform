#ifndef ECHOHANDLE_H
#define ECHOHANDLE_H

#include "Message.h"

class EchoHandler : public Message {
public:
    virtual std::string onMessage(const std::string& msg) override;
};

#endif