import socket
import threading
import json
import time

HOST = "10.0.1.220"
PORT = 3333

URLS = [
    "http://ice1.somafm.com/groovesalad-128-mp3",
    "http://ice1.somafm.com/beatblender-128-mp3",
    "http://stream.radioparadise.com/mp3-128",
]

def rx_loop(s):
    buf = ""
    s.settimeout(0.5)
    while True:
        try:
            data = s.recv(1024).decode('utf-8', errors='ignore')
            if not data:
                print("\n[TCP] rozłączono przez serwer")
                break
            buf += data
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if not line.strip(): continue
                print(f"<< RX: {line}")
        except socket.timeout:
            continue
        except Exception as e:
            print(f"RX error {e}")
            break

def send_line(s, obj):
    line = json.dumps(obj) + "\n"
    s.sendall(line.encode())
    print(f">> TX: {line.strip()}")

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((HOST, PORT))
print(f"Połączono {HOST}:{PORT}")

threading.Thread(target=rx_loop, args=(s,), daemon=True).start()
time.sleep(0.5)

while True:
    print("\n--- MENU ---")
    print("1 - get_state")
    print("2 - set_vol")
    print("3 - set_url / play")
    print("4 - stop")
    print("5 - next")
    print("q - quit")
    ch = input("wybierz> ").strip()

    if ch == "1":
        send_line(s, {"cmd":"get_state"})
    elif ch == "2":
        v = input("vol 0-100> ").strip()
        try:
            send_line(s, {"cmd":"set_vol","value":int(v)})
        except: print("zły vol")
    elif ch == "3":
        print("URLS:")
        for i,u in enumerate(URLS): print(f" {i}: {u}")
        idx = input("idx lub własny url> ").strip()
        url = URLS[int(idx)] if idx.isdigit() and int(idx) < len(URLS) else idx
        send_line(s, {"cmd":"set_url","url":url})
        time.sleep(0.2)
        send_line(s, {"cmd":"play"})
    elif ch == "4":
        send_line(s, {"cmd":"stop"})
    elif ch == "5":
        send_line(s, {"cmd":"next"})
    elif ch == "q":
        break

s.close()