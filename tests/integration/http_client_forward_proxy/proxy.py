# Minimal HTTP forward proxy for the aether#1012 regression.
# - Relays absolute-form GET requests to the origin, stamping X-Via-Proxy: yes.
# - Handles CONNECT with a real bidirectional tunnel (for the https path).
import socket, threading, sys, urllib.request

PORT = int(sys.argv[1])

def pipe(a, b):
    try:
        while True:
            d = a.recv(4096)
            if not d:
                break
            b.sendall(d)
    except Exception:
        pass
    finally:
        for s in (a, b):
            try:
                s.close()
            except Exception:
                pass

def handle(c):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = c.recv(4096)
        if not chunk:
            c.close(); return
        data += chunk
    line = data.split(b"\r\n", 1)[0].decode(errors="replace")
    parts = line.split(" ")
    method, target = parts[0], parts[1]
    if method == "CONNECT":
        host, port = target.split(":"); port = int(port)
        try:
            up = socket.create_connection((host, port), timeout=5)
        except Exception:
            c.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n"); c.close(); return
        c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        threading.Thread(target=pipe, args=(c, up), daemon=True).start()
        threading.Thread(target=pipe, args=(up, c), daemon=True).start()
        return
    try:
        req = urllib.request.Request(target, method=method)
        with urllib.request.urlopen(req, timeout=5) as r:
            body = r.read()
        resp = (b"HTTP/1.1 200 OK\r\nX-Via-Proxy: yes\r\n"
                b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body)) + body
    except Exception:
        resp = b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
    c.sendall(resp); c.close()

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", PORT))
s.listen(8)
sys.stderr.write("PROXY-READY\n"); sys.stderr.flush()
while True:
    conn, _ = s.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
