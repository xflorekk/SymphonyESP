#!/usr/bin/env python3
import socket
import sys
import time
import argparse

def main():
    parser = argparse.ArgumentParser(description='Test client dla ESP32 TCP JSON Lines')
    parser.add_argument('ip', help='IP ESP32 np. 192.168.1.50')
    parser.add_argument('-p', '--port', type=int, default=3333, help='Port (default: 3333)')
    parser.add_argument('-d', '--delay', type=float, default=1.0, help='Delay między liniami w s')
    args = parser.parse_args()

    # Przykładowe komendy pod Twoje net radio / MusicCast
    json_lines = [
        '{"cmd":"get_state"}\n',
        '{"cmd":"set_vol","value":20}\n',
        '{"cmd":"play","url":"http://ice1.somafm.com/groovesalad-128-mp3"}\n',
        '{"type":"now_playing","title":"Groove Salad","artist":"SomaFM"}\n',
        '{"cmd":"next"}\n',
    ]

    print(f"Łączenie do {args.ip}:{args.port}...")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect((args.ip, args.port))
        s.settimeout(0.5)
        print("Połączono! Wysyłam JSON Lines...\n")

        # odbierz welcome od serwera
        try:
            data = s.recv(1024)
            if data:
                print(f"<< RX welcome: {data.decode().strip()}")
        except socket.timeout:
            pass

        for line in json_lines:
            print(f">> TX: {line.strip()}")
            s.sendall(line.encode())

            # spróbuj odebrać echo / odpowiedź
            time.sleep(0.2)
            try:
                while True:
                    resp = s.recv(1024)
                    if not resp:
                        break
                    print(f"<< RX: {resp.decode().strip()}")
            except socket.timeout:
                pass

            time.sleep(args.delay)

        print("\nGotowe. Zamykam.")
        s.close()

    except Exception as e:
        print(f"Błąd: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
    input("Enter aby zamknąć...")

