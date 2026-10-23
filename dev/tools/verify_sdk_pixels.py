"""Compare production SDK RGBA against saved CLI PNGs, including output guards."""
import argparse, hashlib, json
from pathlib import Path
from PIL import Image
from compare_sdk_work import Decoder

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dll',type=Path,required=True)
    p.add_argument('--packets',type=Path,nargs='+',required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args(); records=[]
    for folder in a.packets:
        for path in sorted([*folder.glob('*.fajkv'),*folder.glob('*.fajk')]):
            expected=path.with_suffix('.png')
            if not expected.exists(): continue
            pixels=Image.open(expected).convert('RGBA')
            digest=hashlib.sha256(pixels.tobytes()).hexdigest()
            for workers in (1,0):
                decoder=Decoder(a.dll,path,workers)
                try:
                    assert pixels.size==(decoder.info.width,decoder.info.height)
                    decoder.decode(); actual=decoder.digest()
                    assert actual==digest, f'SDK pixels differ: {path} / {workers}'
                    records.append(dict(path=str(path),workers=workers,sha256=actual,guard_ok=True))
                finally: decoder.close()
    assert records,'No matching packet/PNG pairs'
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(records,indent=2),encoding='utf-8')
    print(f'{len(records)} SDK decodes: exact CLI pixels and intact guards')
if __name__=='__main__': main()
