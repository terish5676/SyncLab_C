import socket, base64, os, struct, hashlib, json, time, urllib.request
GUID="258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
def connect(path):
    s=socket.create_connection(("127.0.0.1",int(os.environ.get("PORT","8000")))); key=base64.b64encode(os.urandom(16)).decode()
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
    buf=b""
    while b"\r\n\r\n" not in buf: buf+=s.recv(1)
    head=buf.decode(); assert "101" in head.split("\r\n")[0], head
    exp=base64.b64encode(hashlib.sha1((key+GUID).encode()).digest()).decode(); assert exp in head, "bad accept"
    return s
def rd(s,n):
    b=b""
    while len(b)<n:
        c=s.recv(n-len(b)); 
        if not c: raise EOFError
        b+=c
    return b
def recv_frame(s):
    h=rd(s,2); op=h[0]&15; ln=h[1]&127
    if ln==126: ln=struct.unpack(">H",rd(s,2))[0]
    elif ln==127: ln=struct.unpack(">Q",rd(s,8))[0]
    return op, rd(s,ln)
def send_text(s,txt,op=1):
    d=txt.encode() if isinstance(txt,str) else txt; mask=os.urandom(4)
    hdr=bytes([0x80|op]); n=len(d)
    hdr+= bytes([0x80|n]) if n<126 else bytes([0x80|126])+struct.pack(">H",n)
    s.sendall(hdr+mask+bytes(b^mask[i%4] for i,b in enumerate(d)))
def api(p,body=None,m="POST"):
    r=urllib.request.Request("http://127.0.0.1:"+os.environ.get("PORT","8000")+""+p,data=json.dumps(body).encode() if body else None,method=m,headers={"Content-Type":"application/json"}); return json.loads(urllib.request.urlopen(r).read())

api("/api/simulation/create",dict(algorithm="writer_preference",reader_count=3,writer_count=2,read_duration_ms=200,write_duration_ms=200,stagger_arrivals_ms=30,max_duration_sec=20))
s=connect("/ws/events?last_seq=0")
op,pl=recv_frame(s); m=json.loads(pl); assert op==1 and m["type"]=="SNAPSHOT" and m["state"]["status"]=="IDLE" and len(m["state"]["processes"])==5; print("SNAPSHOT ok, bytes",len(pl))
# ping/pong
send_text(s,'{"type":"PING"}'); op,pl=recv_frame(s); assert json.loads(pl)["type"]=="PONG"; print("PONG ok")
# protocol-level ping
send_text(s,b"hi",op=9); op,pl=recv_frame(s); assert op==10 and pl==b"hi"; print("WS ping->pong ok")
# live events
api("/api/simulation/start")
evs=[]; s.settimeout(5)
while True:
    op,pl=recv_frame(s); m=json.loads(pl)
    if "seq" in m:
        evs.append(m)
        if m["event_type"]=="SIMULATION_COMPLETED": break
seqs=sorted(e["seq"] for e in evs); print("live events:",len(evs),"seq contiguous:",seqs==list(range(1,len(evs)+1)))
assert seqs==list(range(1,len(evs)+1)) and evs[0]["event_type"] in("SIMULATION_STARTED","PROCESS_REQUESTED")
# reconnect with replay
s2=connect("/ws/events?last_seq=%d"%(len(evs)-3)); s2.settimeout(3)
op,pl=recv_frame(s2); assert json.loads(pl)["type"]=="SNAPSHOT"
got=[]
for _ in range(3):
    op,pl=recv_frame(s2); got.append(json.loads(pl)["seq"])
print("replayed seqs",got); assert got==[len(evs)-2,len(evs)-1,len(evs)]
health=json.loads(urllib.request.urlopen("http://127.0.0.1:"+os.environ.get("PORT","8000")+"/api/health").read()); print("active_websockets",health["active_websockets"]); assert health["active_websockets"]==2
# close handshake
send_text(s,b"\x03\xe8",op=8); op,pl=recv_frame(s); assert op==8
s2.close(); time.sleep(0.3)
health=json.loads(urllib.request.urlopen("http://127.0.0.1:"+os.environ.get("PORT","8000")+"/api/health").read()); print("after close:",health["active_websockets"]); assert health["active_websockets"]==0
print("WS ALL OK")
