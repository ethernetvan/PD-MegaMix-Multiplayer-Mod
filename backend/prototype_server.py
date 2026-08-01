import socket

def start_prototype_server():
    # 1. Create a TCP/IP socket
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    # 2. Bind the socket to your PC's local IP and port 8080
    server_socket.bind(('0.0.0.0', 8080))
    server_socket.listen(1)
    
    print("Waiting for MegaMix payload on port 8080...")
    
    while True:
        # 3. Wait for the Switch to connect
        connection, client_address = server_socket.accept()
        try:
            print(f"Switch connected from: {client_address}")
            
            # 4. Receive the "SONG_STARTED" message
            data = connection.recv(16) 
            if data:
                print(f"Payload received: {data.decode('utf-8')}")
                
        finally:
            connection.close()

if __name__ == '__main__':
    start_prototype_server()

# This is for proof of concept/testing. Eventually move to Go backend