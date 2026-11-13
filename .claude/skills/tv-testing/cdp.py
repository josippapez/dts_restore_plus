#!/usr/bin/env python3
"""usage: cdp.py 'JS expression' -- evaluate it in the DTS Enabler page on the TV.

Needs the inspector tunnel (ssh -N -L 19998:127.0.0.1:9998 webos-tv) and
websocket-client (pip install websocket-client)."""
import json, sys, urllib.request
import websocket

page = [p for p in json.load(urllib.request.urlopen("http://127.0.0.1:19998/json/list", timeout=10))
        if p.get("title") == "DTS Enabler"][0]
ws = websocket.create_connection(page["webSocketDebuggerUrl"].replace("127.0.0.1:9998", "127.0.0.1:19998"), timeout=15)
ws.send(json.dumps({"id": 1, "method": "Runtime.evaluate",
                    "params": {"expression": sys.argv[1], "returnByValue": True, "awaitPromise": True}}))
while True:
    msg = json.loads(ws.recv())
    if msg.get("id") == 1:
        res = msg.get("result", {})
        print(json.dumps(res.get("result", {}).get("value", res), indent=1))
        break
ws.close()
