#!/usr/bin/env python3
import hashlib,json,pathlib
p=pathlib.Path(__file__).resolve().parents[1]/"validation/baseline"
m=json.loads((p/"manifest.json").read_text())
actual=hashlib.sha256((p/m["apk"]).read_bytes()).hexdigest()
if actual!=m["sha256"]: raise SystemExit("FAIL: working APK changed")
print("PASS: working APK SHA256",actual)
