patterns = [b'8192', b'4096', b'8MB', b'4MB', b'8192k', b'4096k']
path = '.pio\\build\\esp32-s3-devkitc-1\\firmware.bin'
try:
    with open(path, 'rb') as f:
        data = f.read()
except FileNotFoundError:
    print('MISSING', path)
    raise SystemExit(1)
for p in patterns:
    off = data.find(p)
    if off != -1:
        print(p.decode(), hex(off))
