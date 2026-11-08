import socket, threading, json, time, argparse

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

parser = argparse.ArgumentParser()
parser.add_argument("ip", nargs="?", default="10.0.1.220")
parser.add_argument("-p", "--port", type=int, default=3333)
args = parser.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((args.ip, args.port))
print(f"Połączono {args.ip}:{args.port}")
threading.Thread(target=rx_loop, args=(s,), daemon=True).start()
time.sleep(0.3)

while True:
    print("\n--- TEST FILTROWANIA - 1 wybór = 1 komenda ---")
    print("1  - get_state")
    print("2  - set_vol")
    print("3  - set_src")
    print("4  - set_net_rdo_url (CONFIG NET_RDO)")
    print("5  - config_net_srv_ip")
    print("6  - config_net_srv_port")
    print("7  - config_net_srv_format")
    print("8  - config_net_srv_rate")
    print("9  - config_bt_format")
    print("10 - config_bt_rate")
    print("11 - play (tylko gdy READY)")
    print("12 - stop")
    print("q  - quit")
    ch = input("wybierz> ").strip()

    if ch == "1":
        send_line(s, {"cmd":"get_state"})
    elif ch == "2":
        v = input("vol 0-100> ").strip()
        if v.isdigit(): send_line(s, {"cmd":"set_vol","value":int(v)})
    elif ch == "3":
        print("src: net_radio / net_server / bt")
        src = input("src> ").strip()
        send_line(s, {"cmd":"set_src","src":src})
    elif ch == "4":
        url = input("url> ").strip()
        send_line(s, {"cmd":"set_net_rdo_url","url":url})
    elif ch == "5":
        ip = input("ip> ").strip()
        send_line(s, {"cmd":"config_net_srv_ip","ip":ip})
    elif ch == "6":
        v = input("port> ").strip()
        if v.isdigit(): send_line(s, {"cmd":"config_net_srv_port","value":int(v)})
    elif ch == "7":
        v = input("format 0=PCM 1=MP3> ").strip()
        if v.isdigit(): send_line(s, {"cmd":"config_net_srv_format","value":int(v)})
    elif ch == "8":
        v = input("rate 44100/48000> ").strip()
        if v.isdigit(): send_line(s, {"cmd":"config_net_srv_rate","value":int(v)})
    elif ch == "9":
        v = input("bt format 0=PCM 1=MP3> ").strip()
        if v.isdigit(): send_line(s, {"cmd":"config_bt_format","value":int(v)})
    elif ch == "10":
        v = input("bt rate> ").strip()
        if v.isdigit(): send_line(s, {"cmd":"config_bt_rate","value":int(v)})
    elif ch == "11":
        send_line(s, {"cmd":"play"})
    elif ch == "12":
        send_line(s, {"cmd":"stop"})
    elif ch == "q":
        break
    else:
        print("zły wybór")

s.close()