#include <iostream>
#include "Helper.hpp"

LogStream logStream;

int RecvFull(int socket, void* buffer, size_t size)
{
    size_t nTotalBytesRecv = 0;
    while (nTotalBytesRecv < size)
    {
        int32_t nBytesRecv = recv(socket, (char*)buffer + nTotalBytesRecv, size - nTotalBytesRecv, 0);
        if (nBytesRecv <= 0) {
            std::cerr << "Failed to receive data" << std::endl;
            return nBytesRecv;
        }

        nTotalBytesRecv += nBytesRecv;
    }

    return nTotalBytesRecv;
}

int SendFull(int socket, const void* buffer, size_t size)
{
    size_t nTotalBytesSent = 0;
    while (nTotalBytesSent < size)
    {
        int32_t nBytesSent = send(socket, (const char*)buffer + nTotalBytesSent, size - nTotalBytesSent, 0);
        if (nBytesSent <= 0) {
            return nBytesSent;
        }

        nTotalBytesSent += nBytesSent;
    }

    return nTotalBytesSent;
}
