import cv2
import socket
import numpy as np
import time

ZEDBOARD_IP = '192.168.1.10'
PORT = 7
WIDTH = 256
HEIGHT = 256
FRAME_SIZE = WIDTH * HEIGHT
CHUNK_SIZE = 1024

print("1. Opening webcam...")
cap = cv2.VideoCapture(0)
ret, frame = cap.read()
cap.release()

if not ret:
    print("Error: Could not read frame from webcam.")
    exit(1)

gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
resized_gray = cv2.resize(gray, (WIDTH, HEIGHT))
payload = resized_gray.tobytes()
print(f"2. Prepared {len(payload)}-byte grayscale frame ({WIDTH}x{HEIGHT}).")

print(f"3. Connecting to ZedBoard at {ZEDBOARD_IP}:{PORT}...")

try:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
        client.settimeout(10.0)
        client.connect((ZEDBOARD_IP, PORT))
        print("Connected!")

        start_time = time.time()

        print("-> Stage 1: Uploading 64 KB grayscale image to DDR3 (0x10000000)...")
        for offset in range(0, FRAME_SIZE, CHUNK_SIZE):
            client.sendall(payload[offset:offset + CHUNK_SIZE])

        print("-> Stage 2: Downloading 64 KB FPGA Sobel output from DDR3 (0x10100000)...")
        received_data = bytearray()
        while len(received_data) < FRAME_SIZE:
            packet = client.recv(4096)
            if not packet:
                break
            received_data.extend(packet)

        elapsed = time.time() - start_time

    print(f"Transfer + FPGA Execution Time: {round(elapsed, 4)} seconds")

    if len(received_data) == FRAME_SIZE:
        sobel_output = np.frombuffer(received_data, dtype=np.uint8).reshape((HEIGHT, WIDTH))
        combined = np.hstack((resized_gray, sobel_output))
        cv2.imshow("Left: Original Grayscale (0x10000000) | Right: FPGA Sobel Output (0x10100000)", combined)
        print("Success! Press any key on the image window to exit.")
        cv2.waitKey(0)
        cv2.destroyAllWindows()
    else:
        print(f"Error: Expected {FRAME_SIZE} bytes, received {len(received_data)} bytes.")

except Exception as e:
    print(f"Connection error: {e}")
