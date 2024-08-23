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

bool kbhit()
{
    termios term;
    tcgetattr(0, &term);

    termios term2 = term;
    term2.c_lflag &= ~ICANON;
    tcsetattr(0, TCSANOW, &term2);

    int byteswaiting;
    ioctl(0, FIONREAD, &byteswaiting);

    tcsetattr(0, TCSANOW, &term);

    return byteswaiting > 0;
}

/* reads from keypress, doesn't echo */
int getch(void)
{
    struct termios oldattr, newattr;
    int ch;
    tcgetattr( STDIN_FILENO, &oldattr );
    newattr = oldattr;
    newattr.c_lflag &= ~( ICANON | ECHO );
    tcsetattr( STDIN_FILENO, TCSANOW, &newattr );
    ch = getchar();
    tcsetattr( STDIN_FILENO, TCSANOW, &oldattr );
    return ch;
}

bool keyState(char key)
{
    return kbhit() && (getch() == key);
}


volatile bool g_bSessionWorkerThreadRunning = true;

int main() 
{
    srand(time(nullptr));

    // Open UDP receiving socket
    int udpSocket_ObjectPos_Stream = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSocket_ObjectPos_Stream == -1) {
        std::cerr << "Failed to create UDP socket" << std::endl;
        return -1;
    }

    sockaddr_in addr_ObjectPos_Stream;
    memset(&addr_ObjectPos_Stream, 0, sizeof(addr_ObjectPos_Stream));
    addr_ObjectPos_Stream.sin_family = AF_INET;
    addr_ObjectPos_Stream.sin_port = 0;
    addr_ObjectPos_Stream.sin_addr.s_addr = INADDR_ANY;

    if (bind(udpSocket_ObjectPos_Stream, (struct sockaddr*)&addr_ObjectPos_Stream, sizeof(addr_ObjectPos_Stream)) == -1) {
        std::cerr << "Failed to bind UDP socket" << std::endl;
        close(udpSocket_ObjectPos_Stream);
        return -1;
    }

    // Connect TCP 127.0.0.1:9180
    int clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == -1) {
        std::cerr << "Failed to create client socket" << std::endl;
        return -1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9180);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        std::cerr << "Failed to connect to server" << std::endl;
        close(clientSocket);
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
    param.GameTime = 20;
    param.BallSpeed = 200;
    param.BallRadius = 30;
    param.PaddleSpeed = 600;
    param.PaddleSize = 200;
    param.PaddleOffsetFromWall = 100;
    sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    if (getsockname(udpSocket_ObjectPos_Stream, (struct sockaddr*)&addr, &addrLen) == -1) {
        std::cerr << "Failed to get socket name" << std::endl;
        close(clientSocket);
        return -1;
    }
    param.UdpPort_Recv_Stream = addr.sin_port;
    //param.UdpPort_Recv_Stream = 9981;

    if (send(clientSocket, &param, sizeof(param), 0) == -1) {
        std::cerr << "Failed to send API query" << std::endl;
        close(clientSocket);
        return -1;
    }

    // Receive response
    struct __attribute__((packed)) CreateSession_Response
    {
        uint32_t QueryID;
        uint8_t Result;
        uint32_t SessionID;
    } response;

    if (recv(clientSocket, &response, sizeof(response), 0) == -1) {
        std::cerr << "Failed to receive API response" << std::endl;
        close(clientSocket);
        return -1;
    }
    assert(response.QueryID == 101);

    if (response.Result != 0) {
        std::cerr << "Failed to create session" << std::endl;
        close(clientSocket);
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

    if (send(clientSocket, &param2, sizeof(param2), 0) == -1) {
        std::cerr << "Failed to send API query" << std::endl;
        close(clientSocket);
        return -1;
    }

    // Receive response
    struct __attribute__((packed)) BeginRound_Response
    {
        uint32_t QueryID;
        uint8_t Result;
    } response2;
    
    if (recv(clientSocket, &response2, sizeof(response2), 0) == -1) {
        std::cerr << "Failed to receive API response" << std::endl;
        close(clientSocket);
        return -1;
    }
    assert(response2.QueryID == 201);

    if (response2.Result != 0) {
        std::cerr << "Failed to begin round" << std::endl;
        close(clientSocket);
        return -1;
    }

    // Listen for object position stream
    std::thread sessionWorkerThread([&](){
        while (g_bSessionWorkerThreadRunning) {
            
            // Send sample input
            static uint32_t tick = 0;
            tick += 1;
            std::cout << "Tick: " << tick << std::endl;
            struct __attribute__((packed)) Input
            {
                uint32_t QueryID;
                uint32_t SessionID;
                uint32_t PlayerID;
                uint8_t Key;
                uint8_t KeyType;
            } input;
            input.QueryID = 301;
            input.SessionID = response.SessionID;

            static bool last_d = false;
            static bool last_f = false;
            static bool last_j = false;
            static bool last_k = false;

            bool now_d = false;
            bool now_f = false;
            bool now_j = false;
            bool now_k = false;

            std::cout << "\033[NA";
            if (kbhit()) {
                char key = getch();
                if (key == 'd') {
                    now_d = true;
                }
                else if (key == 'f') {
                    now_f = true;
                }
                else if (key == 'j') {
                    now_j = true;
                }
                else if (key == 'k') {
                    now_k = true;
                }
            }

            if (now_d && !last_d) {
                input.PlayerID = 1;
                input.Key = 1;
                input.KeyType = 1;
                last_d = true;
                send(clientSocket, &input, sizeof(input), 0);
            }
            else if (!now_d && last_d) {
                input.PlayerID = 1;
                input.Key = 1;
                input.KeyType = 2;
                last_d = false;
                send(clientSocket, &input, sizeof(input), 0);
            }
            if (now_f && !last_f) {
                input.PlayerID = 1;
                input.Key = 2;
                input.KeyType = 1;
                last_f = true;
                send(clientSocket, &input, sizeof(input), 0);
            }
            else if (!now_f && last_f) {
                input.PlayerID = 1;
                input.Key = 2;
                input.KeyType = 2;
                last_f = false;
                send(clientSocket, &input, sizeof(input), 0);
            }
            if (now_j && !last_j) {
                input.PlayerID = 2;
                input.Key = 1;
                input.KeyType = 1;
                last_j = true;
                send(clientSocket, &input, sizeof(input), 0);
            }
            else if (!now_j && last_j) {
                input.PlayerID = 2;
                input.Key = 1;
                input.KeyType = 2;
                last_j = false;
                send(clientSocket, &input, sizeof(input), 0);
            }
            if (now_k && !last_k) {
                input.PlayerID = 2;
                input.Key = 2;
                input.KeyType = 1;
                last_k = true;
                send(clientSocket, &input, sizeof(input), 0);
            }
            else if (!now_k && last_k) {
                input.PlayerID = 2;
                input.Key = 2;
                input.KeyType = 2;
                last_k = false;
                send(clientSocket, &input, sizeof(input), 0);
            }


        END_KEY_INPUT:
            
            // Recv RDP pos
            struct __attribute__((packed)) ObjectPos
            {
                float BallX;
                float BallY;
                float PaddleA;
                float PaddleB;
            } objectPos;

            struct sockaddr_in addr;
            socklen_t addrLen = sizeof(addr);

            if (g_bSessionWorkerThreadRunning) {
                if (recvfrom(udpSocket_ObjectPos_Stream, &objectPos, sizeof(objectPos), 0, (struct sockaddr*)&addr, &addrLen) == -1) {
                    std::cerr << "Failed to receive object position stream" << std::endl;
                    break;
                }
            }

            std::cout << objectPos.BallX << ", " << objectPos.BallY << std::endl;
            std::cout << objectPos.PaddleA << ", " << objectPos.PaddleB << std::endl;

            // Visualize object position
            const int visualWidth = 80;
            const int visualHeight = 20;
            int visualBallX = objectPos.BallX / 800.0f * visualWidth;
            int visualBallY = objectPos.BallY / 400.0f * visualHeight;

            char visualMap[visualHeight + 2][visualWidth + 2]; 
            memset(visualMap, ' ', sizeof(visualMap));

            for (int y = 0; y < visualHeight + 1; ++y) {
                visualMap[y][0] = '|';
                visualMap[y][visualWidth + 1] = '|';
            }

            for (int x = 0; x < visualWidth + 1; ++x) {
                visualMap[0][x] = '-';
                visualMap[visualHeight + 1][x] = '-';
            }

            visualMap[0][0] = '+';
            visualMap[0][visualWidth + 1] = '+';
            visualMap[visualHeight + 1][0] = '+';
            visualMap[visualHeight + 1][visualWidth + 1] = '+';

            visualMap[visualBallY + 1][visualBallX + 1] = 'O';

            // Paddle Position calculation
            struct vec2
            {
                float x;
                float y;
            };
            const vec2 paddleA_BaseAbsPos = { (float)param.PaddleOffsetFromWall, param.FieldHeight / 2.0f };
            const vec2 paddleB_BaseAbsPos = { param.FieldWidth - (float)param.PaddleOffsetFromWall, param.FieldHeight / 2.0f };

            vec2 paddleA_AbsPos;
            paddleA_AbsPos.x = paddleA_BaseAbsPos.x;
            paddleA_AbsPos.y = paddleA_BaseAbsPos.y - objectPos.PaddleA;

            vec2 paddleB_AbsPos;
            paddleB_AbsPos.x = paddleB_BaseAbsPos.x;
            paddleB_AbsPos.y = paddleB_BaseAbsPos.y + objectPos.PaddleB;
            
            vec2 visualPaddleA = { paddleA_AbsPos.x / 800.0f * visualWidth, paddleA_AbsPos.y / 400.0f * visualHeight };
            vec2 visualPaddleB = { paddleB_AbsPos.x / 800.0f * visualWidth, paddleB_AbsPos.y / 400.0f * visualHeight };
            int visualPaddleSize = param.PaddleSize / 400.0f * visualHeight;

            for (int y = 0; y < visualPaddleSize; ++y) {
                visualMap[(int)visualPaddleA.y + y - (visualPaddleSize / 2)][(int)visualPaddleA.x] = 'A';
                visualMap[(int)visualPaddleB.y + y - (visualPaddleSize / 2)][(int)visualPaddleB.x] = 'B';
            }
        
            for (int y = 0; y < visualHeight + 2; ++y) {
                for (int x = 0; x < visualWidth + 2; ++x) {
                    std::cout << visualMap[y][x];
                }
                std::cout << std::endl;
            }
        }
    });
    
    while (true)
    {
        // Wait until round ending response is received
        uint32_t responceQueryID = 0;
        if (recv(clientSocket, &responceQueryID, sizeof(responceQueryID), 0) == -1) {
            std::cerr << "Failed to receive API response " << errno << std::endl;
            close(clientSocket);
            return -1;
        }

        //std::cout << "Received response query ID: " << responceQueryID << std::endl;
        if (responceQueryID == 301) {
            uint8_t resultDummy;
            if (recv(clientSocket, &resultDummy, sizeof(resultDummy), 0) == -1) {
                std::cerr << "Failed to receive API response " << errno << std::endl;
                close(clientSocket);
                return -1;
            }
            continue;
        }

        struct __attribute__((packed)) BeginRound_Response
        {
            uint32_t WinPlayer;
        } response3;

        if (recv(clientSocket, &response3, sizeof(response3), 0) == -1) {
            std::cerr << "Failed to receive API response " << errno << std::endl;
            close(clientSocket);
            return -1;
        }

        std::cout << "Round ended, winner: " << response3.WinPlayer << std::endl;

        g_bSessionWorkerThreadRunning = false;
        exit(0);
        break;
    }
    sessionWorkerThread.join();

    // Close client socket
    close(clientSocket);

    // Wait for session worker thread to finish
    g_bSessionWorkerThreadRunning = false;
    close(udpSocket_ObjectPos_Stream);


    return 0;
}
