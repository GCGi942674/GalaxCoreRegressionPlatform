#include "Buffer.h"
#include <algorithm>
#include <cstring>

Buffer::Buffer(size_t initialSize)
    : buffer_(initialSize)
    , readerIndex_(0)
    , writerIndex_(0) {
}

Buffer::~Buffer() {
}

size_t Buffer::readableBytes() const {
    return this->writerIndex_ - this->readerIndex_;
}

size_t Buffer::writableBytes() const {
    return this->buffer_.size() - this->writerIndex_;
}

const char* Buffer::peek() const {
    return this->begin() + this->readerIndex_;
}

char* Buffer::beginWrite() {
    return this->begin() + this->writerIndex_;
}

void Buffer::retrieve(size_t len) {
    if (len < this->readableBytes()) {
        this->readerIndex_ += len;
    } else {
        this->retrieveAll();
    }
}

void Buffer::retrieveAll() {
    this->readerIndex_ = 0;
    this->writerIndex_ = 0;
}

std::string Buffer::retrieveAsString(size_t len) {
    len = std::min(len, this->readableBytes());
    std::string result(peek(), len);
    this->retrieve(len);
    return result;
}

std::string Buffer::retrieveAllAsString() {
    return this->retrieveAsString(this->readableBytes());
}

void Buffer::append(const char* data, size_t len) {
    this->ensureWritableBytes(len);
    std::memcpy(this->beginWrite(), data, len);
    this->writerIndex_ += len;
}

void Buffer::append(const std::string& data) {
    this->append(data.data(), data.size());
}

void Buffer::ensureWritableBytes(size_t len) {
    if (this->writableBytes() < len) {
        this->makeSpace(len);
    }
}

char* Buffer::begin() {
    return this->buffer_.data();
}

const char* Buffer::begin() const {
    return this->buffer_.data();
}

void Buffer::makeSpace(size_t len) {
    if (this->writableBytes() + this->readerIndex_ >= len) {
        size_t readable = this->readableBytes();
        std::memmove(this->begin(), this->begin() + this->readerIndex_, readable);
        this->readerIndex_ = 0;
        this->writerIndex_ = readable;
    } else {
        this->buffer_.resize(this->writerIndex_ + len);
    }
}