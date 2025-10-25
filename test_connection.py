"""Quick test to verify Unreal MCP plugin is responding"""
import socket
import json
import sys

def test_unreal_connection():
    """Test connection to Unreal Engine MCP plugin"""
    try:
        # Connect
        print("Connecting to Unreal Engine on port 55557...")
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect(('127.0.0.1', 55557))
        print("[OK] Connected successfully!")

        # Send a simple command to get actors
        command = {
            "command": "get_actors_in_level",
            "params": {}
        }

        command_json = json.dumps(command) + '\n'
        print(f"\nSending command: {command_json.strip()}")
        sock.sendall(command_json.encode('utf-8'))

        # Receive response
        print("Waiting for response...")
        chunks = []
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)

            # Try to parse as complete JSON
            try:
                data = b''.join(chunks)
                response = json.loads(data.decode('utf-8'))
                print("\n[OK] Received response:")
                print(json.dumps(response, indent=2))
                break
            except json.JSONDecodeError:
                continue

        sock.close()
        return True

    except Exception as e:
        print(f"\n[ERROR] Error: {e}")
        return False

if __name__ == "__main__":
    success = test_unreal_connection()
    sys.exit(0 if success else 1)
