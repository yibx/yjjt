import socket
import json
import time

def send_gpio_command(gpio, value):
    # Create TCP socket
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect(('127.0.0.1', 8090))
    
    # Prepare JSON message
    message = {
        "gpio": gpio,
        "value": value
    }
    
    # Send message
    client.send(json.dumps(message).encode())
    
    # Receive response
    response = client.recv(1024).decode()
    print(f"Server response: {response}")
    
    client.close()

if __name__ == "__main__":
    # Example usage
    send_gpio_command(17, 1)  # Set GPIO 17 high
    time.sleep(1)
    send_gpio_command(17, 0)  # Set GPIO 17 low
