import socket, threading, json, time, argparse

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
            data = s.recv(4096).decode('utf-8', errors='ignore')
            if not data:
                print("\n[TCP] rozłączono")
                break
            buf += data
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if line.strip():
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

parser = argparse.ArgumentParser(description="TCP JSON client dla SymphonyESP")
parser.add_argument("ip", nargs="?", default="10.0.1.220", help="IP ESP32")
parser.add_argument("-p", "--port", type=int, default=3333, help="port TCP")
args = parser.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((args.ip, args.port))
print(f"Połączono {args.ip}:{args.port}")
threading.Thread(target=rx_loop, args=(s,), daemon=True).start()
time.sleep(0.5)

while True:
    print("\n--- MENU ---\n1-get_state 2-set_vol 3-set_url/play 4-stop 5-next q-quit")
    ch = input("wybierz> ").strip()
    if ch == "1": send_line(s, {"cmd":"get_state"})
    elif ch == "2":
        v = input("vol 0-100> ").strip()
        if v.isdigit(): send_line(s, {"cmd":"set_vol","value":int(v)})
    elif ch == "3":
        for i,u in enumerate(URLS): print(f" {i}: {u}")
        idx = input("idx lub url> ").strip()
        url = URLS[int(idx)] if idx.isdigit() and int(idx) < len(URLS) else idx
        send_line(s, {"cmd":"set_url","url":url})
        time.sleep(0.2)
        send_line(s, {"cmd":"play"})
    elif ch == "4": send_line(s, {"cmd":"stop"})
    elif ch == "5": send_line(s, {"cmd":"next"})
    elif ch == "q": break

s.close()