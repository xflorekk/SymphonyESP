# Test - overflow, split segments, 

import socket, sys, time

IP = sys.argv[1] if len(sys.argv) > 1 else "10.0.1.233"
PORT = 3333

def recv_line(s, timeout=2):
    s.settimeout(timeout)
    buf = b""
    try:
        while not buf.endswith(b"\n"):
            chunk = s.recv(1024)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf

def check(name, got, want):
    ok = want in got if isinstance(want, bytes) else got == want
    print(f"{'PASS' if ok else 'FAIL'} {name}: got={got!r} want={want!r}")
    return ok

s = socket.create_connection((IP, PORT), timeout=5)
print("welcome:", recv_line(s))

# 1. Normalna linia
s.sendall(b"HELLO\n")
check("HELLO", recv_line(s), b"echo: HELLO")

# 2. Pusta linia - STARY BUG: brak echa, NOWE: echo: \n
s.sendall(b"\n")
check("EMPTY LINE", recv_line(s), b"echo: \n")

# 3. Za długa linia, 2000 x A, ma być zrzucona, bez echa
s.sendall(b"A" * 2000 + b"\n")
got = recv_line(s, timeout=1)
print(f"{'PASS' if got == b'' else 'FAIL'} LONG LINE DROPPED: got={got!r} want=timeout")
# Następna linia po długiej MUSI przejść, bufor nie zablokowany
s.sendall(b"AFTER\n")
check("AFTER LONG", recv_line(s), b"echo: AFTER")

# 4. Dwie linie w jednym pakiecie
s.sendall(b"LINE1\nLINE2\n")
check("LINE1", recv_line(s), b"echo: LINE1")
check("LINE2", recv_line(s), b"echo: LINE2")

# 5. Linia podzielona na dwa pakiety
s.sendall(b"HEL")
time.sleep(0.2)
s.sendall(b"LO\n")
check("PARTIAL", recv_line(s), b"echo: HELLO")

# 6. Idle timeout 10s, serwer ma zamknąć
print("czekam 11s na idle timeout...")
time.sleep(11)
got = recv_line(s, timeout=2)
print(f"{'PASS' if got == b'' else 'FAIL'} IDLE TIMEOUT: got={got!r} want=closed")
s.close()
print("KONIEC")