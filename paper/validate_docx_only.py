"""Check current DOCX, source mathematics, tables, model arithmetic and preservation.

Reads protected PDF/TeX bytes for hashing only. Never renders or writes them.
"""
from pathlib import Path
from collections import Counter
from decimal import Decimal, localcontext
from itertools import combinations
import argparse,hashlib,json,math,re,statistics,unicodedata,zipfile
from lxml import etree as E
from math_native import mathml,omml

root=Path(__file__).resolve().parent
cli=argparse.ArgumentParser(description=__doc__)
cli.add_argument('--portable',action='store_true',help='Validate the article and frozen numerical inputs without local preservation inventory or Word page renders.')
args=cli.parse_args()
qa=root/('qa/portable' if args.portable else 'qa/continuation-20261009')
qa.mkdir(parents=True,exist_ok=True)
source=(root/'manuscript.md').read_text(encoding='utf-8')
ns={'w':'http://schemas.openxmlformats.org/wordprocessingml/2006/main','m':'http://schemas.openxmlformats.org/officeDocument/2006/math'}
maths=[(a,True) if a else (b,False) for a,b in re.findall(r'\$\$(.*?)\$\$|\$([^$]+)\$',source,re.S)]
def letters(s):
    return Counter(c for c in unicodedata.normalize('NFKC',s) if c.isalnum())
expected=[]
for expr,display in maths:
    before=letters(''.join(mathml(expr,display).itertext()))
    converted=omml(expr,display)
    after=letters(''.join(converted.xpath('.//m:t/text()',namespaces=ns)))
    assert before==after,expr
    expected.append(''.join(converted.xpath('.//m:t/text()',namespaces=ns)))
with zipfile.ZipFile(root/'ARWLF_Naman_PM.docx') as z:
    assert z.testzip() is None
    xml=E.fromstring(z.read('word/document.xml'))
    actual=[''.join(x.xpath('.//m:t/text()',namespaces=ns)) for x in xml.xpath('.//m:oMath',namespaces=ns)]
    missing=Counter(expected)-Counter(actual)
    assert not missing,missing
    # Display equations are display-mode paragraphs; LibreOffice draws floor/ceiling delimiter objects as slashes.
    assert len(xml.xpath('.//m:oMathPara',namespaces=ns))==sum(display for _,display in maths)
    assert not xml.xpath('.//m:d[m:dPr/m:begChr[@m:val="⌈" or @m:val="⌊"]]',namespaces=ns)
    visible=''.join(xml.xpath('.//w:t/text() | .//m:t/text()',namespaces=ns))
    for token in ('[PAPER TITLE]','[Author One]','[TABLE:','[FIGURE:','\\frac','\\pmod','\ufffd','`'):
        assert token not in visible,token
    keys=re.findall(r'^\[TABLE:([^]]+)\]$',source,re.M)
    payload=json.loads((root/'tables/tables.json').read_text(encoding='utf-8'))
    tables=xml.xpath('.//w:tbl',namespaces=ns)
    assert len(tables)==len(keys)==14
    row_counts={}
    for key,t in zip(keys,tables):
        found=[[''.join(cell.xpath('.//w:t/text()',namespaces=ns)) for cell in row.xpath('./w:tc',namespaces=ns)] for row in t.xpath('./w:tr',namespaces=ns)]
        wanted=[[str(v) for v in payload[key]['headers']]]+[[str(v) for v in r] for r in payload[key]['rows']]
        assert found==wanted,key
        row_counts[key]=len(found)-1
    figures=len(xml.xpath('.//w:drawing',namespaces=ns))
    assert figures==5
protected=None
if not args.portable:
    protected=json.loads((qa/'protected_files_before.json').read_text(encoding='utf-8'))
    after={}
    for rel,before in protected.items():
        p=root.parent/rel
        after[rel]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size}
        assert before==after[rel],rel
    current={str(p.relative_to(root.parent)) for p in root.parent.rglob('*') if p.is_file() and p.suffix.lower() in ('.pdf','.tex') and '.git' not in p.parts}
    assert current==set(protected),{'added':sorted(current-set(protected)),'removed':sorted(set(protected)-current)}
    (qa/'protected_files_after.json').write_text(json.dumps(after,indent=2),encoding='utf-8')
inp=json.loads((root/'data/scaling_inputs.json').read_text(encoding='utf-8'))
model=json.loads((root/'data/scaling_model.json').read_text(encoding='utf-8'))
for dim,v in zip((72,108),model['validation']):
    sizes=[x for c in inp[f'n{dim}_captures'] for x in c['l1']]
    assert math.isclose(statistics.mean(sizes),v['l1_measured_mean'],rel_tol=1e-14)
    assert 0<=v['byte_pass']<=1
    # Independent Poisson-MGF subset expansion at high precision, avoiding the
    # model's truncated probability sum and its small-probability cancellation.
    with localcontext() as ctx:
        ctx.prec=70
        lam=Decimal(str(v['leaf_occupancy']))
        alpha=[1-Decimal(1)/(Decimal(2)**b) for b in v['byte_filter_chunk_bits']]
        answer=Decimal(0)
        for size in range(len(alpha)+1):
            for subset in combinations(alpha,size):
                prod=math.prod(subset,start=Decimal(1))
                answer+=((-1)**size)*(lam*(prod-1)).exp()
        assert math.isclose(float(answer),v['byte_pass'],rel_tol=1e-11),(dim,str(answer),v['byte_pass'])
for p in model['n144']:
    assert 0<=p['byte_pass']<=1
    assert p['retuned_target_leaf_mean']<=13
    assert p['retuned_leaves_per_family']&(p['retuned_leaves_per_family']-1)==0
    assert math.isclose(p['retuned_frontier_hours'],p['root_pairs']*model['arwlf_ns_per_pair_0x1001']*1e-9/(630784*3600),rel_tol=1e-14)
    assert math.isclose(p['frozen_arwlf_frontier_years'],p['frozen_arwlf_core_years']/630784,rel_tol=1e-14)
assert model['native_applicability']['n144_applicable'] is False
abstract=source.split('## Abstract\n\n',1)[1].split('\n\nKeywords:',1)[0]
pages=None
if not args.portable:
    pages=json.loads((qa/'pages/pages.json').read_text(encoding='utf-8-sig'))
    assert all(p['Characters']>80 for p in pages)
report={'mode':'portable' if args.portable else 'local-preservation','source_body_math_checked':len(maths),'native_word_equations':len(actual),'math_character_loss':False,'all_body_expressions_present':True,'figures':figures,'tables':len(tables),'table_data_rows_verified':row_counts,'total_table_data_rows':sum(row_counts.values()),'abstract_words':len(abstract.split()),'word_rendered_pages':len(pages) if pages is not None else None,'empty_pages':[] if pages is not None else None,'poisson_byte_probabilities_independently_verified':True,'scaling_cost_units_verified':True,'protected_pdf_tex_files_unchanged':len(protected) if protected is not None else None,'new_pdf_tex_files':0 if protected is not None else None,'docx_sha256':hashlib.sha256((root/'ARWLF_Naman_PM.docx').read_bytes()).hexdigest()}
(qa/'docx_validation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
