"""Deterministic source, math preservation, package and render checks."""
from pathlib import Path
from collections import Counter
import re,json,zipfile,hashlib,unicodedata
from pypdf import PdfReader
from lxml import etree as E
from math_native import Parser,el,omml

root=Path(__file__).resolve().parent
s=(root/'manuscript.md').read_text(encoding='utf-8')
maths=[a or b for a,b in re.findall(r'\$\$(.*?)\$\$|\$([^$]+)\$',s,re.S)]
ns={'m':'http://schemas.openxmlformats.org/officeDocument/2006/math'}
def letters(s):
    return Counter(x for x in unicodedata.normalize('NFKC',s) if x.isalnum())
issues=[]
for expr in maths:
    clean=re.sub(r'\\tag\{\d+\}','',expr)
    mathml=el('math',None,Parser(clean).row())
    before=letters(''.join(mathml.itertext()))
    after=letters(''.join(x.text or '' for x in omml(clean).findall('.//m:t',ns)))
    if before!=after:issues.append({'expression':expr,'before':dict(before),'after':dict(after)})
assert not issues,issues
report={'body_math_expressions_checked':len(maths),'math_letter_digit_loss':issues,'source_words':len(s.split())}
with zipfile.ZipFile(root/'ARWLF_Naman_PM.docx') as z:
    xml=E.fromstring(z.read('word/document.xml'))
    report['native_word_equations']=len(xml.findall('.//m:oMath',ns))
    assert report['native_word_equations']>=len(maths)
    visible=''.join(xml.itertext())
    for token in ('[PAPER TITLE]','[Author One]','[TABLE:','[FIGURE:','\\frac','\\pmod'):
        assert token not in visible,token
for path in (root/'ARWLF_Naman_PM.pdf',root/'qa'/'word-render.pdf'):
    pdf=PdfReader(path);texts=[p.extract_text() or '' for p in pdf.pages]
    whole='\n\n'.join(texts)
    assert '\ufffd' not in whole
    assert '[TABLE:' not in whole and '[FIGURE:' not in whole
    assert 'Naman PM' in whole or 'NAMAN PM' in whole
    assert len(texts)>=15
    (root/'qa'/('acm-text.txt' if path.name!='word-render.pdf' else 'word-text.txt')).write_text(whole,encoding='utf-8')
    report[path.name]={'pages':len(texts),'text_characters':len(whole),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'empty_pages':[i+1 for i,t in enumerate(texts) if len(t)<80]}
    assert not report[path.name]['empty_pages']
log=(root/'qa'/'latex-pass-3.txt').read_text(encoding='utf-8',errors='replace')
for token in ('Overfull','Float too large','undefined','Fatal error','LaTeX Error'):
    assert token not in log,token
report['latex_final_pass']='success; no undefined citations, overfull boxes or oversized floats'
report['expected_warning']='Unassigned ACM publication reference block intentionally suppressed'
(root/'qa'/'article_validation.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf-8')
print(json.dumps(report,indent=2,ensure_ascii=False))
