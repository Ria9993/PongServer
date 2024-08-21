#include <iostream>
#include <thread>
#include <cstdint>
#include <atomic>
#include <ctime>
#include <vector>
#include <deque>
#include <cassert>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h> 

#include "config.hpp"
#include "math.h"
#include "Helper.hpp"
#include "Session.hpp"

int main() 
{
    // notify to docker
    std::cout << "Server Started!\n";

    srand(time(nullptr));

    /* -------------------------------------------------------------------------- */
    /*                            Session / Thread Pool                           */
    /* -------------------------------------------------------------------------- */
    /** 
     * Only main thread has permission to modify the order of Session vector.
     * */
    std::vector<Session*> sessions;

    // Init session worker thread pool
    std::thread             sessionWorkerThreads[NUM_SESSION_WORKER_THREAD];
    struct TaskQueue
    {
        Session**            Tasks  = nullptr;
        std::atomic<int32_t> Count  = 0;
        size_t               Capacity = 0;
    };

    std::condition_variable sessionWorkerWakeUpCondition;
    std::mutex              sessionWorkerWakeUpMutex;
    TaskQueue               sessionWorkerTaskQueue[NUM_SESSION_WORKER_THREAD];

    std::condition_variable mainThreadWakeUpCondition;
    std::mutex              mainThreadWakeUpMutex;
    std::atomic<size_t>     sessionWorkerAvailableCount(0);

    bool                    bSessionWorkerJoinFlag = false;


    for (size_t i = 0; i < NUM_SESSION_WORKER_THREAD; i++) {
        sessionWorkerThreads[i] = std::thread([&](size_t threadId)
        {
            while (true) 
            {
                TaskQueue& taskQueue = sessionWorkerTaskQueue[threadId];

                // Check if task exist
                std::unique_lock<std::mutex> cvLock(sessionWorkerWakeUpMutex);
                {
                    sessionWorkerWakeUpCondition.wait(cvLock, 
                        [&]() -> bool {
                            return (taskQueue.Count > 0) | bSessionWorkerJoinFlag;
                        });
                    if (bSessionWorkerJoinFlag) {
                        cvLock.unlock();
                        return;
                    }
                }
                cvLock.unlock();

                // Process all tasks in the local task queue
                while (true)
                {
                    // Get exclusive access to session
                    int32_t taskIdx = taskQueue.Count.fetch_sub(1, std::memory_order_acquire) - 1; // TODO: < Is it safe to use acq?
                    if (taskIdx < 0) {
                        break;
                    }
                    Session* session = taskQueue.Tasks[taskIdx];
                    assert(session != nullptr);

                    // Update session
                    session->Update();

                    // Send session state to client
                    session->SendObjectState();
                }

                // Work stealing from other worker
                for (size_t targetThreadId = threadId + 1; targetThreadId != threadId; targetThreadId = (targetThreadId + 1 < NUM_SESSION_WORKER_THREAD) ? targetThreadId + 1 : 0)
                {
                    while (true)
                    {
                        // Get exclusive access to session to steal
                        TaskQueue& targetTaskQueue = sessionWorkerTaskQueue[targetThreadId];
                        int32_t    taskIdx = taskQueue.Count.fetch_sub(1, std::memory_order_acquire) - 1; // TODO: < Is it safe to use acq?
                        if (taskIdx < 0) {
                            break;
                        }
                        Session* session = targetTaskQueue.Tasks[targetTaskQueue.Count];
                        assert(session != nullptr);

                        // Update session
                        session->Update();

                        // Send session state to client
                        session->SendObjectState();
                    }
                }

                // Wake up main thread if all workers are completed
                {
                    std::unique_lock<std::mutex> cvLock(mainThreadWakeUpMutex);
                    sessionWorkerAvailableCount -= 1;
                    mainThreadWakeUpCondition.notify_one();
                }
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

    // Set socket option to non-blocking
#if defined(__linux__)
    int flags = fcntl(serverSocket, F_GETFL, 0);
    fcntl(serverSocket, F_SETFL, flags | O_NONBLOCK);
#elif defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(serverSocket, FIONBIO, &mode);
#elif defined(__APPLE__)
    int flags = fcntl(serverSocket, F_GETFL, 0);
    fcntl(serverSocket, F_SETFL, flags | O_NONBLOCK);
#else
    #error "Unsupported platform"
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

    // Register server socket to select()
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(serverSocket, &readfds);
    int maxFd = serverSocket;


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
        if (select(maxFd + 1, &readfds, NULL, NULL, NULL) == -1) {
            std::cerr << "Failed to select" << std::endl;
            close(serverSocket);
            return 1;
        }
        int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientAddressSize);
        if (clientSocket == -1) {
            std::cerr << "Failed to accept connection" << std::endl;
            close(serverSocket);
            return 1;
        }

        // Intialize recv/send buffer (for partial recv/send)
        std::vector<char> recvBuffer;
        std::vector<char> sendBuffer;
        recvBuffer.reserve(4096);
        sendBuffer.reserve(4096);

        while (true)
        {
            /* ------------------- Send buffered message to the client ------------------- */
            {
                if (!sendBuffer.empty()) {
                    const int nBytesSent = send(clientSocket, sendBuffer.data(), sendBuffer.size(), 0);
                    if (nBytesSent == -1) {
                        std::cout << "[DEBUG] send() == -1. errno: " << errno << std::endl;
                    }
                    else {
                        sendBuffer.erase(sendBuffer.begin(), sendBuffer.begin() + nBytesSent);
                    }
                }
            }

            /* --------------------- Receive message from the client -------------------- */
            {
                char buffer[1024];
                const int nBytesRecv = recv(clientSocket, buffer, sizeof(buffer), 0);
                if (nBytesRecv == -1) {
                    std::cout << "[DEBUG] recv() == -1. errno: " << errno << std::endl;
                }
                else if (nBytesRecv == 0) {
                    std::cout << "Client disconnected" << std::endl;
                    break;
                }
                else {
                    recvBuffer.insert(recvBuffer.end(), buffer, buffer + nBytesRecv);
                }
            }

            /* ---------------------------- Handle API Query ---------------------------- */
            {
                size_t recvBufferOffset = 0;

                uint32_t queryID;
                if (recvBuffer.size() < sizeof(queryID)) {
                    goto CONTINUE_HANDLE_API_QUERY;
                }
                memcpy(&queryID, recvBuffer.data() + recvBufferOffset, sizeof(queryID));
                recvBufferOffset += sizeof(queryID);

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

                    // Receive param
                    if (recvBuffer.size() - recvBufferOffset < sizeof(param)) {
                        goto CONTINUE_HANDLE_API_QUERY;
                    }
                    memcpy(&param, recvBuffer.data() + recvBufferOffset, sizeof(param));
                    recvBufferOffset += sizeof(param);

                    std::cout << "[DEBUG] CreateSession_v1: " << param.FieldWidth << ", " << param.FieldHeight << ", " << param.WinScore << ", " << param.GameTime << ", " << param.BallSpeed << ", " << param.BallRadius << ", " << param.PaddleSpeed << ", " << param.PaddleSize << ", " << param.PaddleOffsetFromWall << ", " << param.RecvPort_ObjectPos_Stream << std::endl;

                    if (sessions.size() == MAX_SESSION) {
                        sendBuffer.insert(sendBuffer.end(), (char*)&fail_response, (char*)&fail_response + sizeof(fail_response));
                        break;
                    }

                    // Create a new session with unique session ID
                    assert (sessionIdPoolTop != MAX_SESSION);
                    const uint32_t sessionID = sessionIdPool[sessionIdPoolTop++];

                    // Open UDP socket for object position stream
                    int udpSocket_ObjectPos_Stream = socket(AF_INET, SOCK_DGRAM, 0);
                    if (udpSocket_ObjectPos_Stream == -1) {
                        std::cerr << "Failed to create UDP socket" << std::endl;
                        sendBuffer.insert(sendBuffer.end(), (char*)&fail_response, (char*)&fail_response + sizeof(fail_response));
                        break;
                    }

                    sessions.push_back(new Session(sessionID,
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
                                                    param.RecvPort_ObjectPos_Stream));

                    std::cout << "[DEBUG] Session Created: " << sessionID << std::endl;

                    response.Result = 0;
                    response.SessionID = sessionID;
                    sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                    break;
                }
                
                // AbortSession_v1
                case 102:
                {
                    struct __attribute__((packed)) AbortSession_Param
                    {
                        uint32_t SessionID;
                    } param;

                    struct __attribute__((packed)) AbortSession_Response
                    {
                        uint32_t QueryID = 102;
                        uint8_t Result;
                    } response;

                    // Receive param
                    if (recvBuffer.size() - recvBufferOffset < sizeof(param)) {
                        goto CONTINUE_HANDLE_API_QUERY;
                    }
                    memcpy(&param, recvBuffer.data() + recvBufferOffset, sizeof(param));
                    recvBufferOffset += sizeof(param);

                    std::cout << "[DEBUG] AbortSession_v1: " << param.SessionID << std::endl;

                    size_t i;
                    for (i = 0; i < sessions.size(); i++) {
                        if (sessions[i]->GetSessionID() == param.SessionID) {
                            delete sessions[i];
                            sessions.erase(sessions.begin() + i);
                            response.Result = 0;
                            sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                            break;
                        }
                    }

                    // Session not found
                    if (i == sessions.size()) {
                        response.Result = 1;
                        sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                    }

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

                    // Receive param
                    if (recvBuffer.size() - recvBufferOffset < sizeof(param)) {
                        goto CONTINUE_HANDLE_API_QUERY;
                    }
                    memcpy(&param, recvBuffer.data() + recvBufferOffset, sizeof(param));
                    recvBufferOffset += sizeof(param);

                    std::cout << "[DEBUG] BeginRound_v1: " << param.SessionID << std::endl;

                    size_t i;
                    for (i = 0; i < sessions.size(); i++) {
                        if (sessions[i]->GetSessionID() == param.SessionID) {
                            if (sessions[i]->BeginRound()) {
                                response.Result = 0;
                            }
                            else {
                                response.Result = 1;
                            }
                            sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                            break;
                        }
                    }

                    // Session not found
                    if (i == sessions.size()) {
                        response.Result = 1;
                        sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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

                    // Receive param
                    if (recvBuffer.size() - recvBufferOffset < sizeof(param)) {
                        goto CONTINUE_HANDLE_API_QUERY;
                    }
                    memcpy(&param, recvBuffer.data() + recvBufferOffset, sizeof(param));
                    recvBufferOffset += sizeof(param);

                    std::cout << "[DEBUG] ActionPlayerInput_v1: " << param.SessionID << ", " << param.PlayerID << ", " << param.InputKey << ", " << param.InputType << std::endl;

                    // Find session
                    size_t findSessionIdx = MAX_SESSION;
                    for (size_t i = 0; i < sessions.size(); i++) {
                        if (sessions[i]->GetSessionID() == param.SessionID) {  
                            findSessionIdx = i;
                            break;
                        }
                    }

                    // Session not found
                    if (findSessionIdx == MAX_SESSION) {
                        response.Result = 1;
                        sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                        break;
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
                        sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                        sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                        sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                        break;
                    }

                    assert(sessions[findSessionIdx] != nullptr);
                    sessions[findSessionIdx]->SetPlayerInput(playerID, inputKey, inputType);

                    response.Result = 0;
                    sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                    sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                    break;
                }
                }

                recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + recvBufferOffset);
            }
        CONTINUE_HANDLE_API_QUERY:

            /* -------------------------- Begin Session Workers --------------------------- */
            std::vector<Session*> validSessions;
            {
                // Exclude sessions that have not elapsed a server tick duration or round is not running
                {
                    const std::chrono::milliseconds tickDuration(1000 / SERVER_TICK_RATE);
                    const std::chrono::system_clock::time_point nowTime = std::chrono::system_clock::now();
                    
                    for (size_t i = 0; i < sessions.size(); i++) {
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - sessions[i]->GetLastTickUpdateTime()) >= tickDuration
                            && sessions[i]->IsRoundRunning()) {
                            validSessions.push_back(sessions[i]);
                        }
                    }
                }

                // Distribute session to session worker
                mainThreadWakeUpMutex.lock();
                {
                    size_t sessionOffset = 0;
                    const size_t sessionPerWorker = validSessions.size() / NUM_SESSION_WORKER_THREAD;
                    const size_t sessionRemainder = validSessions.size() % NUM_SESSION_WORKER_THREAD;
                    for (size_t i = 0; i < NUM_SESSION_WORKER_THREAD; i++) 
                    {
                        TaskQueue& taskQueue = sessionWorkerTaskQueue[i];
                        
                        const size_t numTask = sessionPerWorker + ((i < sessionRemainder) ? 1 : 0);
                        if (taskQueue.Capacity < numTask) {
                            taskQueue.Capacity = numTask;
                            delete taskQueue.Tasks;
                            taskQueue.Tasks = new Session*[taskQueue.Capacity];
                        }

                        for (size_t j = 0; j < numTask; j++) {
                            taskQueue.Tasks[j] = validSessions[sessionOffset + j];
                        }
                        taskQueue.Count = numTask;
                        sessionOffset += numTask;
                    }

                    sessionWorkerAvailableCount = NUM_SESSION_WORKER_THREAD;
                }
                mainThreadWakeUpMutex.unlock();

                // Wake up session worker
                sessionWorkerWakeUpCondition.notify_all();
            }

            /* -------------------------- Wait for Session Workers -------------------------- */
            {
                std::unique_lock<std::mutex> cvLock(mainThreadWakeUpMutex);
                mainThreadWakeUpCondition.wait(cvLock, [&]() -> bool { return sessionWorkerAvailableCount == 0; });
            }

            /* ------------------------------ Send Round Result ----------------------------- */
            {
                for (Session* session : validSessions) {
                    if (!session->IsRoundRunning()) {
                        struct __attribute__((packed)) RoundResult_Response
                        {
                            uint32_t QueryID = 201;
                            uint32_t WinPlayer;
                        } response;

                        Session::RoundResultType roundResult = session->GetRoundResult();
                        if (roundResult == Session::RoundResultType::Timeout) {
                            response.WinPlayer = 0;
                        }
                        else if (roundResult == Session::RoundResultType::WinPlayerA) {
                            response.WinPlayer = 1;
                        }
                        else if (roundResult == Session::RoundResultType::WinPlayerB) {
                            response.WinPlayer = 2;
                        }
                        else {
                            assert(false);
                        }
                        
                        sendBuffer.insert(sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                    }
                }
            }

            /* ------------------------------ Close Session ------------------------------- */
            {
                for (size_t i = 0; i < sessions.size(); i++) {
                    if (sessions[i]->IsSessionEnded()) {
                        delete sessions[i];
                        sessions.erase(sessions.begin() + i);
                    }
                }
            }
        }


    CLOSE_CLIENT_CONNECTION:
        /* ---------------------------- Cleanup Resources --------------------------- */
        {
            // Close all session
            for (size_t i = 0; i < MAX_SESSION; i++) {
                delete sessions[i];
                sessions[i] = nullptr;
            }
            sessions.clear();

            // Close client socket
            close(clientSocket);
        }
    }


    // Cleanup threads and resources
    sessionWorkerWakeUpMutex.lock();
    {
        bSessionWorkerJoinFlag = true;
    }
    sessionWorkerWakeUpMutex.unlock();
    sessionWorkerWakeUpCondition.notify_all();
    for (int i = 0; i < NUM_SESSION_WORKER_THREAD; i++) {
        sessionWorkerThreads[i].join();
    }

    close(serverSocket);

    return 0;
}
