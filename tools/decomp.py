#!/usr/bin/env python3
"""Print pseudocode from a saved rea batch_decompile JSON. Usage: decomp.py <json> [maxchars-per-fn]"""
import json,sys,re
d=json.load(open(sys.argv[1])); m=int(sys.argv[2]) if len(sys.argv)>2 else 6000
for it in d['normalized_result']['items']:
    t=it.get('pseudocode') or ('ERR '+str(it.get('procedure',{}).get('status')))
    t=re.sub(r'\n\s*\n','\n',t)
    t=re.sub(r'  FUN_00569320\(&DAT_007a2838,[^\n]*\n','',t)
    print('=====',it['address']);print(t[:m])
