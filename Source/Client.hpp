#pragma once

#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h> 

class Session;

struct Client {
    int socket;
    sockaddr_in address;
    socklen_t   addressLen;

    std::vector<Session*> sessions;
    
    // recv/send buffer (for partial recv/send)
    std::vector<char> recvBuffer;
    std::vector<char> sendBuffer;
    
    inline Client()
        : addressLen(sizeof(sockaddr_in))
    {
        recvBuffer.reserve(4096);
        sendBuffer.reserve(4096);
    }

    inline Client(Client&& src)
        : socket(src.socket)
        , address(src.address)
        , addressLen(src.addressLen)
        , sessions(std::move(src.sessions))
        , recvBuffer(std::move(src.recvBuffer))
        , sendBuffer(std::move(src.sendBuffer))
    {
        src.socket = -1;
    }

    inline Client& operator=(Client&& rhs) 
    {
        socket = rhs.socket;
        address = rhs.address;
        addressLen = rhs.addressLen;
        sessions = std::move(rhs.sessions);
        recvBuffer = std::move(rhs.recvBuffer);
        sendBuffer = std::move(rhs.sendBuffer);

        return *this;
    }

    inline ~Client()
    {
        close(socket);
    }
};
