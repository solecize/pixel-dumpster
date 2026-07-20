# pd-ble-bridge

BLE Nordic UART ↔ local TCP bridge for pixel-dumpster NDJSON (same protocol as USB serial).

## Install

```bash
python3 -m pip install -r requirements.txt
```

## Run

```bash
python3 pd_ble_bridge.py --name pixel-dumpster --port 9877
```

Then point dumpster-diver at the bridge (see dumpster-diver `--ble-bridge` / config), or smoke-test:

```bash
printf '%s\n' '{"cmd":"status"}' | nc 127.0.0.1 9877
```
