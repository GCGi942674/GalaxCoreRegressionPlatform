#ifndef BUFFER_H_
#define BUFFER_H_

#include <cstddef>
#include <string>
#include <vector>

class Buffer {
public:
    explicit Buffer(size_t initialSize = 1024);
    ~Buffer();

    size_t readableBytes() const;
    size_t writableBytes() const;

    const char* peek() const;
    char* beginWrite();

    void retrieve(size_t len);
    void retrieveAll();

    std::string retrieveAsString(size_t len);
    std::string retrieveAllAsString();

    void append(const char* data, size_t len);
    void append(const std::string& data);

    void ensureWritableBytes(size_t len);

private:
    char* begin();
    const char* begin() const;
    void makeSpace(size_t len);

private:
    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};

#endif