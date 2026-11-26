import socket
import sys
import time
import threading
import argparse

class Client:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = None
        self.running = False
        self.rx_thread = None
        self.disconnected_by_server = False

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=5)
        self.sock.settimeout(0.5)
        self.running = True
        self.disconnected_by_server = False
        self.rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self.rx_thread.start()
        print(f"[+] connected {self.host}:{self.port}")

    def _rx_loop(self):
        buf = b""
        while self.running:
            try:
                chunk = self.sock.recv(1024)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                # recv == 0, serwer zamknął
                print("\n[!] SERVER DISCONNECTED (recv == 0, timeout lub close)")
                self.disconnected_by_server = True
                self.running = False
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(f"\n[RX] {line.decode(errors='ignore')}")
                print("> ", end="", flush=True)

    def send(self, text):
        if not self.running or not self.sock:
            print("[!] not connected")
            return
        try:
            self.sock.sendall((text + "\n").encode())
            print(f"[TX] {text}")
        except OSError as e:
            print(f"[!] send failed {e}, server probably closed")
            self.running = False

    def disconnect(self):
        print("[*] closing by client...")
        self.running = False
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass
        # daj chwilę żeby wątek RX się zakończył
        time.sleep(0.2)
        print("[+] disconnected by client")

    def test_idle(self, wait_sec):
        print(f"[*] idle test: czekam {wait_sec}s bez wysyłania...")
        print(f"    serwer ma idle_timeout_ms, jak przekroczysz, to on zamknie")
        start = time.time()
        while time.time() - start < wait_sec:
            if not self.running:
                print(f"[!] serwer rozłączył po {time.time()-start:.1f}s - TIMEOUT DZIAŁA")
                return
            time.sleep(0.5)
            print(f"    ... {int(time.time()-start)}s / {wait_sec}s, connected={self.running}")
        print(f"[+] minęło {wait_sec}s i dalej connected={self.running}")
        print("    jak connected=True, to timeout serwera jest większy niż wait")
        # spróbuj wysłać po idle, jak serwer zamknął, to send się wywali
        self.send("after idle")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True, help="IP z logu got ip: np. 10.0.1.233")
    p.add_argument("--port", type=int, default=3333)
    args = p.parse_args()

    print("=== SymphonyESP interactive TCP test ===")
    print("Komendy:")
    print("  /help          - pomoc")
    print("  /idle <sek>    - test timeout serwera, nic nie wysyła przez <sek>")
    print("  /disconnect    - zamknij połączenie (klient zamyka)")
    print("  /reconnect     - połącz ponownie")
    print("  /quit          - wyjście")
    print("  cokolwiek innego - wyślij jako linię do S3")
    print("")

    cli = Client(args.host, args.port)
    cli.connect()

    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\n[*] Ctrl+C, disconnect")
            cli.disconnect()
            break

        if line == "/quit":
            cli.disconnect()
            break
        elif line == "/help":
            print("  /idle 12 - czekaj 12s, zobacz czy serwer rozłączy (jak idle_timeout_ms=10000, to po 10s)")
            print("  /disconnect - zamknij, serwer powinien zalogować srv client disconnected")
            print("  /reconnect - połącz znowu, dostaniesz welcome")
        elif line.startswith("/idle"):
            try:
                sec = int(line.split()[1])
            except:
                sec = 12
            cli.test_idle(sec)
        elif line == "/disconnect":
            cli.disconnect()
        elif line == "/reconnect":
            if cli.running:
                cli.disconnect()
            cli.connect()
        elif line == "":
            continue
        else:
            cli.send(line)

if __name__ == "__main__":
    main()