"""Read pixel colours out of a PNG.

Used to check the GUI against the Figma render: export a frame, sample the
same point in both, compare. There is no PIL in this environment and adding
one for six lines of colour-picking is not worth it, so this decodes the PNG
itself -- enough of the format for what Figma and JUCE actually emit.

    python3 tools/pngpick.py shot.png 640,140 60,700

"""

import sys, zlib, struct

def load(path):
    d = open(path,'rb').read(); assert d[:8]==b'\x89PNG\r\n\x1a\n'
    i, idat, pal = 8, b'', None
    while i < len(d):
        ln, typ = struct.unpack('>I4s', d[i:i+8]); body = d[i+8:i+8+ln]
        if typ==b'IHDR': w,h,bd,ct = struct.unpack('>IIBB', body[:10])
        elif typ==b'IDAT': idat += body
        elif typ==b'PLTE': pal = body
        i += 12+ln
    raw = zlib.decompress(idat)
    ch = {0:1,2:3,3:1,4:2,6:4}[ct]; bpp = ch*bd//8; stride = w*bpp
    out, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p+1:p+1+stride]); p += 1+stride
        for x in range(stride):
            a = line[x-bpp] if x>=bpp else 0; b = prev[x]; c = prev[x-bpp] if x>=bpp else 0
            if f==1: line[x]=(line[x]+a)&255
            elif f==2: line[x]=(line[x]+b)&255
            elif f==3: line[x]=(line[x]+(a+b)//2)&255
            elif f==4:
                pa=abs(b-c); pb=abs(a-c); pc=abs(a+b-2*c)
                pr = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[x]=(line[x]+pr)&255
        out.append(bytes(line)); prev = line
    return w,h,ch,pal,out

def px(path, x, y):
    w,h,ch,pal,rows = load(path)
    row = rows[y]
    if pal is not None:
        idx = row[x]; return tuple(pal[idx*3:idx*3+3])
    return tuple(row[x*ch:x*ch+3])

if __name__ == '__main__':
    path = sys.argv[1]
    for pair in sys.argv[2:]:
        x,y = map(int, pair.split(','))
        print("(%s,%s) -> #%02X%02X%02X" % ((x,y)+px(path,x,y)))
