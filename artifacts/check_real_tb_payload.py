import glob, os
root = os.path.expanduser("~/koi-bot/engines/syzygy")
files = sorted(glob.glob(os.path.join(root, "*")))
tables = [f for f in files if f.endswith((".rtbw", ".rtbz"))]
bad = []
for path in tables:
    with open(path, "rb") as stream:
        head = stream.read(64)
    if len(head) <= 4 or not any(head[4:]):
        bad.append(os.path.basename(path))
print("tables", len(tables), "zero_payload", len(bad), bad[:5])
