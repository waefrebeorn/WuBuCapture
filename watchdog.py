"""
WuBuCapture Watchdog — runs in background, monitors timing, NEVER kills the app.
Suggests optimizations. Can trigger rebuild+restart of worker via launcher.
"""
import csv
import time
import os
import subprocess
import sys
import json

BUILD_DIR = "C:/Users/eman5/WuBuCapture"
CSV_PATH = os.path.join(BUILD_DIR, "pipeline_timing.csv")
LOG_PATH = os.path.join(BUILD_DIR, "watchdog.log")

def log(msg):
    ts = time.strftime("%H:%M:%S")
    line = f"[{ts}] {msg}"
    print(line, flush=True)
    try:
        with open(LOG_PATH, "a") as f:
            f.write(line + "\n")
    except:
        pass

def analyze():
    rows = []
    try:
        with open(CSV_PATH, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                rows.append(row)
    except:
        return None
    
    if len(rows) < 10:
        return None
    
    recent = rows[-300:]
    pip, ci, ri, pw = [], [], [], []
    for r in recent:
        try:
            p = float(r['pipeline_ms'])
            c = float(r['cap_ms'])
            rend = float(r['rend_ms'])
            w = float(r['present_ms'])
            if 0 < p < 100: pip.append(p)
            if 0 < c < 50: ci.append(c)
            if 0 < rend < 50: ri.append(rend)
            if 0 <= w < 50: pw.append(w)
        except:
            pass
    
    def s(d):
        if not d: return {}
        d.sort()
        n = len(d)
        return {"avg": sum(d)/n, "med": d[n//2], "p99": d[int(n*0.99)] if n>100 else d[-1], "max": d[-1], "n": n}
    
    return {
        "pipeline": s(pip), "cap": s(ci), "render": s(ri), "present": s(pw),
        "total": len(rows)
    }

def check_running():
    """Check if any WuBu capture process is running"""
    try:
        result = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq WuBuCapture*.exe', '/FO', 'CSV'], 
                              capture_output=True, text=True)
        return 'WuBuCapture' in result.stdout
    except:
        return False

def main():
    log("=== WuBuCapture Watchdog Started ===")
    log(f"Monitoring: {CSV_PATH}")
    log("This process NEVER kills the capture app. It only reads and reports.")
    
    last_total = 0
    stall_count = 0
    
    while True:
        try:
            # Check if app is running
            if not check_running():
                log("⚠️ No WuBuCapture process detected! Waiting...")
                time.sleep(5)
                continue
            
            stats = analyze()
            if not stats:
                time.sleep(5)
                continue
            
            total = stats["total"]
            if total == last_total:
                stall_count += 1
                if stall_count > 10:
                    log("⚠️ No new frames for 50 seconds — app may be stalled")
                    stall_count = 0
            else:
                stall_count = 0
                last_total = total
            
            # Report every 30 seconds
            p = stats.get("pipeline", {})
            if p.get("n", 0) > 0:
                log(f"Pipeline: avg={p['avg']:.2f}ms med={p['med']:.2f}ms p99={p['p99']:.2f}ms | "
                    f"Cap: {stats['cap'].get('avg', 0):.1f}ms | "
                    f"Present: {stats['present'].get('avg', 0):.2f}ms max={stats['present'].get('max', 0):.1f}ms | "
                    f"Frames: {total}")
                
                # Warnings
                if p.get("avg", 0) > 3.0:
                    log(f"  🔴 Pipeline latency HIGH: {p['avg']:.2f}ms")
                if p.get("p99", 0) > 5.0:
                    log(f"  🟡 Pipeline p99 SPIKY: {p['p99']:.2f}ms")
                if stats["present"].get("avg", 0) > 5.0:
                    log(f"  🔴 Present wait HIGH: {stats['present']['avg']:.2f}ms (missing vsync)")
                if stats["render"].get("avg", 0) > 17.0:
                    log(f"  🔴 Render interval HIGH: {stats['render']['avg']:.2f}ms (<60fps)")
                
                if p.get("avg", 0) < 2.0 and stats["present"].get("avg", 0) < 2.0:
                    log(f"  🟢 OPTIMAL: pipeline {p['avg']:.2f}ms + present {stats['present']['avg']:.2f}ms")
            
            time.sleep(5)
            
        except KeyboardInterrupt:
            log("Watchdog stopped by user")
            break
        except Exception as e:
            log(f"Error: {e}")
            time.sleep(10)

if __name__ == "__main__":
    main()
