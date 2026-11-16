import json, sys, time, urllib.request
import os
B="http://127.0.0.1:%s"%os.environ.get("PORT","8000")
def call(path, body=None, method=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(B+path, data=data, method=method or ("POST" if body is not None or path.endswith(("start","pause","resume","stop","reset")) else "GET"), headers={"Content-Type":"application/json"})
    try:
        with urllib.request.urlopen(req) as r: return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())
for algo in ["reader_preference","writer_preference","fair_fifo"]:
    cfg=dict(algorithm=algo,reader_count=8,writer_count=3,read_duration_ms=300,write_duration_ms=400,initial_value=100,writer_op="ADD",writer_amount=10,max_duration_sec=30,stagger_arrivals_ms=50)
    s,st=call("/api/simulation/create",cfg); assert s==200 and st["status"]=="IDLE"
    s,st=call("/api/simulation/start","" and None) if False else (None,None)
    req=urllib.request.Request(B+"/api/simulation/start",method="POST"); st=json.loads(urllib.request.urlopen(req).read())
    assert st["status"]=="RUNNING"
    t0=time.time()
    while True:
        st=json.loads(urllib.request.urlopen(B+"/api/simulation/state").read())
        # live invariant check
        assert not (st["active_reader_ids"] and st["active_writer_id"]), "VIOLATION"
        if st["status"]!="RUNNING": break
        time.sleep(0.05)
    ev=json.loads(urllib.request.urlopen(B+"/api/simulation/events?limit=2000").read())
    errs=[e for e in ev if e["event_type"]=="INVARIANT_VIOLATION"]
    m=st["metrics"]
    print(f"{algo:18s} status={st['status']} value={st['resource_value']} ver={st['resource_version']} reads={m['completed_reads']}/{m['total_readers']} writes={m['completed_writes']}/{m['total_writers']} avgR={m['avg_reader_wait_ms']} avgW={m['avg_writer_wait_ms']} maxW={m['max_writer_wait_ms']} elapsed={m['total_elapsed_ms']}ms occ={m['resource_occupancy_pct']}% events={st['total_events']} violations={len(errs)} wall={time.time()-t0:.2f}s")
    assert st["status"]=="COMPLETED" and st["resource_value"]==130 and st["resource_version"]==3 and not errs
    seqs=[e["seq"] for e in ev]; assert seqs==sorted(seqs)==list(range(1,len(ev)+1))
print("ALL OK")
