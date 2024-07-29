#pragma once

#include <cstdint>
#include <iostream>
#include <condition_variable>
#include <sys/socket.h>
#include <unistd.h>

int RecvFull(int socket, void* buffer, size_t size);

int SendFull(int socket, const void* buffer, size_t size);


// custom logging stream like std::cerr
#include <iostream>
#include <sstream>

class LogStream
{
public:
    LogStream() = default;
    ~LogStream()
    {
        std::cerr << m_Stream.str() << std::endl;
    }

    template <typename T>
    LogStream& operator<<(const T& value)
    {
        m_Stream << value;
        return *this;
    }

    // support for std::endl
    LogStream& operator<<(std::ostream& (*func)(std::ostream&))
    {
        m_Stream << func;
        return *this;
    }

private:
    std::ostringstream m_Stream;
};

extern LogStream logStream;
