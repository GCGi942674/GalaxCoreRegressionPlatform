#ifndef UTILS_H
#define UTILS_H

#include <cstddef>
#include <fcntl.h>
#include <cstdint>
#include <sys/socket.h>

int setNonBlocking(int fd);
bool sendAll(int fd, const void* data, size_t len);
bool recvAll(int fd, void* data, size_t len);

uint64_t getSteadyClockMs();
uint64_t getSteadyClockUs();

#endif // UTILS_H