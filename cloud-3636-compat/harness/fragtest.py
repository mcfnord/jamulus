import socket, sys, time
r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); r.bind(("127.0.0.1", 0)); r.settimeout(1.0); port = r.getsockname()[1]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
for n in (100, 1400, 1500, 5000):
    s.sendto(b"x"*n, ("127.0.0.1", port))
    try:
        d,_ = r.recvfrom(65535); print(n, "bytes sent -> received", len(d))
    except socket.timeout:
        print(n, "bytes sent -> LOST")
