"""Rasterize Word's page EMFs with the native Windows GDI path used by Pillow.

GDI+ DrawImage can add apparent heavy outlines on transparent text; native
GDI preserves the Word text weights. No document, PDF or TeX is opened here.
"""
from pathlib import Path
import argparse,json
from PIL import Image

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory',type=Path)
args=parser.parse_args()
pages=json.loads((args.directory/'pages.json').read_text(encoding='utf-8-sig'))
assert hasattr(Image.core,'drawwmf'),'Native Windows EMF rasterizer unavailable'
for page in pages:
    stem=args.directory/f"page-{page['Page']:02d}"
    with Image.open(stem.with_suffix('.emf')) as im:
        original_dpi=im.info['dpi']
        if not isinstance(original_dpi,tuple):original_dpi=(original_dpi,original_dpi)
        # Use the page's Letter aspect and exact requested pixel dimensions.
        target=(page['Width'],page['Height'])
        dpi=tuple((desired+1e-5)*resolution/current for desired,resolution,current in zip(target,original_dpi,im.size))
        im.load(dpi=dpi)
        assert im.size==target,(im.size,target)
        im.save(stem.with_suffix('.png'))
print(f"Native GDI rasterized {len(pages)} Word page EMFs; table body font weight retained.")
