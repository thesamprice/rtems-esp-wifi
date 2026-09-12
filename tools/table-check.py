# Diffs the preprocessed struct's members against the table's initialisers.
# Takes the source path so the negative control can point it at a mutated copy.
import re, sys
import os
SP=os.environ.get('SP', os.path.join(os.path.dirname(__file__),'..','..'))
src=open(sys.argv[1] if len(sys.argv)>1 else f'{SP}/glue/src/rtems_wifi_os_adapter.c').read()
t=open(f'{SP}/memprobe.i').read()
body=re.search(r'typedef\s+struct\s*\{(.*?)\}\s*wifi_osi_funcs_t', t, re.S).group(1)
active=re.findall(r'\(\s*\*\s*(_\w+)\s*\)', body)+re.findall(r'^\s*(?:int|int32_t)\s+(_\w+)\s*;', body, re.M)
init=re.findall(r'\.(_\w+)\s*=', re.search(r'wifi_osi_funcs_t g_wifi_osi_funcs = \{(.*?)\n\};', src, re.S).group(1))
miss=sorted(set(active)-set(init)); extra=sorted(set(init)-set(active))
dups=sorted(x for x in set(init) if init.count(x)>1)
print(f"members {len(active)}  initialised {len(init)}  missing {miss or '-'}  extra {extra or '-'}  dup {dups or '-'}")
sys.exit(0 if not (miss or extra or dups) else 1)
