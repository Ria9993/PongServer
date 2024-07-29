#include <iostream>
#include <thread>
#include <cstdint>
#include <atomic>
#include <ctime>
#include <vector>
#include <cassert>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h> 

#include "config.hpp"
#include "math.h"
#include "Helper.hpp"
#include "Session.hpp"

bool g_bSessionWorkerThreadRunning = false;
bool g_bSessionWorkerThreadJoin = false;

int main() 
{
    // notify to docker
    std::cout << "Server Started!\n";

    srand(time(nullptr));

    /* -------------------------------------------------------------------------- */
    /*                            Session / Thread Pool                           */
    /* -------------------------------------------------------------------------- */
    /** 
     * Only main thread has permission to modify the Session array value and sessionCount, sessionWorkerAvailableCount.
     * Therefore, no need to use atomic operation to access.
     * */
    Session* sessions[MAX_SESSION] = { nullptr };
    size_t sessionCount = 0;

    // Init session worker thread pool
    std::thread sessionWorkerThreads[NUM_SESSION_WORKER_THREAD];
    std::atomic<size_t> nextWorkSessionIndex(0);    //< Check session size boundary before access
    std::condition_variable sessionWorker_CvRunning; //< with g_bSessionWorkerThreadRunning
    std::condition_variable seesionWorker_CvStopped;
    std::mutex sessionWorker_MutexRunning;
    std::atomic<size_t> sessionWorkerRealtimeRunningCount(NUM_SESSION_WORKER_THREAD); 
    size_t sessionWorkerAvailableCount = 0; // Wake up worker thread based on session count.
    

    for (size_t i = 0; i < NUM_SESSION_WORKER_THREAD; i++) {
        sessionWorkerThreads[i] = std::thread([&](size_t threadId)
        {
            while (true) 
            {
                // Check running condition
                std::unique_lock<std::mutex> cvLock(sessionWorker_MutexRunning);
                {
                    sessionWorkerRealtimeRunningCount.fetch_sub(1, std::memory_order_acquire);
                    //std::cout << "Session Worker Thread C" << sessionWorkerRealtimeRunningCount << " is stopped\n";  
                    
                    // Notify to main thread if all session worker thread is stopped
                    if (sessionWorkerRealtimeRunningCount.load(std::memory_order_relaxed) == 0 && g_bSessionWorkerThreadRunning == false) {
                        seesionWorker_CvStopped.notify_one();
                    }
                    
                    sessionWorker_CvRunning.wait(cvLock, 
                        [&]() -> bool {
                            return (g_bSessionWorkerThreadRunning && (threadId < sessionWorkerAvailableCount)) | g_bSessionWorkerThreadJoin;
                        });
                    if (g_bSessionWorkerThreadJoin) {
                        cvLock.unlock();
                        return;
                    }

                    sessionWorkerRealtimeRunningCount.fetch_add(1, std::memory_order_relaxed);
                    //std::cout << "Session Worker Thread " << sessionWorkerRealtimeRunningCount << " is stopped\n";
                }
                cvLock.unlock();

                // Find session to work
                const size_t sessionIdx = nextWorkSessionIndex.fetch_add(1, std::memory_order_acquire);
                if (sessionIdx >= sessionCount)
                {
                    // Safe modulo
                    size_t nextSessionIdx = sessionIdx + 1;
                    nextWorkSessionIndex.compare_exchange_weak(nextSessionIdx, 0, std::memory_order_acquire, std::memory_order_acquire);
                    std::atomic_thread_fence(std::memory_order_release);
                    continue;
                }

                // Get exclusive access to session
                Session* session = sessions[sessionIdx];
                assert(session != nullptr);
                
                if (session->bThreadWorkerRunning.exchange(true, std::memory_order_acquire)) {
                    continue;
                }
                {
                    // Check if session is already updated in this tick
                    const std::chrono::milliseconds tickDuration(1000 / SERVER_TICK_RATE);
                    const std::chrono::system_clock::time_point nowTime = std::chrono::system_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - session->GetLastTickUpdateTime()) < tickDuration) {
                        session->bThreadWorkerRunning.store(false, std::memory_order_relaxed);
                        continue;
                    }

                    // Log Latency(us)
                    std::chrono::microseconds latency = std::chrono::duration_cast<std::chrono::microseconds>(nowTime - session->GetLastTickUpdateTime() - tickDuration);
                    std::cout << "[DEBUG] [Thread " << threadId << "] Start work on session #" << session->GetSessionID() << ". Latency: " << latency.count() << "us" << std::endl;
                    
                    // Update session
                    session->Update();

                    // Send session state to client
                    session->SendObjectState();
                }
                session->bThreadWorkerRunning.store(false, std::memory_order_release);
            }
        }, i); //< threadId
    }

    /* -------------------------------------------------------------------------- */
    /*                                 Socket Init                                */
    /* -------------------------------------------------------------------------- */
    // Init listen socket
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    // Set socket option to reuse address
#ifdef __linux__
    int opt = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        std::cerr << "Failed to set socket option" << std::endl;
        close(serverSocket);
        return 1;
    }
#endif

    sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) == -1) {
        std::cerr << "Failed to bind socket to address" << std::endl;
        close(serverSocket);
        return 1;
    }

    if (listen(serverSocket, 1) == -1) {
        std::cerr << "Failed to listen for connections" << std::endl;
        close(serverSocket);
        return 1;
    }


    /* -------------------------------------------------------------------------- */
    /*                                 Server Loop                                */
    /* -------------------------------------------------------------------------- */
    while (true)
    {
        // Generate session id pool for unique session id
        uint32_t sessionIdPoolTop = 0;
        uint32_t sessionIdPool[MAX_SESSION];
        for (int i = 0; i < MAX_SESSION; i++) {
            sessionIdPool[i] = i;
        }

        // Accept client server
        sockaddr_in clientAddress;
        socklen_t clientAddressSize = sizeof(clientAddress);
        int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientAddressSize);
        if (clientSocket == -1) {
            std::cerr << "Failed to accept connection" << std::endl;
            close(serverSocket);
            return 1;
        }

        // Mark session worker thread is running
        g_bSessionWorkerThreadRunning = true;
        sessionWorkerAvailableCount = 0;

        // Client loop
        while (true)
        {
        
            // Handle API Query
            uint32_t queryID;
            if (RecvFull(clientSocket, &queryID, sizeof(queryID)) <= 0) {
                break;
            }

            switch (queryID)
            {
            // CreateSession_v1
            case 101:
            {
                struct __attribute__((packed)) CreateSession_Param
                {
                    uint32_t FieldWidth;
                    uint32_t FieldHeight;
                    uint32_t WinScore;
                    uint32_t GameTime;
                    uint32_t BallSpeed;
                    uint32_t BallRadius;
                    uint32_t PaddleSpeed;
                    uint32_t PaddleSize;
                    uint32_t PaddleOffsetFromWall;
                    uint16_t RecvPort_ObjectPos_Stream;
                } param;

                struct __attribute__((packed)) CreateSession_Fail_Response
                {
                    uint32_t QueryID = 101;
                    uint8_t Result = 1;
                } fail_response;

                struct __attribute__((packed)) CreateSession_Response
                {
                    uint32_t QueryID = 101;
                    uint8_t Result;
                    uint32_t SessionID;
                } response;

                const int nBytesRecv = RecvFull(clientSocket, &param, sizeof(param));
                if (nBytesRecv <= 0) {
                    goto CLOSE_CLIENT_CONNECTION;
                }
                std::cout << "[DEBUG] CreateSession_v1: " << param.FieldWidth << ", " << param.FieldHeight << ", " << param.WinScore << ", " << param.GameTime << ", " << param.BallSpeed << ", " << param.BallRadius << ", " << param.PaddleSpeed << ", " << param.PaddleSize << ", " << param.PaddleOffsetFromWall << ", " << param.RecvPort_ObjectPos_Stream << std::endl;


                if (sessionCount == MAX_SESSION) {
                    SendFull(clientSocket, &fail_response, sizeof(fail_response));
                    break;
                }

                // Create a new session with unique session ID
                if (sessionIdPoolTop == MAX_SESSION) {
                    SendFull(clientSocket, &fail_response, sizeof(fail_response));
                    break;
                }
                const uint32_t sessionID = sessionIdPool[sessionIdPoolTop++];

                // Open UDP socket for object position stream
                int udpSocket_ObjectPos_Stream = socket(AF_INET, SOCK_DGRAM, 0);
                if (udpSocket_ObjectPos_Stream == -1) {
                    std::cerr << "Failed to create UDP socket" << std::endl;
                    SendFull(clientSocket, &fail_response, sizeof(fail_response));
                    break;
                }

                sessions[sessionCount] = new Session(clientSocket,
                                                     sessionID,
                                                     param.FieldWidth,
                                                     param.FieldHeight,
                                                     param.WinScore,
                                                     param.GameTime,
                                                     param.BallSpeed,
                                                     param.BallRadius,
                                                     param.PaddleSpeed,
                                                     param.PaddleSize,
                                                     param.PaddleOffsetFromWall,
                                                     udpSocket_ObjectPos_Stream,
                                                     clientAddress,
                                                     param.RecvPort_ObjectPos_Stream);
                // sessionCount must be updated after session is updated in sessions array
                std::atomic_thread_fence(std::memory_order_release);
                sessionCount += 1;

                // Wake up worker thread based on session count
                if (sessionWorkerAvailableCount < sessionCount && sessionWorkerAvailableCount < NUM_SESSION_WORKER_THREAD) {
                    sessionWorkerAvailableCount += 1;
                    std::cout << "[DEBUG] sessionAvailable :" << sessionWorkerAvailableCount <<  std::endl;
                }
                sessionWorker_CvRunning.notify_one();

                std::cout << "[DEBUG] Session Created: " << sessionID << std::endl;

                response.Result = 0;
                response.SessionID = sessionID;
                SendFull(clientSocket, &response, sizeof(response));
                break;
            }
            
            // BeginRound_v1
            case 201:
            {
                struct __attribute__((packed)) BeginRound_Param
                {
                    uint32_t SessionID;
                } param;

                struct __attribute__((packed)) BeginRound_Response
                {
                    uint32_t QueryID = 201;
                    uint8_t Result;
                } response;

                const int nBytesRecv = RecvFull(clientSocket, &param, sizeof(param));
                if (nBytesRecv <= 0) {
                    goto CLOSE_CLIENT_CONNECTION;
                }
                std::cout << "[DEBUG] BeginRound_v1: " << param.SessionID << std::endl;


                size_t i;
                for (i = 0; i < sessionCount; i++) {
                    if (sessions[i]->GetSessionID() == param.SessionID) {
                        if (sessions[i]->BeginRound()) {
                            response.Result = 0;
                            SendFull(clientSocket, &response, sizeof(response));
                        }
                        else {
                            response.Result = 1;
                            SendFull(clientSocket, &response, sizeof(response));
                        }
                        break;
                    }
                }

                // Session not found
                if (i == sessionCount) {
                    response.Result = 1;
                    SendFull(clientSocket, &response, sizeof(response));
                }
                
                break;
            }

            // ActionPlayerInput_v1
            case 301:
            {
                struct __attribute__((packed)) ActionPlayerInput_Param
                {
                    uint32_t SessionID;
                    uint32_t PlayerID;
                    uint8_t InputKey;
                    uint8_t InputType;
                } param;

                struct __attribute__((packed)) ActionPlayerInput_Response
                {
                    uint32_t QueryID = 301;
                    uint8_t Result;
                } response;

                const int nBytesRecv = RecvFull(clientSocket, &param, sizeof(param));
                if (nBytesRecv <= 0) {
                    goto CLOSE_CLIENT_CONNECTION;
                }
                std::cout << "[DEBUG] ActionPlayerInput_v1: " << param.SessionID << ", " << param.PlayerID << ", " << param.InputKey << ", " << param.InputType << std::endl;


                // Find session
                size_t findSessionIdx = MAX_SESSION;
                for (size_t i = 0; i < sessionCount; i++) {
                    if (sessions[i]->GetSessionID() == param.SessionID) {  
                        findSessionIdx = i;
                        break;
                    }
                }

                // Session not found
                if (findSessionIdx == MAX_SESSION) {
                    response.Result = 1;
                    SendFull(clientSocket, &response, sizeof(response));
                }

                Session::PlayerID playerID;
                if (param.PlayerID == 1) {
                    playerID = Session::PlayerID::PlayerA;
                }
                else if (param.PlayerID == 2) {
                    playerID = Session::PlayerID::PlayerB;
                }
                else {
                    response.Result = 1;
                    SendFull(clientSocket, &response, sizeof(response));
                    break;
                }

                Session::InputKey inputKey;
                if (param.InputKey == 1) {
                    inputKey = Session::InputKey::Left;
                }
                else if (param.InputKey == 2) {
                    inputKey = Session::InputKey::Right;
                }
                else {
                    response.Result = 1;
                    SendFull(clientSocket, &response, sizeof(response));
                    break;
                }

                Session::InputType inputType;
                if (param.InputType == 0) {
                    inputType = Session::InputType::None;
                }
                else if (param.InputType == 1) {
                    inputType = Session::InputType::Press;
                }
                else if (param.InputType == 2) {
                    inputType = Session::InputType::Release;
                }
                else {
                    response.Result = 1;
                    SendFull(clientSocket, &response, sizeof(response));
                    break;
                }

                assert(sessions[findSessionIdx] != nullptr);
                sessions[findSessionIdx]->SetPlayerInput(playerID, inputKey, inputType);

                response.Result = 0;
                SendFull(clientSocket, &response, sizeof(response));
                break;
            }

            // Unknown Query ID
            default:
            {
                std::cerr << "Unknown Query ID: " << queryID << std::endl;

                struct __attribute__((packed)) UnknownQueryID_Response
                {
                    uint32_t QueryID;
                    uint8_t Result = 1;
                } response;
                response.QueryID = queryID;
                response.Result = 1;
                SendFull(clientSocket, &response, sizeof(response));
                break;
            }
            }
        }


    CLOSE_CLIENT_CONNECTION:
        /* ---------------------------- Cleanup Resources --------------------------- */
        // Wait until all session worker thread is stopped
        g_bSessionWorkerThreadRunning = false;
        sessionWorkerAvailableCount = 0;
        sessionWorker_CvRunning.notify_all();
        std::unique_lock<std::mutex> cvLock(sessionWorker_MutexRunning);
        seesionWorker_CvStopped.wait(cvLock, [&]() -> bool { return sessionWorkerRealtimeRunningCount == 0; });
        {
            // Close all session
            sessionCount = 0;
            for (size_t i = 0; i < MAX_SESSION; i++) {
                delete sessions[i];
                sessions[i] = nullptr;
            }

            // Close client socket
            close(clientSocket);
        }
        cvLock.unlock();
    }


    // Cleanup threads and resources
    g_bSessionWorkerThreadJoin = true;
    sessionWorker_CvRunning.notify_all();
    for (int i = 0; i < NUM_SESSION_WORKER_THREAD; i++) {
        sessionWorkerThreads[i].join();
    }

    close(serverSocket);

    return 0;
}
