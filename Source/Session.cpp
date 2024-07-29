#include "Session.hpp"

Session::Session(int clientSocket, 
            uint32_t sessionID, 
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
            uint16_t recvPort_ObjectPos_Stream)
    : SessionID(sessionID)
    , LastTickUpdateTime(std::chrono::system_clock::now())
    , ClientSocket(clientSocket)
    , FieldWidth(fieldWidth)
    , FieldHeight(fieldHeight)
    , WinScore(winScore)
    , GameTime(gameTime)
    , BallSpeed(ballSpeed)
    , BallRadius(ballRadius)
    , PaddleSpeed(paddleSpeed)
    , PaddleSize(paddleSize)
    , PaddleOffsetFromWall(paddleOffsetFromWall)
    , UdpSocket_ObjectPos_Stream(udpSocket_ObjectPos_Stream)
    , Addr_ObjectPos_Stream(addr_ObjectPos_Stream)
    , RecvPort_ObjectPos_Stream(recvPort_ObjectPos_Stream)
    , ScoreA(0)
    , ScoreB(0)
    , RoundTimeElapsed(0)
    , bRoundRunning(false)
    , bThreadWorkerRunning(false)
{
    Addr_ObjectPos_Stream.sin_port = recvPort_ObjectPos_Stream;
}

bool Session::BeginRound()
{
    if (bRoundRunning) {
        return false;
    }

    bool casExpected = false;
    while (!bThreadWorkerRunning.compare_exchange_weak(casExpected, true, std::memory_order_relaxed, std::memory_order_acquire))
    {
    }
    {
        PlayerA_Input.Key = InputKey::None;
        PlayerB_Input.Key = InputKey::None;
        PlayerA_Input.Type = InputType::None;
        PlayerB_Input.Type = InputType::None;

        BallPos = { FieldWidth / 2.0f, FieldHeight / 2.0f };
        
        // Randomize ball direction
        const float theta = (rand() % 360) * (3.14159265358f / 180.0f);
        BallVel.x = cosf(theta);
        BallVel.y = sinf(theta);
        BallVel = vec2::normalize(BallVel) * BallSpeed;

        PlayerA_PaddlePos = 0.0f;
        PlayerB_PaddlePos = 0.0f;
        PlayerA_PaddleDir = InputKey::None;
        PlayerB_PaddleDir = InputKey::None;

        RoundTimeElapsed = std::chrono::milliseconds(0);
        bRoundRunning = true;
    }
    bThreadWorkerRunning.store(false, std::memory_order_release);

    return true;
}

bool Session::SetPlayerInput(PlayerID playerID, InputKey key, InputType type)
{
    if (!bRoundRunning) {
        return false;
    }

    // std::cout << "[DEBUG] SetPlayerInput: " << (int)playerID << ", " << (int)key << ", " << (int)type << std::endl;

    if (playerID == PlayerID::PlayerA) {
        PlayerA_Input.Key = key;
        PlayerA_Input.Type = type;
    }
    else if (playerID == PlayerID::PlayerB) {
        PlayerB_Input.Key = key;
        PlayerB_Input.Type = type;
    }

    return true;
}

bool Session::Update()
{
    // Get delta time
    const std::chrono::milliseconds tickDuration(1000 / SERVER_TICK_RATE);
    const std::chrono::system_clock::time_point nowTime = std::chrono::system_clock::now();
    const std::chrono::milliseconds deltaTime_Ms = std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - LastTickUpdateTime);
    const float deltaTime_Sec = (float)deltaTime_Ms.count() / 1000;
    
    // assert(deltaTime_Ms.count() <= tickDuration.count());
    if (deltaTime_Ms.count() > tickDuration.count()) {
        logStream << "Session " << SessionID << " is too fast. deltaTime: " << deltaTime_Ms.count() << "ms" << std::endl;
    }

    // Update last tick update time
    LastTickUpdateTime = std::chrono::system_clock::now();

    if (!bRoundRunning) {
        return true;
    }

    RoundTimeElapsed += deltaTime_Ms;
    // Timeout 
    if (RoundTimeElapsed >= std::chrono::milliseconds(GameTime * 1000)) {
        bRoundRunning = false;

        // Response the round result
        struct __attribute__((packed)) RoundResult
        {
            uint8_t Result;
            uint32_t WinPlayer;
        } roundResult;

        roundResult.Result = 0;
        roundResult.WinPlayer = 0;

        return SendFull(ClientSocket, &roundResult, sizeof(roundResult));
    }

    // Update paddle position
    const uint32_t deltaPaddlePos = PaddleSpeed * deltaTime_Sec;

    const float paddlePosMax = (float)FieldHeight / 2.f;
    const float paddlePosMin = -(float)FieldHeight / 2.f;
    if (PlayerA_PaddleDir == InputKey::Right) {
        PlayerA_PaddlePos -= deltaPaddlePos;
        if (PlayerA_PaddlePos < paddlePosMin) {
            PlayerA_PaddlePos = paddlePosMin;
        }
    }
    else if (PlayerA_PaddleDir == InputKey::Left) {
        PlayerA_PaddlePos += deltaPaddlePos;
        if (PlayerA_PaddlePos > paddlePosMax) {
            PlayerA_PaddlePos = paddlePosMax;
        }
    }
    if (PlayerB_PaddleDir == InputKey::Right) {
        PlayerB_PaddlePos -= deltaPaddlePos;
        if (PlayerB_PaddlePos < paddlePosMin) {
            PlayerB_PaddlePos = paddlePosMin;
        }
    }
    else if (PlayerB_PaddleDir == InputKey::Left) {
        PlayerB_PaddlePos += deltaPaddlePos;
        if (PlayerB_PaddlePos > paddlePosMax) {
            PlayerB_PaddlePos = paddlePosMax;
        }
    }

    if (PlayerA_Input.Type == InputType::Release) {
        PlayerA_PaddleDir = InputKey::None;
    }
    if (PlayerA_Input.Type == InputType::Press) {
        PlayerA_PaddleDir = PlayerA_Input.Key;
    }
    if (PlayerB_Input.Type == InputType::Release) {
        PlayerB_PaddleDir = InputKey::None;
    }
    if (PlayerB_Input.Type == InputType::Press) {
        PlayerB_PaddleDir = PlayerB_Input.Key;
    }

    // Compute absolute position of paddle
    const vec2 paddleA_BaseAbsPos = { (float)PaddleOffsetFromWall, FieldHeight / 2.0f };
    const vec2 paddleB_BaseAbsPos = { FieldWidth - (float)PaddleOffsetFromWall, FieldHeight / 2.0f };

    vec2 paddleA_AbsPos;
    paddleA_AbsPos.x = paddleA_BaseAbsPos.x;
    paddleA_AbsPos.y = paddleA_BaseAbsPos.y - PlayerA_PaddlePos;

    vec2 paddleB_AbsPos;
    paddleB_AbsPos.x = paddleB_BaseAbsPos.x;
    paddleB_AbsPos.y = paddleB_BaseAbsPos.y + PlayerB_PaddlePos;

    // DEBUG: TEST
    // BallPos.x = 400.f;
    // BallPos.y = 599.f;
    // BallVel.x = 100.f;
    // BallVel.y = 160.f;

    // Compute ball position with collision detection recursively.
    float ballLeftMove = deltaTime_Sec * BallSpeed;
    vec2 nextBallPos;
    nextBallPos.x = BallPos.x + BallVel.x * deltaTime_Sec;
    nextBallPos.y = BallPos.y + BallVel.y * deltaTime_Sec;

    while (ballLeftMove >= 1.0f)
    {
        vec2 ballDir = vec2::normalize(nextBallPos - BallPos);

        // shortestPointA = A_s + factorS[0, 1] * (A_e - A_s)
        // shortestPointB = B_s + factorT[0, 1] * (B_e - Bs)
        auto func_Compute_ShortestDistancePoint_LineSeg = [](vec2 A1, vec2 A2, vec2 B1, vec2 B2, vec2* outPointA, vec2* outPointB, float* outFactorS = nullptr, float* outFactorT = nullptr) -> void
        {
            // Reference: https://math.stackexchange.com/a/2812513
            vec2 L1 = A2 - A1;
            vec2 L2 = B2 - B1;
            vec2 L12 = B1 - A1;

            float v22 = vec2::dot(L2, L2);
            float v11 = vec2::dot(L1, L1);
            float v12 = vec2::dot(L1, L2);
            float den = v12 * v12 - v11 * v22;

            float v1_12 = vec2::dot(L1, L12);
            float v2_12 = vec2::dot(L2, L12);

            // Print v22, v11, v12, den, v1_12, v2_12
            // std::cout << std::endl;
            // std::cout << "BallPrevPos: " << A1.x << ", " << A1.y << std::endl;
            // std::cout << "BallNextPos: " << A1.x << ", " << A2.y << std::endl;
            // std::cout << "v22: " << v22 << std::endl;
            // std::cout << "v11: " << v11 << std::endl;
            // std::cout << "v12: " << v12 << std::endl;
            // std::cout << "den: " << den << std::endl;
            // std::cout << "v1_12: " << v1_12 << std::endl;
            // std::cout << "v2_12: " << v2_12 << std::endl;

            float s = 0.f;
            float t = 0.f;

            // Divergence by overlapping line
            if (std::abs(den) < FLT_EPSILON) {
                // t = (d3121 - R1*s) / d4321
                s = 0.f;
                t = (-v1_12) / v12;
            }
            else {
                s = (v2_12 * v12 - v22 * v1_12) / den;
                t = (v11 * v2_12 - v1_12 * v12) / den;
            }
            
            // std::cout << std::endl;
            // std::cout << "s: " << s << std::endl;
            // std::cout << "t: " << t << std::endl;

            // Clamp [0, 1]
            s = std::max(0.f, std::min(1.f, s));
            t = std::max(0.f, std::min(1.f, t));
            

            vec2 S1 = A1 + s * L1;
            vec2 S2 = B1 + t * L2;
            
            *outPointA = S1;
            *outPointB = S2;

            if (outFactorS != nullptr) {
                *outFactorS = s;
            }
            if (outFactorT != nullptr) {
                *outFactorT = t;
            }

            return;
        };

        // Paddle
        {
            vec2 paddleAbsPos[2] = { paddleA_AbsPos, paddleB_AbsPos };
            for (int i = 0; i < 2; i++)
            {
                vec2 currPaddle_AbsPos = paddleAbsPos[i];

                vec2 shortestPointA;
                vec2 shortestPointB;
                float factorT; //< shortestPointB = B_s + factorT[0, 1] * (B_e - B_s)
                func_Compute_ShortestDistancePoint_LineSeg(
                    BallPos, 
                    nextBallPos,
                    {currPaddle_AbsPos.x, currPaddle_AbsPos.y - PaddleSize / 2}, //< paddle_bottom
                    {currPaddle_AbsPos.x, currPaddle_AbsPos.y + PaddleSize / 2}, //< paddle_top
                    &shortestPointA, &shortestPointB, nullptr, &factorT);
                
                vec2 shortestVec = shortestPointB - shortestPointA;

                // Collision
                if (shortestVec.length() < BallRadius - FLT_EPSILON)
                {
                    // Ignore collisions if the ball's path was away from the wall
                    if (vec2::dot(shortestVec, ballDir) < 0) {
                        continue;
                    }

                    // Reflection by custom formula
                    const vec2 paddleVec = { 0, (float)PaddleSize };
                    const float theta = vec2::cross(paddleVec, ballDir);

                    vec2 paddleNormal;
                    if (theta < 0) {
                        paddleNormal = LineNormalVector({ currPaddle_AbsPos.x, currPaddle_AbsPos.y - PaddleSize / 2 }, { currPaddle_AbsPos.x, currPaddle_AbsPos.y + PaddleSize / 2 }, true);
                    }
                    else {
                        paddleNormal = LineNormalVector({ currPaddle_AbsPos.x, currPaddle_AbsPos.y - PaddleSize / 2 }, { currPaddle_AbsPos.x, currPaddle_AbsPos.y + PaddleSize / 2 }, false);
                    }

                    const float paddleReflectFactor = 0.8f; // Adjust reflection angle to [-90' * Factor, 90' * Factor];
                    float reflectTheta = (factorT - 0.5f) * paddleReflectFactor; // factorT [0.f, 1.f] -> [-0.5f, 0.5f]
                    reflectTheta *= paddleNormal.x;

                    // rotation paddleNormal reflectThetaRad to make reflection vector
                    const float reflectThetaRad = reflectTheta * 3.14f;
                    vec2 reflectVec;
                    reflectVec.x = paddleNormal.x * cosf(reflectThetaRad) - paddleNormal.y * sinf(reflectThetaRad);
                    reflectVec.y = paddleNormal.x * sinf(reflectThetaRad) + paddleNormal.y * cosf(reflectThetaRad);
                    reflectVec = vec2::normalize(reflectVec);


                    // Update ball velocity vector
                    BallVel = reflectVec * BallSpeed;

                    // Move ball to exact collision point
                    //vec2 collisionPos = shortestPointB + vec2::normalize(-shortestVec * BallRadius);
                    vec2 collisionPos = shortestPointA;
                    nextBallPos = collisionPos + reflectVec * (ballLeftMove - (collisionPos - BallPos).length());
                    ballLeftMove = (collisionPos - BallPos).length();
                    const float epsilon = 0.1f;
                    BallPos = collisionPos + (reflectVec * epsilon);

                    // std::cout << "[DEBUG] PointA: " << shortestPointA.x << ", " << shortestPointA.y << std::endl;
                    // std::cout << "[DEBUG] PointB: " << shortestPointB.x << ", " << shortestPointB.y << std::endl;
                    // std::cout << "[DEBUG] ShortestLength: " << shortestVec.length() << std::endl;
                    // std::cout << "[DEBUG] reflectTheta: " << reflectTheta << std::endl;
                    // std::cout << "[DEBUG] wallNormal: " << paddleNormal.x << ", " << paddleNormal.y << std::endl;
                    // std::cout << "[DEBUG] reflectVec: " << reflectVec.x << ", " << reflectVec.y << std::endl;
                    // std::cout << "[DEBUG] BallPos: " << BallPos.x << ", " << BallPos.y << std::endl;
                    // std::cout << "[DEBUG] BallNextPos: " << nextBallPos.x << ", " << nextBallPos.y << std::endl;
                    

                    goto CONTINUE_COLLISION_DETECTION;
                }
            }
        }

        // Wall
        vec2 wall[4][2];
        wall[0][0] = { 0                , 0 };
        wall[0][1] = { (float)FieldWidth, 0 };
        wall[1][0] = { 0               , (float)FieldHeight };
        wall[1][1] = { (float)FieldWidth, (float)FieldHeight };
        wall[2][0] = { 0, 0 };
        wall[2][1] = { 0, (float)FieldHeight };
        wall[3][0] = { (float)FieldWidth, 0 };
        wall[3][1] = { (float)FieldWidth, (float)FieldHeight };
        for (int i = 0; i < 4; i++) 
        {
            vec2 shortestPointA;
            vec2 shortestPointB;
            func_Compute_ShortestDistancePoint_LineSeg(BallPos, nextBallPos, wall[i][0], wall[i][1], &shortestPointA, &shortestPointB);

            vec2 shortestVec = shortestPointB - shortestPointA;

            // Collision
            if (shortestVec.length() < BallRadius - FLT_EPSILON) 
            {
                // Ignore collisions if the ball's path was away from the wall
                if (vec2::dot(shortestVec, ballDir) < 0) {
                    continue;
                }

                // Touch goal
                if (i == 2 || i == 3) {
                    bRoundRunning = false;

                    // Response the round result
                    struct __attribute__((packed)) RoundResult
                    {
                        uint8_t Result;
                        uint32_t WinPlayer;
                    } roundResult;

                    roundResult.Result = 0;

                    if (i == 2) {
                        ScoreA++;
                        roundResult.WinPlayer = 1;
                    }
                    else {
                        ScoreB++;
                        roundResult.WinPlayer = 2;
                    }

                    std::cout << "[DEBUG] RoundResult: " << (int)roundResult.WinPlayer << std::endl;

                    return SendFull(ClientSocket, &roundResult, sizeof(roundResult));
                }

                // Reflection
                const vec2 wallVec = wall[i][1] - wall[i][0];
                const float theta  = vec2::cross(wallVec, ballDir);

                vec2 wallNormal;
                if (theta > 0) {
                    wallNormal = LineNormalVector(wall[i][0], wall[i][1], true);
                }
                else {
                    wallNormal = LineNormalVector(wall[i][0], wall[i][1], false);
                }

                const vec2 reflectVec = (2 * vec2::dot(-ballDir, wallNormal) * wallNormal) + ballDir;
                
                // Update ball velocity vector
                BallVel = reflectVec * BallSpeed;

                // Move ball from exact collision point
                vec2 collisionPos = shortestPointB + vec2::normalize(-shortestVec) * BallRadius;
                nextBallPos = collisionPos + reflectVec * (ballLeftMove - (collisionPos - BallPos).length());
                ballLeftMove = (collisionPos - BallPos).length();
                const float epsilon = 0.1f;
                BallPos = collisionPos + (reflectVec * epsilon);

                goto CONTINUE_COLLISION_DETECTION;
            }
        }

        // No collision
        {
            BallPos = nextBallPos;
            ballLeftMove = 0.f;
            break;
        }

        CONTINUE_COLLISION_DETECTION:;
    }
    END_COLLISION_DETECTION:;

    return true;
}

bool Session::SendObjectState()
{
    struct __attribute__((packed)) ObjectState
    {
        vec2 BallPos;
        float PlayerA_PaddlePos;
        float PlayerB_PaddlePos;
    } objectState;

    objectState.BallPos = BallPos;
    objectState.PlayerA_PaddlePos = PlayerA_PaddlePos;
    objectState.PlayerB_PaddlePos = PlayerB_PaddlePos;

    int nBytesSend = sendto(UdpSocket_ObjectPos_Stream, &objectState, sizeof(objectState), 0, (sockaddr*)&Addr_ObjectPos_Stream, sizeof(Addr_ObjectPos_Stream));
    if (nBytesSend <= 0) {
        return false;
    }

    return true;
}
