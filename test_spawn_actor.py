"""Test spawning an actor in Unreal Engine"""
import socket
import json

def send_command(command, params=None):
    """Send a command to Unreal and get response"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)

    try:
        sock.connect(('127.0.0.1', 55557))

        message = {"command": command, "params": params or {}}
        message_json = json.dumps(message) + '\n'

        print(f"Sending: {command}")
        sock.send(message_json.encode('utf-8'))

        response_data = sock.recv(8192)
        response = json.loads(response_data.decode('utf-8'))

        print(f"Response: {json.dumps(response, indent=2)}")
        return response

    finally:
        sock.close()

if __name__ == "__main__":
    # Test 1: Get actors in level
    print("\n=== Test 1: Get actors in level ===")
    send_command("get_actors_in_level")

    # Test 2: Spawn a new actor
    print("\n=== Test 2: Spawn a cube actor ===")
    send_command("spawn_actor", {
        "name": "TestCube",
        "type": "StaticMeshActor",
        "location": [500, 0, 100]
    })

    # Test 3: Get actors again to confirm
    print("\n=== Test 3: Get actors again ===")
    result = send_command("get_actors_in_level")

    if result.get("status") == "success":
        actors = result.get("result", {}).get("actors", [])
        print(f"\n[OK] Found {len(actors)} actors in level")
        test_cube = [a for a in actors if a.get("name") == "TestCube"]
        if test_cube:
            print("[OK] TestCube was successfully spawned!")
        else:
            print("[WARNING] TestCube not found in actor list")
