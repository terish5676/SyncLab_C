import json, time, urllib.request, urllib.error, socket, base64, os, struct, hashlib
import os
B="http://127.0.0.1:%s"%os.environ.get("PORT","8000")
def req(path, body=None, method=None, raw=False, headers=None):
    h={"Content-Type":"application/json"}; h.update(headers or {})
    data = json.dumps(body).encode() if isinstance(body,(dict,list)) else (body.encode() if isinstance(body,str) else None)
    r = urllib.request.Request(B+path, data=data, method=method or ("POST" if data is not None else "GET"), headers=h)
    try:
        with urllib.request.urlopen(r) as x: b=x.read(); return x.status, (b if raw else json.loads(b)), dict(x.headers)
    except urllib.error.HTTPError as e:
        b=e.read(); 
        try: return e.code, json.loads(b), dict(e.headers)
        except Exception: return e.code, b, dict(e.headers)
def post(p): return req(p, method="POST")
cfg=dict(algorithm="fair_fifo",reader_count=4,writer_count=2,read_duration_ms=800,write_duration_ms=800,initial_value=5,writer_op="MULTIPLY",writer_amount=3,max_duration_sec=30,stagger_arrivals_ms=100)

# --- pause / resume keeps leases, no progress while paused
req("/api/simulation/create",cfg); post("/api/simulation/start"); time.sleep(0.5)
s,st,_=post("/api/simulation/pause"); assert st["status"]=="PAUSED"
p1={p["id"]:p["progress_pct"] for p in st["processes"]}; time.sleep(0.6)
s,st2,_=req("/api/simulation/state"); p2={p["id"]:p["progress_pct"] for p in st2["processes"]}
assert p1==p2, "progress moved while paused"
assert st2["status"]=="PAUSED"
e1=st2["metrics"]["total_elapsed_ms"]; time.sleep(0.3); e2=req("/api/simulation/state")[1]["metrics"]["total_elapsed_ms"]
assert abs(e1-e2)<5, f"elapsed moved while paused {e1} {e2}"
s,st,_=post("/api/simulation/resume"); assert st["status"]=="RUNNING"
while req("/api/simulation/state")[1]["status"]=="RUNNING": time.sleep(0.05)
st=req("/api/simulation/state")[1]
assert st["status"]=="COMPLETED" and st["resource_value"]==45 and st["resource_version"]==2, st["resource_value"]
fin=st["metrics"]["total_elapsed_ms"]; time.sleep(0.4); assert req("/api/simulation/state")[1]["metrics"]["total_elapsed_ms"]==fin, "elapsed not frozen"
print("pause/resume OK, MULTIPLY 5*3*3 =",st["resource_value"])

# --- start after COMPLETED auto-resets and reruns
s,st,_=post("/api/simulation/start"); assert st["status"]=="RUNNING" and st["resource_version"]==0 and st["total_events"]<=6, st["total_events"]
time.sleep(0.4)
# --- stop mid-run
s,st,_=post("/api/simulation/stop"); assert st["status"]=="CANCELLED"
time.sleep(0.4)
st=req("/api/simulation/state")[1]
states={p["state"] for p in st["processes"]}; print("after stop states:",sorted(states)); assert states<= {"COMPLETED","CANCELLED"}
assert not st["active_reader_ids"] and st["active_writer_id"] is None
# --- reset
s,st,_=post("/api/simulation/reset"); assert st["status"]=="IDLE" and st["resource_version"]==0 and st["total_events"]==1 and st["recent_events"][0]["event_type"]=="SIMULATION_RESET" and st["recent_events"][0]["seq"]==1
print("stop/reset OK")

# --- validation (FastAPI-style 422)
s,b,_=req("/api/simulation/create",{"reader_count":0,"writer_count":51,"algorithm":"x","read_duration_ms":"abc","writer_amount":1.5}); print(s,json.dumps(b)[:400]); assert s==422 and len(b["detail"])==5
s,b,_=req("/api/simulation/create","not json"); assert s==422 and b["detail"][0]["loc"]==["body",0]
s,b,_=req("/api/simulation/create",{}); assert s==200 and b["config"]["reader_count"]==6
s,b,_=req("/api/simulation/create",{"reader_count":"7"}); assert s==200 and b["config"]["reader_count"]==7
s,b,_=req("/api/simulation/events?limit=0"); assert s==422 and b["detail"][0]["loc"]==["query","limit"]
s,b,_=req("/api/simulation/events?process_type=BOGUS"); assert s==422
s,b,_=req("/api/simulation/state",method="POST"); assert s==405
s,b,_=req("/api/simulation/events/export?format=xml"); assert s==422
print("validation OK")

# --- events filtering + exports
req("/api/simulation/create",dict(cfg,algorithm="reader_preference",reader_count=3,writer_count=1,read_duration_ms=100,write_duration_ms=100,stagger_arrivals_ms=0)); post("/api/simulation/start"); time.sleep(1.2)
s,ev,_=req("/api/simulation/events?min_seq=0&limit=2000"); n=len(ev)
s,evw,_=req("/api/simulation/events?process_type=WRITER"); assert evw and all(e["process_type"]=="WRITER" for e in evw)
s,evt,_=req("/api/simulation/events?event_type=SIMULATION_STARTED"); assert len(evt)==1
s,evm,_=req("/api/simulation/events?min_seq=%d"%(n-2)); assert len(evm)==2
s,csvb,h=req("/api/simulation/events/export?format=csv",raw=True); txt=csvb.decode(); print(h["Content-Type"],h["Content-Disposition"]); print(txt.split("\r\n")[0]); print(txt.split("\r\n")[2]); assert txt.count("\r\n")==n+1
s,jb,h=req("/api/simulation/events/export?format=json"); assert len(jb)==n and "attachment" in h["Content-Disposition"]
s,res,_=req("/api/simulation/results"); assert res["final_resource_version"]==1 and set(res)>={"simulation_id","status","algorithm","config","final_resource_value","final_resource_version","metrics","processes","total_events"}
print("events/export/results OK",n,"events")

# --- compare
t=time.time(); s,c,_=req("/api/simulation/compare",dict(cfg,reader_count=5,writer_count=2,read_duration_ms=200,write_duration_ms=200,stagger_arrivals_ms=30,writer_op="ADD",writer_amount=1))
assert s==200 and set(c["comparison"])=={"reader_preference","writer_preference","fair_fifo"}
for k,v in c["comparison"].items(): print(k, v["final_value"],v["final_version"],v["total_events"], v["metrics"]["avg_reader_wait_ms"], v["metrics"]["avg_writer_wait_ms"]); assert v["final_version"]==2 and v["total_events"]==23
print("compare OK %.1fs"%(time.time()-t))

# --- CORS
s,b,h=req("/api/health",headers={"Origin":"http://localhost:5173"}); assert h["Access-Control-Allow-Origin"]=="http://localhost:5173"
import http.client
c=http.client.HTTPConnection("127.0.0.1",int(os.environ.get("PORT","8000"))); c.request("OPTIONS","/api/simulation/create",headers={"Origin":"http://x.y","Access-Control-Request-Method":"POST","Access-Control-Request-Headers":"content-type"}); r=c.getresponse(); print("preflight",r.status,r.getheader("Access-Control-Allow-Headers")); assert r.status==200
print("ALL OK")
