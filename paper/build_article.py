"""Build a reference-cloned DOCX; --docx-only never opens or writes TeX."""
from pathlib import Path
from copy import deepcopy
import re,json,zipfile,hashlib,argparse
from docx import Document
from docx.shared import Inches,Pt,RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT,WD_CELL_VERTICAL_ALIGNMENT
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from lxml import etree as E
from math_native import omml

ROOT=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--docx-only',action='store_true',help='Preserve TeX and PDF files completely')
args=parser.parse_args()
REFERENCE=Path(r'C:\Users\naman\.codex\skills\artifact-template-theory-focused-research-paper\assets\reference.docx')
if (ROOT/'template'/'reference.docx').exists():REFERENCE=ROOT/'template'/'reference.docx'
assert hashlib.sha256(REFERENCE.read_bytes()).hexdigest()=='22c6531e9c00c0218533be28ce2e7def3fc08ca6244a81ce1d9e6d216a10a7a7'
text=(ROOT/'manuscript.md').read_text(encoding='utf-8')
refs=json.loads((ROOT/'references.json').read_text(encoding='utf-8'))
used=set(re.findall(r'\[@([^\]]+)\]',text))
bykey={r['key']:r for r in refs}
assert used<=bykey.keys(),used-bykey.keys()
# A shared explicit citation sequence makes PDF/DOCX numbering identical.
refs=sorted([bykey[k] for k in used],key=lambda r:(r['authors'][0]['family'].casefold(),r['fields']['year'],r['fields']['title']))
nums={r['key']:i+1 for i,r in enumerate(refs)}
tables=json.loads((ROOT/'tables'/'tables.json').read_text(encoding='utf-8'))
figs=json.loads((ROOT/'figures'/'figures.json').read_text(encoding='utf-8'))
for table in tables.values():
    if isinstance(table.get('notes'),str):table['notes']=[table['notes']]

def texescape(s):
    return ''.join({'&':r'\&','%':r'\%','$':r'\$','#':r'\#','_':r'\_','{':r'\{','}':r'\}','~':r'\textasciitilde{}','^':r'\textasciicircum{}','\\':r'\textbackslash{}'}.get(c,c) for c in str(s))
def bibplain(r):
    f=r['fields'];names=[]
    for a in r['authors']:names.append((a.get('given','')+' '+a['family']).strip())
    author=', '.join(names[:-1])+(' and ' if len(names)>1 else '')+names[-1]
    venue=f.get('journal',f.get('booktitle',''))
    detail=venue
    if f.get('volume'):detail+=' '+f['volume']
    if f.get('number'):detail+='('+f['number']+')'
    if f.get('pages'):detail+=', '+f['pages'].replace('--','–')
    return f"{author}. {f['year']}. {f['title'].strip('{}')}. {detail}."

def inline_tex(s):
    parts=re.split(r'(\$[^$]+\$|\[@[^\]]+\]|\*\*[^*]+\*\*)',s)
    out=[]
    for p in parts:
        if p.startswith('$'):out.append(p)
        elif p.startswith('[@'):out.append(r'\cite{'+p[2:-1]+'}')
        elif p.startswith('**'):out.append(r'\textbf{'+texescape(p[2:-2])+'}')
        else:
            escaped=texescape(p)
            escaped=re.sub(r'\b[0-9a-f]{40}\b',lambda m:r'\texttt{'+r'\allowbreak{}'.join(m[0][i:i+8] for i in range(0,40,8))+'}',escaped)
            out.append(escaped)
    return ''.join(out)

def bibliography_tex():
    result=[r'\begin{thebibliography}{99}']
    for r in refs:
        f=r['fields'];result.extend([r'\bibitem{'+r['key']+'}',inline_tex(bibplain(r))])
        if f.get('doi'):result.append(r'\url{https://doi.org/'+f['doi']+'}.')
        elif f.get('url'):result.append(r'\url{'+f['url']+'}.')
    return result+[r'\end{thebibliography}']

def blocks():
    lines=text.splitlines();i=0
    while i<len(lines):
        s=lines[i].strip()
        if not s:i+=1;continue
        if s=='$$':
            j=i+1
            while lines[j].strip()!='$$':j+=1
            yield ('math','\n'.join(lines[i+1:j]));i=j+1
        elif s.startswith('```'):
            j=i+1
            while not lines[j].startswith('```'):j+=1
            yield ('code','\n'.join(lines[i+1:j]));i=j+1
        elif s.startswith('#'):
            m=re.match(r'(#+) (.*)',s);yield ('head'+str(len(m[1])),m[2]);i+=1
        elif s.startswith('[TABLE:') or s.startswith('[FIGURE:'):
            typ,key=s[1:-1].split(':');yield(typ.lower(),key);i+=1
        else:
            ps=[s];i+=1
            while i<len(lines) and lines[i].strip() and not lines[i].startswith(('#','$$','```','[TABLE:','[FIGURE:')):
                ps.append(lines[i].strip());i+=1
            yield('para',' '.join(ps))

bs=list(blocks());title=bs[0][1]
abstract=next(bs[i+1][1] for i,b in enumerate(bs) if b==('head2','Abstract'))
keywords=next(v.split(':',1)[1].strip().rstrip('.') for k,v in bs if k=='para' and v.startswith('Keywords:'))

tex=[r'\documentclass[manuscript,screen,review]{acmart}',r'\usepackage[T1]{fontenc}',r'\usepackage{tabularx}',r'\usepackage{listings}',r'\setcopyright{none}',r'\settopmatter{printacmref=false}',r'\renewcommand\footnotetextcopyrightpermission[1]{}',r'\acmDOI{}',r'\acmISBN{}',r'\acmYear{}',r'\acmConference[]{}{}{}',r'\title{'+texescape(title)+'}',r'\author{Naman PM}',r'\renewcommand{\shortauthors}{Naman PM}',r'\begin{document}',r'\begin{abstract}',inline_tex(abstract),r'\end{abstract}',r'\ccsdesc[500]{Theory of computation~Design and analysis of algorithms}',r'\keywords{'+texescape(keywords)+'}',r'\maketitle']
tex.append(r'\raggedbottom')
appendix=False;started=False;fig_no=0;table_no=0
for k,v in bs:
    if k=='head2' and v=='1 Introduction':started=True
    if not started:continue
    if k.startswith('head'):
        if v.startswith('Appendix '):
            if not appendix:
                tex.extend([r'\clearpage']+bibliography_tex()+[r'\clearpage',r'\appendix'])
                appendix=True
            title2=re.sub(r'^Appendix [A-Z] ','',v)
        else:title2=re.sub(r'^\d+(?:\.\d+)*\s+','',v)
        tex.append(('\\section' if k=='head2' else '\\subsection')+'{'+texescape(title2)+'}')
    elif k=='para':tex.extend([inline_tex(v),''])
    elif k=='math':tex.extend([r'\begin{equation*}',v,r'\end{equation*}'])
    elif k=='code':tex.extend([r'\noindent\begin{minipage}{\linewidth}',r'\begin{lstlisting}[basicstyle=\ttfamily\small,breaklines=true,columns=fullflexible,keepspaces=true]',v,r'\end{lstlisting}',r'\end{minipage}'])
    elif k=='figure':
        f=figs[v];fig_no+=1
        tex.extend([r'\begin{figure}[htbp]',r'\centering',r'\includegraphics[width=\linewidth]{figures/'+v+'.pdf}',r'\caption{'+inline_tex(f['caption'])+'}',r'\Description{'+texescape(f.get('description',f['caption']))+'}',r'\label{fig:'+v+'}',r'\end{figure}'])
    elif k=='table':
        t=tables[v];table_no+=1;n=len(t['headers'])
        spec='@{}'+r'>{\raggedright\arraybackslash}X'+r'>{\centering\arraybackslash}X'*(n-1)+'@{}'
        tex.extend([r'\begin{table}[htbp]',r'\caption{'+inline_tex(t['caption'])+'}',r'\label{tab:'+v+'}',r'\small',r'\begin{tabularx}{\linewidth}{'+spec+'}',r'\toprule',' & '.join(r'\textbf{'+texescape(x)+'}' for x in t['headers'])+r' \\',r'\midrule'])
        tex.extend(' & '.join(texescape(x) for x in row)+r' \\' for row in t['rows'])
        tex.extend([r'\bottomrule',r'\end{tabularx}'])
        for note in t.get('notes',[]):tex.append(r'\par\smallskip\footnotesize '+inline_tex(note))
        tex.append(r'\end{table}')
if not appendix:tex.extend(bibliography_tex())
tex.append(r'\end{document}')
if not args.docx_only:
    (ROOT/'manuscript.tex').write_text('\n'.join(tex),encoding='utf-8')

# Clone the retained reference and reuse its direct title/author properties.
doc=Document(REFERENCE)
title_pattern=deepcopy(doc.paragraphs[0]._p)
author_pattern=deepcopy(doc.paragraphs[2]._p)
body=doc._element.body
for x in list(body):
    if x.tag!=qn('w:sectPr'):body.remove(x)
def newp(style='Normal',pattern=None):
    p=doc.add_paragraph(style=style)
    if pattern is not None:
        prop=pattern.find(qn('w:pPr'))
        if prop is not None:
            if p._p.pPr is not None:p._p.remove(p._p.pPr)
            p._p.insert(0,deepcopy(prop))
    return p

math_count=0
def inline(p,s):
    global math_count
    for part in re.split(r'(\$[^$]+\$|\[@[^\]]+\]|\*\*[^*]+\*\*|`[^`]+`)',s):
        if not part:continue
        if part.startswith('$'):
            p._p.append(omml(part[1:-1]));math_count+=1
        elif part.startswith('[@'):p.add_run('['+str(nums[part[2:-1]])+']')
        elif part.startswith('**'):p.add_run(part[2:-2]).bold=True
        elif part.startswith('`'):p.add_run(part[1:-1]).font.name='Consolas'
        else:p.add_run(part)
    p.paragraph_format.widow_control=True
def caption(s,keep_next=False):
    p=newp('Caption');inline(p,s)
    p.paragraph_format.keep_with_next=keep_next
    p.paragraph_format.space_before=Pt(6);p.paragraph_format.space_after=Pt(6)
    return p
def add_table(key,num):
    t=tables[key]
    caption('Table '+str(num)+'. '+t['caption'],True)
    n=len(t['headers']);table=doc.add_table(rows=1,cols=n)
    table.alignment=WD_TABLE_ALIGNMENT.CENTER;table.autofit=False
    width=6.7
    weights=[1.25]+[1]*(n-1)
    if key=='protocols':weights=[1.6,.65,.9,1.0,.85]
    if key in ('scaling_validation','integration'):weights=[1.25,1.55,1.8]
    if key in ('scaling_n144','scaling_cost'):weights=[2.2,1,1,1]
    widths=[Inches(width*w/sum(weights)) for w in weights]
    for col,w in zip(table.columns,widths):col.width=w
    for cell,h in zip(table.rows[0].cells,t['headers']):cell.text=str(h)
    for row in t['rows']:
        for cell,value in zip(table.add_row().cells,row):cell.text=str(value)
    borders=OxmlElement('w:tblBorders')
    for edge in ('top','left','bottom','right','insideH','insideV'):
        z=OxmlElement('w:'+edge);z.set(qn('w:val'),'single');z.set(qn('w:sz'),'4');z.set(qn('w:color'),'D9D9D9');borders.append(z)
    table._tbl.tblPr.insert_element_before(borders,'w:shd','w:tblLayout','w:tblCellMar','w:tblLook','w:tblCaption','w:tblDescription','w:tblPrChange')
    for i,row in enumerate(table.rows):
        pr=row._tr.get_or_add_trPr();cant=OxmlElement('w:cantSplit');pr.append(cant)
        if i==0:pr.append(OxmlElement('w:tblHeader'))
        for j,cell in enumerate(row.cells):
            cell.width=widths[j]
            cell.vertical_alignment=WD_CELL_VERTICAL_ALIGNMENT.CENTER
            tcpr=cell._tc.get_or_add_tcPr();marg=OxmlElement('w:tcMar')
            for edge,val in [('top',75),('left',80),('bottom',75),('right',80)]:
                a=OxmlElement('w:'+edge);a.set(qn('w:w'),str(val));a.set(qn('w:type'),'dxa');marg.append(a)
            tcpr.insert_element_before(marg,'w:textDirection','w:tcFitText','w:vAlign','w:hideMark','w:headers','w:cellIns','w:cellDel','w:cellMerge','w:tcPrChange')
            sh=OxmlElement('w:shd');sh.set(qn('w:val'),'clear');sh.set(qn('w:fill'),'16324F' if i==0 else 'F3F6F8' if i%2==0 else 'FFFFFF')
            tcpr.insert_element_before(sh,'w:noWrap','w:tcMar','w:textDirection','w:tcFitText','w:vAlign','w:hideMark','w:headers','w:cellIns','w:cellDel','w:cellMerge','w:tcPrChange')
            for p in cell.paragraphs:
                p.alignment=WD_ALIGN_PARAGRAPH.LEFT if j==0 else WD_ALIGN_PARAGRAPH.CENTER
                p.paragraph_format.space_after=Pt(0);p.paragraph_format.line_spacing=1.05
                p.paragraph_format.keep_with_next=True
                for run in p.runs:
                    run.font.size=Pt(9)
                    run.bold=(i==0)
                    if i==0:run.font.color.rgb=RGBColor.from_string('FFFFFF')
    for note in t.get('notes',[]):
        p=newp();inline(p,note)
        p.paragraph_format.keep_with_next=False
        for r in p.runs:r.font.size=Pt(9)

def references_docx():
    p=newp('Heading 1');p.add_run('References');p.paragraph_format.page_break_before=True
    for r in refs:
        p=newp();p.paragraph_format.left_indent=Inches(.22);p.paragraph_format.first_line_indent=Inches(-.22)
        inline(p,'['+str(nums[r['key']])+'] '+bibplain(r))
        f=r['fields']
        if f.get('doi'):p.add_run(' https://doi.org/'+f['doi'])
        elif f.get('url'):p.add_run(' '+f['url'])

p=newp(pattern=title_pattern);r=p.add_run(title)
orig=title_pattern.find(qn('w:r'))
if orig is not None and orig.find(qn('w:rPr')) is not None:r._r.insert(0,deepcopy(orig.find(qn('w:rPr'))))
p.paragraph_format.keep_with_next=True
p=newp(pattern=author_pattern);p.add_run('Naman PM').bold=True;p.paragraph_format.keep_with_next=True
p=newp('Heading 1');p.add_run('Abstract')
p=newp();inline(p,abstract)
p=newp();p.add_run('Keywords. ').bold=True;p.add_run(keywords)

started=False;fnum=0;tnum=0;doc_refs=False
for k,v in bs:
    if k=='head2' and v=='1 Introduction':started=True
    if not started:continue
    if k.startswith('head'):
        first_appendix=v.startswith('Appendix ') and not doc_refs
        if first_appendix:references_docx();doc_refs=True
        p=newp('Heading 1' if k=='head2' else 'Heading 2');p.add_run(v)
        if first_appendix:p.paragraph_format.page_break_before=True
    elif k=='para':
        p=newp();inline(p,v)
        if v.startswith('**Theorem '):p.paragraph_format.keep_together=True
    elif k=='math':
        p=newp();p.alignment=WD_ALIGN_PARAGRAPH.CENTER
        p.paragraph_format.space_before=Pt(6);p.paragraph_format.space_after=Pt(6)
        p._p.append(omml(v,display=True));math_count+=1
    elif k=='code':
        for line in v.splitlines():
            p=newp();p.paragraph_format.space_after=Pt(0);p.paragraph_format.line_spacing=1
            p.paragraph_format.keep_with_next=True
            r=p.add_run(line);r.font.name='Consolas';r.font.size=Pt(9)
        p.paragraph_format.keep_with_next=False
    elif k=='figure':
        fnum+=1;p=newp();p.paragraph_format.keep_with_next=True
        pic=p.add_run().add_picture(str(ROOT/'figures'/(v+'.png')),width=Inches(6.7))
        pic._inline.docPr.set('descr',figs[v].get('description',figs[v]['caption']))
        caption('Figure '+str(fnum)+'. '+figs[v]['caption'])
    elif k=='table':tnum+=1;add_table(v,tnum)

if not doc_refs:references_docx()

# Preserve header/footer paragraph/run design; change only their text slots.
for sec in doc.sections:
    for p in sec.header.paragraphs:
        runs=list(p.runs)
        if runs:
            runs[0].text='Adaptive Wide-Leaf Filtering for Exact Ternary Representation Root Reporting'
            for r in runs[1:]:r.text=''
    for p in sec.footer.paragraphs:
        for r in p.runs:
            if r.text:r.text='Naman PM  •  '
        run=p.add_run()
        begin=OxmlElement('w:fldChar');begin.set(qn('w:fldCharType'),'begin')
        instr=OxmlElement('w:instrText');instr.text=' PAGE '
        sep=OxmlElement('w:fldChar');sep.set(qn('w:fldCharType'),'separate')
        value=OxmlElement('w:t');value.text='1'
        end=OxmlElement('w:fldChar');end.set(qn('w:fldCharType'),'end')
        for x in (begin,instr,sep,value,end):run._r.append(x)
doc.core_properties.title=title;doc.core_properties.author='Naman PM'
doc.core_properties.subject='Research article prepared for ACM Transactions on Algorithms'
doc.core_properties.keywords=keywords
settings=doc.settings.element
u=settings.find(qn('w:updateFields'))
if u is None:
    u=OxmlElement('w:updateFields')
    settings.insert_element_before(u,'w:hdrShapeDefaults','w:footnotePr','w:endnotePr','w:compat','w:docVars','w:rsids','m:mathPr','w:themeFontLang','w:clrSchemeMapping')
u.set(qn('w:val'),'true')
temp=ROOT/'working'/'generated.docx';doc.save(temp)
final=ROOT/'ARWLF_Naman_PM.docx'
allowed={'word/document.xml','word/_rels/document.xml.rels','[Content_Types].xml','word/header1.xml','word/footer1.xml','word/settings.xml','docProps/core.xml'}
with zipfile.ZipFile(REFERENCE) as rz,zipfile.ZipFile(temp) as gz,zipfile.ZipFile(final,'w',zipfile.ZIP_DEFLATED) as oz:
    baseline=set(rz.namelist())
    for name in gz.namelist():oz.writestr(name,gz.read(name) if name in allowed or name not in baseline else rz.read(name))
with zipfile.ZipFile(REFERENCE) as rz,zipfile.ZipFile(final) as fz:
    altered=[n for n in rz.namelist() if rz.read(n)!=fz.read(n)]
    assert set(altered)<=allowed,altered
    # Exact reference section geometry and all preserved styles are retained.
    ns={'w':'http://schemas.openxmlformats.org/wordprocessingml/2006/main'}
    a=E.fromstring(rz.read('word/document.xml')).find('.//w:sectPr',ns)
    b=E.fromstring(fz.read('word/document.xml')).find('.//w:sectPr',ns)
    assert E.tostring(a)==E.tostring(b)
    xml=fz.read('word/document.xml').decode()
    assert '[PAPER TITLE]' not in xml and '[Author One]' not in xml
manifest={'reference_sha256':hashlib.sha256(REFERENCE.read_bytes()).hexdigest(),'changed_reference_parts':altered,'preserve_only_parts_verified':len(baseline)-len(altered),'math_expressions':math_count,'figures':fnum,'tables':tnum,'citations':nums,'word_count':len(text.split())}
manifest.update({'mode':'docx-only' if args.docx_only else 'docx-and-tex','intended_venue':'ACM Transactions on Algorithms','abstract_words':len(abstract.split())})
(ROOT/'qa'/'build_manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
print(json.dumps(manifest,indent=2))
