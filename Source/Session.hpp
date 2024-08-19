#pragma once

#include <cmath>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <cassert>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h> 
#include <unistd.h>

#include "math.hpp"
#include "config.hpp"
#include "Helper.hpp"

class Session
{
public:
    enum class PlayerID;
    enum class InputKey;
    enum class InputType;
    enum class RoundResultType;

public:
    Session(uint32_t sessionID, 
            uint32_t fieldWidth, 
            uint32_t fieldHeight, 
            uint32_t winScore, 
            uint32_t gameTime, 
            uint32_t ballSpeed, 
            uint32_t ballRadius,
            uint32_t paddleSpeed, 
            uint32_t paddleSize,
            uint32_t paddleOffsetFromWall,
            int udpSocket_ObjectPos_Stream,
            sockaddr_in addr_ObjectPos_Stream,
            uint16_t recvPort_ObjectPos_Stream);

    bool BeginRound();

    bool SetPlayerInput(PlayerID playerID, InputKey key, InputType type);

    bool Update();

    bool SendObjectState();

    inline uint32_t GetSessionID() const { return SessionID; }
    
    inline std::chrono::system_clock::time_point GetLastTickUpdateTime() const { return LastTickUpdateTime; }

    inline bool IsRoundRunning() const { return bRoundRunning; }

    inline RoundResultType GetRoundResult() const { return LastRoundResult; }

    inline bool IsSessionEnded() const { return bSessionEnded; }

public:
    // Player Input State
    enum class PlayerID
    {
        PlayerA = 1,
        PlayerB = 2
    };

    enum class InputKey
    {
        None = 0,
        Left = 1,
        Right = 2
    };

    enum class InputType
    {
        None = 0,
        Press = 1,
        // Release must exist after the press. Or not, the input will be ignored.
        Release = 2
    };

    struct PlayerInput
    {
        InputKey Key;
        InputType Type;
    };

public:
    // Round Result
    enum class RoundResultType
    {
        Timeout = 0,
        WinPlayerA = 1,
        WinPlayerB = 2
    };

private:
    uint32_t SessionID;
    std::chrono::system_clock::time_point LastTickUpdateTime; //< Time point of started last tick processing

    // Parameters
    uint32_t FieldWidth;
    uint32_t FieldHeight;
    uint32_t WinScore;
    uint32_t GameTime;
    uint32_t BallSpeed;
    uint32_t BallRadius;
    uint32_t PaddleSpeed;
    uint32_t PaddleSize;
    uint32_t PaddleOffsetFromWall;
    int UdpSocket_ObjectPos_Stream;
    sockaddr_in Addr_ObjectPos_Stream;
    uint16_t RecvPort_ObjectPos_Stream;

    // Player Input
    PlayerInput PlayerA_Input;
    PlayerInput PlayerB_Input;

    // Game State
    uint32_t ScoreA;
    uint32_t ScoreB ;
    vec2 BallPos;
    vec2 BallVel;
    float PlayerA_PaddlePos;
    float PlayerB_PaddlePos;
    InputKey PlayerA_PaddleDir; // Direction at last tick
    InputKey PlayerB_PaddleDir;
    std::chrono::milliseconds RoundTimeElapsed;
    bool bRoundRunning;
    bool bSessionEnded;
    RoundResultType LastRoundResult;
};
