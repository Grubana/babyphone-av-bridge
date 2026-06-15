#!/usr/bin/env python3
import os
CT = {'.html':'text/html', '.js':'application/javascript', '.css':'text/css'}
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    web = os.path.join(here, 'web'); out = os.path.join(here, 'src', 'web_assets.h')
    entries = []
    for name in sorted(os.listdir(web)):
        p = os.path.join(web, name)
        if not os.path.isfile(p): continue
        data = open(p, 'rb').read()
        ct = CT.get(os.path.splitext(name)[1], 'application/octet-stream')
        route = '/' if name == 'index.html' else '/' + name
        arr = ','.join(str(b) for b in data)
        var = 'asset_' + name.replace('.', '_').replace('-', '_')
        entries.append((route, ct, var, data, arr))
    with open(out, 'w') as f:
        f.write('#pragma once\n#include <cstddef>\n\nnamespace webassets {\n')
        for route, ct, var, data, arr in entries:
            f.write(f'static const unsigned char {var}[] = {{{arr}}};\n')
        f.write('struct Asset { const char* route; const char* ctype; const unsigned char* data; size_t len; };\n')
        f.write('static const Asset kAssets[] = {\n')
        for route, ct, var, data, arr in entries:
            f.write(f'  {{"{route}", "{ct}", {var}, {len(data)}}},\n')
        f.write('};\nstatic const size_t kAssetCount = sizeof(kAssets)/sizeof(kAssets[0]);\n}\n')
    print(f"wrote {out} with {len(entries)} assets")
if __name__ == '__main__': main()
