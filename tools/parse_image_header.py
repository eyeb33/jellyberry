import sys

fn = sys.argv[1]
with open(fn, 'rb') as f:
    data = f.read(32)
if len(data) < 8:
    print('File too small')
    sys.exit(2)
# esp_image_header_t (esp-idf v3/v4) layout:
# 0: magic (0xE9)
# 1: segment count
# 2: spi_mode_and_size (bits: 0-3 speed, 4-6 mode, 7-? size?)
# For many images: byte 2 encodes flash_size in lower nibble in custom mapping used by esptool

b = list(data)
print('Header bytes:', ' '.join(f'{x:02x}' for x in b[:16]))
magic = b[0]
seg_count = b[1]
spi_byte = b[2]
print(f'Magic: 0x{magic:02x}')
print(f'Segment count: {seg_count}')
print(f'Raw spi byte: 0x{spi_byte:02x} ({spi_byte:08b})')

# Common ESP32 mapping for "spi size" (esp_image): lower 3 bits (0..2) select flash size
# But different toolchains sometimes set different bits. We'll decode common values:
size_map = {
    0: '1MB',
    1: '2MB',
    2: '4MB',
    3: '8MB',
    4: '16MB',
    5: '32MB'
}

# Try several plausible decodings
low3 = spi_byte & 0x7
low4 = spi_byte & 0xf
high3 = (spi_byte >> 4) & 0x7
print('Decoded candidates:')
print(' - lower 3 bits ->', low3, size_map.get(low3, 'unknown'))
print(' - lower 4 bits ->', low4, size_map.get(low4, 'unknown'))
print(' - upper 3 bits ->', high3, size_map.get(high3, 'unknown'))

# If available, also look for string occurrences that mention sizes
with open(fn, 'rb') as f:
    content = f.read()
for s in [b'4096', b'8192', b'8MB', b'4MB', b'8192k', b'4096k']:
    if s in content:
        print('Found ascii marker:', s.decode(errors='ignore'))

print('\nDone')
