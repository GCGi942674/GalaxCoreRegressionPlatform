#ifndef MESSAGE_H
#define MESSAGE_H

#include <string>

class Message {
public:
    virtual std::string onMessage(const std::string& msg) = 0;
    virtual ~Message() = default;
};

#endif