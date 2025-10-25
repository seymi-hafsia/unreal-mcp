"""Test raw socket communication with Unreal MCP"""
import socket
import time

def test_raw_send():
    """Test sending raw data to see if Unreal receives it"""
    try:
        print("Connecting to Unreal Engine on port 55557...")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect(('127.0.0.1', 55557))
        print("[OK] Connected!")

        # Send a simple message
        message = '{"command": "get_actors_in_level", "params": {}}\n'
        print(f"\nSending: {message.strip()}")
        print(f"Message length: {len(message)} bytes")

        # Send byte by byte to debug
        bytes_sent = sock.send(message.encode('utf-8'))
        print(f"Bytes sent: {bytes_sent}")

        # Make sure data is flushed
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        print("\nWaiting for response (15 seconds)...")
        time.sleep(1)

        # Try to receive with longer timeout
        sock.settimeout(15)
        data = sock.recv(4096)

        if data:
            print(f"\n[OK] Received {len(data)} bytes:")
            print(data.decode('utf-8'))
        else:
            print("\n[ERROR] No data received")

        sock.close()

    except socket.timeout:
        print("\n[ERROR] Timeout - no response from Unreal")
    except Exception as e:
        print(f"\n[ERROR] {e}")

if __name__ == "__main__":
    test_raw_send()
