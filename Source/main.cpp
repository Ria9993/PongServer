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
#include "Client.hpp"
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

        inline ~TaskQueue() {
            delete Tasks;
        }
    };

    std::condition_variable sessionWorkerWakeUpCondition;
    std::mutex              sessionWorkerWakeUpMutex;
    TaskQueue               sessionWorkerTaskQueue[NUM_SESSION_WORKER_THREAD];
    std::atomic<int32_t>    sessionWorkerTotalTaskRemainingCount(0);

    std::condition_variable mainThreadWakeUpCondition;
    std::mutex              mainThreadWakeUpMutex;

    bool                    bSessionWorkerJoinFlag = false;


    for (size_t i = 0; i < NUM_SESSION_WORKER_THREAD; i++) {
        sessionWorkerThreads[i] = std::thread([&](size_t threadId)
        {
            while (true) 
            {
                TaskQueue& taskQueue = sessionWorkerTaskQueue[threadId];

                // Check if task exist
                {
                    std::unique_lock<std::mutex> cvLock(sessionWorkerWakeUpMutex);
                    sessionWorkerWakeUpCondition.wait(cvLock, 
                        [&]() -> bool {
                            const int32_t localTaskCount = taskQueue.Count.load(std::memory_order_relaxed);
                            return (localTaskCount > 0) | bSessionWorkerJoinFlag;
                        });
                    if (bSessionWorkerJoinFlag) {
                        return;
                    }
                }

                int32_t completedTaskCount = 0;

                // Process all tasks in the local task queue
                while (true)
                {
                    // Get exclusive access to session
                    const int32_t taskIdx = taskQueue.Count.fetch_sub(1, std::memory_order_relaxed) - 1; 
                    if (taskIdx < 0) {
                        break;
                    }
                    Session* session = taskQueue.Tasks[taskIdx];
                    assert(session != nullptr);

                    //std::cout << "[DEBUG] thread[" << threadId << "] Begin work sesion #" << session->GetSessionID() << std::endl;
                    {
                        // Update session
                        session->Update();

                        // Send session state to client
                        session->SendObjectState();
                    }
                    completedTaskCount += 1;
                }

                // Work stealing from other worker
                for (size_t targetThreadId = threadId + 1; targetThreadId != threadId; targetThreadId = (targetThreadId + 1 < NUM_SESSION_WORKER_THREAD) ? targetThreadId + 1 : 0)
                {
                    while (true)
                    {
                        // Get exclusive access to session to steal
                        TaskQueue& targetTaskQueue = sessionWorkerTaskQueue[targetThreadId];
                        const int32_t taskIdx = targetTaskQueue.Count.fetch_sub(1, std::memory_order_relaxed) - 1;
                        if (taskIdx < 0) {
                            break;
                        }
                        Session* session = targetTaskQueue.Tasks[taskIdx];
                        assert(session != nullptr);

                        //std::cout << "[DEBUG] thread[" << threadId << "] Begin work sesion #" << session->GetSessionID() << "[Steal]" << std::endl;
                        {
                            // Update session
                            session->Update();

                            // Send session state to client
                            session->SendObjectState();
                        }
                        completedTaskCount += 1;
                    }
                }

                // Wake up main thread if all workers are completed
                {
                    std::unique_lock cvLock(mainThreadWakeUpMutex);
                    const int32_t remainTaskCount = sessionWorkerTotalTaskRemainingCount.fetch_sub(completedTaskCount, std::memory_order_release) - completedTaskCount;
                    // std::cout << "[DEBUG] completed. remainTaskCount: " << remainTaskCount << std::endl;
                    if (remainTaskCount == 0) {
                        // std::cout << "[DEBUG] wake up main thread." << std::endl;
                        cvLock.unlock();
                        mainThreadWakeUpCondition.notify_one();
                    }
                    assert(remainTaskCount >= 0);
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
#if defined(__linux__)
    int opt = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        std::cerr << "Failed to set socket option" << std::endl;
        close(serverSocket);
        return 1;
    }
#elif defined(__APPLE__)
    int opt = 1;
    if (::setsockopt(serverSocket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
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
    fd_set globalFdSet;
    FD_ZERO(&globalFdSet);
    FD_SET(serverSocket, &globalFdSet);
    int globalFdSet_MaxFd = serverSocket;

    /* -------------------------------------------------------------------------- */
    /*                                 Server Loop                                */
    /* -------------------------------------------------------------------------- */
    std::vector<Client*> clients;

    Session::InitSessionIdPool();

    while (true)
    {
        fd_set recvFdSet = globalFdSet;
        fd_set sendFdSet = globalFdSet;

        timeval zeroTimeout = { 0, };
        if (select(globalFdSet_MaxFd + 1, &recvFdSet, &sendFdSet, NULL, &zeroTimeout) == -1) {
            std::cerr << "Failed to select" << std::endl;
            close(serverSocket);
            return 1;
        }

        // Accept new clients 
        if (FD_ISSET(serverSocket, &recvFdSet)) 
        {
            while (true)
            {
                Client* newClient = new Client;
                assert(newClient != nullptr);

                newClient->socket = accept(serverSocket, (struct sockaddr*)&newClient->address, &newClient->addressLen);
                if (newClient->socket == -1) {
                    delete newClient;
                    break;
                }

                FD_SET(newClient->socket, &globalFdSet);
                if (globalFdSet_MaxFd < newClient->socket) {
                    globalFdSet_MaxFd = newClient->socket;
                }

                clients.push_back(newClient);

                std::cout << "[LOG] New client connectied." << std::endl;
            }
        }
            
        // Process each socket
        for (std::vector<Client*>::iterator clientIt = clients.begin(); clientIt != clients.end();)
        {
            Client& client = *(*clientIt);

            /* ------------------- Send buffered message to the client ------------------- */
            if (FD_ISSET(client.socket, &sendFdSet))
            {
                if (!client.sendBuffer.empty()) {
                    const int nBytesSent = send(client.socket, client.sendBuffer.data(), client.sendBuffer.size(), 0);
                    if (nBytesSent == -1) {
                        std::cout << "[DEBUG] send() == -1. errno: " << errno << std::endl;
                    }
                    else {
                        client.sendBuffer.erase(client.sendBuffer.begin(), client.sendBuffer.begin() + nBytesSent);
                    }
                }
            }

            /* --------------------- Receive message from the client -------------------- */
            if (FD_ISSET(client.socket, &recvFdSet))
            {
                char buffer[1024];
                const int nBytesRecv = recv(client.socket, buffer, sizeof(buffer), 0);
                if (nBytesRecv == -1) {
                    std::cout << "[DEBUG] recv() == -1. errno: " << errno << std::endl;
                }
                // Client disconnected
                else if (nBytesRecv == 0) {
                    std::cout << "Client disconnected" << std::endl;

                    FD_CLR(client.socket, &globalFdSet);

                    // remove sessions of the client
                    for (std::vector<Session*>::iterator sessionIt = sessions.begin(); sessionIt != sessions.end();) {
                        if ((*sessionIt)->GetOwnerClient() == &client) {
                            delete *sessionIt;
                            sessionIt = sessions.erase(sessionIt);
                        }
                        else {
                            ++sessionIt;
                        }
                    }

                    // delete client
                    delete *clientIt;
                    clientIt = clients.erase(clientIt);

                    continue;
                }
                else {
                    client.recvBuffer.insert(client.recvBuffer.end(), buffer, buffer + nBytesRecv);
                }
            }

            /* ---------------------------- Handle API Query ---------------------------- */
            if (FD_ISSET(client.socket, &recvFdSet))
            {
                size_t recvBufferOffset = 0;

                uint32_t queryID;
                if (client.recvBuffer.size() < sizeof(queryID)) {
                    goto CONTINUE_HANDLE_API_QUERY;
                }
                memcpy(&queryID, client.recvBuffer.data() + recvBufferOffset, sizeof(queryID));
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
                    if (client.recvBuffer.size() - recvBufferOffset < sizeof(param)) {
                        goto CONTINUE_HANDLE_API_QUERY;
                    }
                    memcpy(&param, client.recvBuffer.data() + recvBufferOffset, sizeof(param));
                    recvBufferOffset += sizeof(param);

                    std::cout << "[DEBUG] CreateSession_v1: " << param.FieldWidth << ", " << param.FieldHeight << ", " << param.WinScore << ", " << param.GameTime << ", " << param.BallSpeed << ", " << param.BallRadius << ", " << param.PaddleSpeed << ", " << param.PaddleSize << ", " << param.PaddleOffsetFromWall << ", " << param.RecvPort_ObjectPos_Stream << std::endl;

                    if (sessions.size() == MAX_SESSION) {
                        client.sendBuffer.insert(client.sendBuffer.end(), (char*)&fail_response, (char*)&fail_response + sizeof(fail_response));
                        break;
                    }

                    // Open global UDP socket for object position stream
                    static int udpSocket_ObjectPos_Stream = socket(AF_INET, SOCK_DGRAM, 0);
                    if (udpSocket_ObjectPos_Stream == -1) {
                        std::cerr << "Failed to create UDP socket" << std::endl;
                        client.sendBuffer.insert(client.sendBuffer.end(), (char*)&fail_response, (char*)&fail_response + sizeof(fail_response));
                        break;
                    }

                    Session* newSession = new Session(&client,
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
                                                    client.address,
                                                    param.RecvPort_ObjectPos_Stream);
                    assert(newSession != nullptr);
                    sessions.push_back(newSession);
                    client.sessions.push_back(newSession);

                    std::cout << "[DEBUG] Session Created: " << newSession->GetSessionID() << std::endl;

                    response.Result = 0;
                    response.SessionID = newSession->GetSessionID();
                    client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                    if (client.recvBuffer.size() - recvBufferOffset < sizeof(param)) {
                        goto CONTINUE_HANDLE_API_QUERY;
                    }
                    memcpy(&param, client.recvBuffer.data() + recvBufferOffset, sizeof(param));
                    recvBufferOffset += sizeof(param);

                    std::cout << "[DEBUG] AbortSession_v1: " << param.SessionID << std::endl;

                    size_t i;
                    for (i = 0; i < sessions.size(); i++) {
                        if (sessions[i]->GetSessionID() == param.SessionID) {
                            delete sessions[i];
                            sessions.erase(sessions.begin() + i);
                            response.Result = 0;
                            client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                            break;
                        }
                    }

                    // Session not found
                    if (i == sessions.size()) {
                        response.Result = 1;
                        client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                    if (client.recvBuffer.size() - recvBufferOffset < sizeof(param)) {
                        goto CONTINUE_HANDLE_API_QUERY;
                    }
                    memcpy(&param, client.recvBuffer.data() + recvBufferOffset, sizeof(param));
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
                            client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                            break;
                        }
                    }

                    // Session not found
                    if (i == sessions.size()) {
                        response.Result = 1;
                        client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                    if (client.recvBuffer.size() - recvBufferOffset < sizeof(param)) {
                        goto CONTINUE_HANDLE_API_QUERY;
                    }
                    memcpy(&param, client.recvBuffer.data() + recvBufferOffset, sizeof(param));
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
                        client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                        client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                        client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                        client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                        break;
                    }

                    assert(sessions[findSessionIdx] != nullptr);
                    sessions[findSessionIdx]->SetPlayerInput(playerID, inputKey, inputType);

                    response.Result = 0;
                    client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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
                    client.sendBuffer.insert(client.sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
                    break;
                }
                }

                client.recvBuffer.erase(client.recvBuffer.begin(), client.recvBuffer.begin() + recvBufferOffset);
            }
        CONTINUE_HANDLE_API_QUERY:
            ++clientIt;
        }

        /* -------------------------- Begin Session Workers --------------------------- */
        static std::chrono::steady_clock::time_point lastTickTime = std::chrono::steady_clock::now();
        std::vector<Session*> workableSessions;
        {
            // Check if the server tick duration time has elapsed
            const std::chrono::milliseconds tickDuration(1000 / SERVER_TICK_RATE);
            const std::chrono::steady_clock::time_point nowTime = std::chrono::steady_clock::now();
            const std::chrono::milliseconds deltaTime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - lastTickTime);
            const std::chrono::microseconds latency = std::chrono::duration_cast<std::chrono::microseconds>(nowTime - lastTickTime - tickDuration);
            //std::cout << "deltatime : " << deltaTime_ms.count() << " tickDuration: " << tickDuration.count() << std::endl;
            if (deltaTime_ms < tickDuration) {
                continue;
            }


            // Update last tick time         
            lastTickTime = nowTime;
            
            // Exclude sessions that round is not running
            {
                
                for (size_t i = 0; i < sessions.size(); i++) {
                    // if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - sessions[i]->GetLastTickUpdateTime()) >= tickDuration
                    if (sessions[i]->IsRoundRunning()) {
                        workableSessions.push_back(sessions[i]);
                    }
                }
            }

            // Log Latency(us)
            std::cout << "[DEBUG] RunningSession: " << workableSessions.size() << " Lat:" << latency.count() << "us" << std::endl;

            // Distribute session to session worker
            // (The wake-up condition can only be satisfied by this main thread, therefore, omit the sessionWorkerWakeUpMutex)
            {
                size_t sessionOffset = 0;
                const size_t sessionPerWorker = workableSessions.size() / NUM_SESSION_WORKER_THREAD;
                const size_t sessionRemainder = workableSessions.size() % NUM_SESSION_WORKER_THREAD;
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
                        taskQueue.Tasks[j] = workableSessions[sessionOffset + j];
                    }
                    taskQueue.Count = numTask;
                    sessionOffset += numTask;
                }

                // Print distribution
                // std::cout << "[DEBUG] -[TaskDistribution]-" << std::endl;
                // for (size_t i = 0; i < NUM_SESSION_WORKER_THREAD; i++) 
                // {
                //     TaskQueue& taskQueue = sessionWorkerTaskQueue[i];
                //     std::cout << "[" << i << "] { ";
                //     for (int32_t idx = 0; idx < taskQueue.Count.load(std::memory_order_relaxed); idx++) {
                //         std::cout << taskQueue.Tasks[idx] << ", ";
                //     }
                //     std::cout << "}" << std::endl;
                // }

                sessionWorkerTotalTaskRemainingCount.store(workableSessions.size(), std::memory_order_release);
            }


            // Wake up session worker
            if (workableSessions.size() != 0) {
                //std::cout << "[DEBUG] begin worker. taskNum: " << sessionWorkerTotalTaskRemainingCount.load(std::memory_order_relaxed) << std::endl;
                sessionWorkerWakeUpCondition.notify_all();
            }
        }

        /* -------------------------- Wait for Session Workers -------------------------- */
        if (workableSessions.size() != 0)
        {
            std::unique_lock<std::mutex> cvLock(mainThreadWakeUpMutex);
            mainThreadWakeUpCondition.wait(cvLock, [&]() -> bool {
                //std::cout << "[DEBUG] [MainThread] WakeUp. sessionWorkerTotalTaskRemainingCount: " << sessionWorkerTotalTaskRemainingCount.load(std::memory_order_relaxed) <<  std::endl;
                return sessionWorkerTotalTaskRemainingCount.load(std::memory_order_relaxed) == 0;
            });
        }

        /* ------------------------------ Send Round Result ----------------------------- */
        {
            for (Session* session : workableSessions) {
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
                    
                    Client* const ownerClient = session->GetOwnerClient();
                    ownerClient->sendBuffer.insert(ownerClient->sendBuffer.end(), (char*)&response, (char*)&response + sizeof(response));
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


    /* ---------------------------- Cleanup Resources --------------------------- */
    {
        // Close all session
        for (size_t i = 0; i < MAX_SESSION; i++) {
            delete sessions[i];
            sessions[i] = nullptr;
        }
        sessions.clear();

        // Close client socket
        for (Client* client : clients) {
            delete client;
        }
        clients.clear();
    }


    // Cleanup threads and resources
    std::atomic_store_explicit((std::atomic<bool>*)&bSessionWorkerJoinFlag, true, std::memory_order_release);
    sessionWorkerWakeUpCondition.notify_all();
    for (int i = 0; i < NUM_SESSION_WORKER_THREAD; i++) {
        sessionWorkerThreads[i].join();
    }

    close(serverSocket);

    return 0;
}
