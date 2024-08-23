#include <iostream>
#include <thread>
#include <cstdint>
#include <atomic>
#include <ctime>
#include <vector>
#include <cassert>
#include <condition_variable>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h> 
#include <cstring>
#include <cstdlib>

#include <sys/ioctl.h>
#include <termios.h>

#define NUM_SESSION 200
#define SESSION_TIMEOUT 20

int main() 
{
    srand(time(nullptr));

    class Session {
    public:
        int udpSock;
    };
    std::vector<Session> sessions(NUM_SESSION);

    int tcpSock;

    // Connect TCP 127.0.0.1:9180
    tcpSock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpSock == -1) {
        std::cerr << "Failed to create client socket" << std::endl;
        return -1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9180);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(tcpSock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        std::cerr << "Failed to connect to server" << std::endl;
        close(tcpSock);
        return -1;
    }

    for (int sessionIdx = 0; sessionIdx < NUM_SESSION; sessionIdx++)
    {
        Session& session = sessions[sessionIdx];

        // Open UDP receiving socket
        session.udpSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (session.udpSock == -1) {
            std::cerr << "Failed to create UDP socket" << std::endl;
            return -1;
        }

        sockaddr_in addr_ObjectPos_Stream;
        memset(&addr_ObjectPos_Stream, 0, sizeof(addr_ObjectPos_Stream));
        addr_ObjectPos_Stream.sin_family = AF_INET;
        addr_ObjectPos_Stream.sin_port = 0;
        addr_ObjectPos_Stream.sin_addr.s_addr = INADDR_ANY;

        if (bind(session.udpSock, (struct sockaddr*)&addr_ObjectPos_Stream, sizeof(addr_ObjectPos_Stream)) == -1) {
            std::cerr << "Failed to bind UDP socket" << std::endl;
            close(session.udpSock);
            return -1;
        }

        // Send API Query CreateSession_v1
        struct __attribute__((packed)) CreateSession_Param
        {
            uint32_t QueryID;
            uint32_t FieldWidth;
            uint32_t FieldHeight;
            uint32_t WinScore;
            uint32_t GameTime;
            uint32_t BallSpeed;
            uint32_t BallRadius;
            uint32_t PaddleSpeed;
            uint32_t PaddleSize;
            uint32_t PaddleOffsetFromWall;
            uint16_t UdpPort_Recv_Stream;
        } param;
        param.QueryID = 101;
        param.FieldWidth = 800;
        param.FieldHeight = 400;
        param.WinScore = 5;
        param.GameTime = SESSION_TIMEOUT;
        param.BallSpeed = 200;
        param.BallRadius = 30;
        param.PaddleSpeed = 600;
        param.PaddleSize = 200;
        param.PaddleOffsetFromWall = 100;
        sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        if (getsockname(session.udpSock, (struct sockaddr*)&addr, &addrLen) == -1) {
            std::cerr << "Failed to get socket name" << std::endl;
            close(tcpSock);
            return -1;
        }
        param.UdpPort_Recv_Stream = addr.sin_port;
        //param.UdpPort_Recv_Stream = 9981;

        if (send(tcpSock, &param, sizeof(param), 0) == -1) {
            std::cerr << "Failed to send API query" << std::endl;
            close(tcpSock);
            return -1;
        }

        // Receive response
        struct __attribute__((packed)) CreateSession_Response
        {
            uint32_t QueryID;
            uint8_t Result;
            uint32_t SessionID;
        } response;

        if (recv(tcpSock, &response, sizeof(response), 0) == -1) {
            std::cerr << "Failed to receive API response" << std::endl;
            close(tcpSock);
            return -1;
        }
        assert(response.QueryID == 101);

        if (response.Result != 0) {
            std::cerr << "Failed to create session" << std::endl;
            close(tcpSock);
            return -1;
        }

        std::cout << "Session ID: " << response.SessionID << std::endl;


        // Send API Query BeginRound_v1
        struct __attribute__((packed)) BeginRound_Param
        {
            uint32_t QueryID;
            uint32_t SessionID;
        } param2;
        param2.QueryID = 201;
        param2.SessionID = response.SessionID;

        if (send(tcpSock, &param2, sizeof(param2), 0) == -1) {
            std::cerr << "Failed to send API query" << std::endl;
            close(tcpSock);
            return -1;
        }

        // Receive response
        struct __attribute__((packed)) BeginRound_Response
        {
            uint32_t QueryID;
            uint8_t Result;
        } response2;
        
        if (recv(tcpSock, &response2, sizeof(response2), 0) == -1) {
            std::cerr << "Failed to receive API response" << std::endl;
            close(tcpSock);
            return -1;
        }
        assert(response2.QueryID == 201);

        if (response2.Result != 0) {
            std::cerr << "Failed to begin round" << std::endl;
            close(tcpSock);
            return -1;
        }
    }

    std::cout << "begin" << std::endl;   

    sleep(SESSION_TIMEOUT + 1);
    for (int sessionIdx = 0; sessionIdx < NUM_SESSION; sessionIdx++)
    {
        close(sessions[sessionIdx].udpSock);
    }
    close(tcpSock);

    std::cout << "end" << std::endl;

    return 0;
}
