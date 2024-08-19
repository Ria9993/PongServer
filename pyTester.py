import socket
import struct
import threading
import pygame
import sys

# Initialize Pygame
pygame.init()

# Screen dimensions
SCREEN_WIDTH = 800
SCREEN_HEIGHT = 600

# Colors
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)

# Ball and paddle dimensions
BALL_SIZE = 20
BALL_SPEED = 400
PADDLE_WIDTH = 5
PADDLE_HEIGHT = 300
PADDLE_SPEED = 400
PADDLE_OFFSET = 100

# Create the screen
screen = pygame.display.set_mode((SCREEN_WIDTH, SCREEN_HEIGHT))
pygame.display.set_caption("Pong Game Visualization")

# Thread event
stop_event = threading.Event()

# Function to draw the game objects
def draw_objects(ball_x, ball_y, paddle_a_y, paddle_b_y):
    screen.fill(BLACK)
    pygame.draw.rect(screen, WHITE, (PADDLE_OFFSET,  (SCREEN_HEIGHT / 2) + paddle_a_y - (PADDLE_HEIGHT / 2), PADDLE_WIDTH, PADDLE_HEIGHT))
    pygame.draw.rect(screen, WHITE, (SCREEN_WIDTH - PADDLE_OFFSET - PADDLE_WIDTH, (SCREEN_HEIGHT / 2) + paddle_b_y - (PADDLE_HEIGHT / 2), PADDLE_WIDTH, PADDLE_HEIGHT))
    pygame.draw.ellipse(screen, WHITE, (ball_x, ball_y, BALL_SIZE, BALL_SIZE))
    pygame.display.flip()

def udp_listener(udp_socket):
    while not stop_event.is_set():
        data, _ = udp_socket.recvfrom(16)  # 4 * 4 bytes = 16 bytes
        if len(data) != 16:
            print(f"Unexpected data length: {len(data)}")
            continue
        unpacked_data = struct.unpack('ffff', data)
        ball_x, ball_y, paddle_a_y, paddle_b_y = unpacked_data
        draw_objects(ball_x, ball_y, paddle_a_y, paddle_b_y)

def recv_all(sock, length):
    data = b''
    while len(data) < length:
        more = sock.recv(length - len(data))
        if not more:
            raise EOFError('Socket closed before receiving all data')
        data += more
    return data

def main():
    # Open UDP receiving socket
    udp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_socket.bind(('', 0))
    udp_port = struct.pack("!H", socket.htons(udp_socket.getsockname()[1]))
    udp_port_bigendian = struct.unpack("!H", udp_port)[0]

    print(f"UDP port: {udp_port}")
    
    # Connect TCP 127.0.0.1:9180
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_socket.connect(('127.0.0.1', 9180))

    # Send API Query CreateSession_v1
        # uint32_t QueryID;
        # uint32_t FieldWidth;
        # uint32_t FieldHeight;
        # uint32_t WinScore;
        # uint32_t GameTime;
        # uint32_t BallSpeed;
        # uint32_t BallRadius;
        # uint32_t PaddleSpeed;
        # uint32_t PaddleSize;
        # uint32_t PaddleOffsetFromWall;
        # uint16_t UdpPort_Recv_Stream;
    create_session_param = struct.pack('<IIIIIIIIIIH', 101, SCREEN_WIDTH, SCREEN_HEIGHT, 5, 300, BALL_SPEED, BALL_SIZE, PADDLE_SPEED, PADDLE_HEIGHT, PADDLE_OFFSET, udp_port_bigendian)
    client_socket.sendall(create_session_param)

    # Receive response
    response = recv_all(client_socket, 9)  # 4 + 1 + 4 bytes
    print(f"Received response length: {len(response)}")
    query_id, result, session_id = struct.unpack('<IBI', response)
    print(f"Query ID: {query_id}, Result: {result}, Session ID: {session_id}")
    assert query_id == 101

    if result != 0:
        print("Failed to create session")
        client_socket.close()
        return

    print(f"Session ID: {session_id}")

    # Send API Query BeginRound_v1
    begin_round_param = struct.pack('<II', 201, session_id)
    client_socket.sendall(begin_round_param)

    # Receive response
    response2 = client_socket.recv(5)  # 4 + 1 bytes
    query_id2, result2 = struct.unpack('<IB', response2)
    assert query_id2 == 201

    if result2 != 0:
        print("Failed to begin round")
        client_socket.close()
        return
    else:
        print("Round begun")

    # Start UDP listener thread
    udp_thread = threading.Thread(target=udp_listener, args=(udp_socket,))
    udp_thread.start()

    # Loop to detect round end
    while True:
        responce_query_id = struct.unpack('<I', recv_all(client_socket, 4))[0]
        print(f"Received query ID: {responce_query_id}")
        if responce_query_id == 301:
            result_dummy = struct.unpack('<B', recv_all(client_socket, 1))[0]
            continue

        if responce_query_id == 201:
            result_dummy = struct.unpack('<B', recv_all(client_socket, 1))[0]
            win_player = struct.unpack('<I', recv_all(client_socket, 4))[0]
            print(f"Round ended, winner: {win_player}")
            break

    stop_event.set()
    udp_thread.join()

if __name__ == "__main__":
    main()
