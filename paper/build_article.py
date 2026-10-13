"""Build the article on ACM's Word submission template; --docx-only never opens or writes TeX.

The DOCX uses only the template's own styles, section geometry, footers and numbering.
The 13 October 2026 build on the earlier theory-paper reference is kept unchanged in
archive/2026-10-13-theory-template/; its builder is this file at commit 04bd1e0.
"""
from pathlib import Path
from copy import deepcopy
import re,json,zipfile,hashlib,argparse
from docx import Document
from docx.shared import Emu,Pt,Twips
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT,WD_CELL_VERTICAL_ALIGNMENT
from docx.opc.constants import RELATIONSHIP_TYPE as RT
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from lxml import etree as E
from math_native import omml

ROOT=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--docx-only',action='store_true',help='Preserve TeX and PDF files completely')
parser.add_argument('--output',type=Path,default=ROOT/'ARWLF_Naman_PM.docx',help='DOCX to write (default: the article)')
args=parser.parse_args()
TEMPLATE=ROOT/'template'/'acm_submission_template.docx'
assert hashlib.sha256(TEMPLATE.read_bytes()).hexdigest()=='05e757a3c93f9cbf9dbca257009d48d141f311b1132f8396b8cdb2fbac9ef0e9'
# Front matter the manuscript source does not carry, in the template's affiliation format.
AUTHOR='Naman Menasagi'
AFFILIATION='Independent Researcher, Bengaluru, Karnataka, India, naman.menasagi@gmail.com'
SHORT_TITLE='Adaptive Wide-Leaf Filtering for TER Root Reporting'
# ACM Computing Classification System 2012: (top-level concept, leaf concepts).
CCS=[('Theory of computation',['Data structures design and analysis'])]
JOURNAL='ACM Trans. Algorithms'
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

# Clone the ACM template: keep its styles, numbering, footers and section geometry, and
# reuse the paragraph and run properties of its own front-matter samples.
NS={'w':'http://schemas.openxmlformats.org/wordprocessingml/2006/main'}
R_NS='{http://schemas.openxmlformats.org/officeDocument/2006/relationships}'
doc=Document(TEMPLATE)
body=doc._element.body
sample={}
for p in doc.paragraphs:sample.setdefault(p.style.name,deepcopy(p._p))
for x in list(body):
    if x.tag!=qn('w:sectPr'):body.remove(x)
sect=doc.sections[0]
TEXT_WIDTH=sect.page_width-sect.left_margin-sect.right_margin

def newp(style):
    return doc.add_paragraph(style=style)
def from_sample(style,runs):
    """A `style` paragraph with the template sample's paragraph properties; each run is
    (text, i) and takes the properties of the sample's run i, or none when i is None."""
    p=newp(style);src=sample[style]
    p._p.remove(p._p.pPr);p._p.insert(0,deepcopy(src.find(qn('w:pPr'))))
    srcruns=src.findall(qn('w:r'))
    for text,i in runs:
        r=p.add_run(text)
        rpr=None if i is None else srcruns[i].find(qn('w:rPr'))
        if rpr is not None:r._r.insert(0,deepcopy(rpr))
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
        elif part.startswith('`'):p.add_run(part[1:-1],style='In-text code')
        else:p.add_run(part)
    p.paragraph_format.widow_control=True
def rule(edges):
    borders=OxmlElement('w:tcBorders')
    for edge in edges:
        z=OxmlElement('w:'+edge);z.set(qn('w:val'),'single');z.set(qn('w:sz'),'4');z.set(qn('w:space'),'0');z.set(qn('w:color'),'auto');borders.append(z)
    return borders
def add_table(key,num):
    t=tables[key]
    p=newp('TableCaption');inline(p,'Table '+str(num)+': '+t['caption'])
    n=len(t['headers']);table=doc.add_table(rows=1,cols=n);table.style=doc.styles['Table Grid']
    table.alignment=WD_TABLE_ALIGNMENT.CENTER;table.autofit=False
    weights=[1.25]+[1]*(n-1)
    if key=='protocols':weights=[1.6,.65,.9,1.0,.85]
    if key in ('scaling_validation','integration'):weights=[1.25,1.55,1.8]
    if key in ('scaling_n144','scaling_cost'):weights=[2.2,1,1,1]
    widths=[Emu(int(TEXT_WIDTH*w/sum(weights))) for w in weights]
    for col,w in zip(table.columns,widths):col.width=w
    for cell,h in zip(table.rows[0].cells,t['headers']):cell.text=str(h)
    for row in t['rows']:
        for cell,value in zip(table.add_row().cells,row):cell.text=str(value)
    # The template's table: no grid, zero left cell margin, rules above and below the
    # header row and below the last row.
    tblpr=table._tbl.tblPr
    tblw=tblpr.find(qn('w:tblW'));tblw.set(qn('w:w'),str(sum(w.twips for w in widths)));tblw.set(qn('w:type'),'dxa')
    borders=OxmlElement('w:tblBorders')
    for edge in ('top','left','bottom','right','insideH','insideV'):
        z=OxmlElement('w:'+edge);z.set(qn('w:val'),'none');z.set(qn('w:sz'),'0');z.set(qn('w:space'),'0');z.set(qn('w:color'),'auto');borders.append(z)
    tblpr.insert_element_before(borders,'w:shd','w:tblLayout','w:tblCellMar','w:tblLook','w:tblCaption','w:tblDescription','w:tblPrChange')
    margins=OxmlElement('w:tblCellMar');left=OxmlElement('w:left');left.set(qn('w:w'),'0');left.set(qn('w:type'),'dxa');margins.append(left)
    tblpr.insert_element_before(margins,'w:tblLook','w:tblCaption','w:tblDescription','w:tblPrChange')
    last=len(table.rows)-1
    for i,row in enumerate(table.rows):
        pr=row._tr.get_or_add_trPr();pr.append(OxmlElement('w:cantSplit'))
        if i==0:pr.append(OxmlElement('w:tblHeader'))
        edges=('top','bottom') if i==0 else ('bottom',) if i==last else ()
        for j,cell in enumerate(row.cells):
            cell.width=widths[j]
            cell.vertical_alignment=WD_CELL_VERTICAL_ALIGNMENT.CENTER
            if edges:cell._tc.get_or_add_tcPr().insert_element_before(rule(edges),'w:shd','w:noWrap','w:tcMar','w:textDirection','w:tcFitText','w:vAlign','w:hideMark','w:headers','w:cellIns','w:cellDel','w:cellMerge','w:tcPrChange')
            for p in cell.paragraphs:
                p.style=doc.styles['TableCell']
                p.paragraph_format.first_line_indent=Twips(0)
                p.alignment=WD_ALIGN_PARAGRAPH.LEFT if j==0 else WD_ALIGN_PARAGRAPH.CENTER
                p.paragraph_format.keep_with_next=True
    # TableFootnote sets no typeface or size of its own; match the template's table text.
    for note in t.get('notes',[]):
        p=newp('TableFootnote');inline(p,note)
        p.alignment=WD_ALIGN_PARAGRAPH.JUSTIFY;p.paragraph_format.space_before=Pt(3)
        for r in p.runs:r.font.name='Linux Libertine O';r.font.size=Pt(8)

def references_docx():
    # Bib_entry numbers its paragraphs [1], [2], ... in order, matching the citation numbers.
    newp('ReferenceHead').add_run('REFERENCES')
    for r in refs:
        p=newp('Bib_entry');inline(p,bibplain(r))
        f=r['fields']
        if f.get('doi'):p.add_run(' https://doi.org/'+f['doi'])
        elif f.get('url'):p.add_run(' '+f['url'])

from_sample('Title_document',[(title,0)])
from_sample('Short Title',[(SHORT_TITLE,0)])
from_sample('Authors',[(AUTHOR,0)])
from_sample('Affiliation',[(AFFILIATION,0)])
p=newp('Abstract');inline(p,abstract)
ccs=[('CCS CONCEPTS ',0)]
for g,(top,leaves) in enumerate(CCS):
    ccs+=[('• ',1),(top,1),(' → '+'; '.join(leaves)+('.' if g==len(CCS)-1 else '; '),2)]
from_sample('CCSDescription',ccs)
from_sample('KeyWords',[('Additional Keywords and Phrases: ',0),(keywords.replace('; ',', '),None)])
from_sample('ACMRefHead',[('ACM Reference Format:',0)])
from_sample('ACMRef',[(f'{AUTHOR}. 2026. {title}. Manuscript submitted to {JOURNAL}.',0)])

# Head1 and Head2 number themselves; the manuscript's numbers must equal that sequence,
# because the text cites sections by number.
STATEMENT=re.compile(r'\*\*(Lemma|Theorem|Corollary|Proposition|Definition) ')
started=False;fnum=0;tnum=0;doc_refs=False;appendix=False;h1=h2=0;prev='head'
for k,v in bs:
    if k=='head2' and v=='1 Introduction':started=True
    if not started:continue
    if k=='head2':
        if v.startswith('Appendix '):
            if not doc_refs:references_docx();doc_refs=True
            m=re.fullmatch(r'Appendix ([A-Z]) (.+)',v);assert m,v;appendix=True
            p=newp('AppendixH1');p.add_run(m[1]);p.add_run(' ');p.add_run(m[2]);prev='head'
        elif m:=re.fullmatch(r'(\d+) (.+)',v):
            h1+=1;h2=0;assert not appendix and int(m[1])==h1,v
            newp('Head1').add_run(m[2]);prev='head'
        else:
            assert not appendix,v
            newp('AckHead').add_run(v.upper());prev='ackhead'
    elif k=='head3':
        m=re.fullmatch(r'(\d+)\.(\d+) (.+)',v);h2+=1
        assert m and not appendix and (int(m[1]),int(m[2]))==(h1,h2),v
        newp('Head2').add_run(m[3]);prev='head'
    elif k=='para':
        style='Statements' if STATEMENT.match(v) else {'head':'PostHeadPara','ackhead':'AckPara','math':'ParaContinue'}.get(prev,'Para')
        p=newp(style);inline(p,v)
        if style=='Statements':p.paragraph_format.keep_together=True
        if v.startswith('**Proof.**'):p.runs[0].bold=False;p.runs[0].italic=True
        prev='para'
    elif k=='math':
        p=newp('DisplayFormula' if r'\tag{' in v else 'DisplayFormulaUnnum')
        p._p.append(omml(v,display=True));math_count+=1;prev='math'
    elif k=='code':
        lines=v.splitlines();m=re.fullmatch(r'Algorithm (\d+)\s+(.+)',lines[0]);assert m,lines[0]
        newp('AlgorithmCaption').add_run(f'ALGORITHM {m[1]}: {m[2]}')
        # The template's InconsolataN is rarely installed; its In-text code style (Courier New)
        # at the template's ComputerCode size of 8 pt keeps the longest line on one line.
        for line in lines[1:]:
            p=newp('Algorithm');p.add_run(line,style='In-text code').font.size=Pt(8)
            p.paragraph_format.keep_with_next=True
        p.paragraph_format.keep_with_next=False;prev='para'
    elif k=='figure':
        fnum+=1;p=newp('Image')
        pic=p.add_run().add_picture(str(ROOT/'figures'/(v+'.png')),width=TEXT_WIDTH)
        pic._inline.docPr.set('descr',figs[v].get('description',figs[v]['caption']))
        p=newp('FigureCaption');inline(p,'Figure '+str(fnum)+': '+figs[v]['caption']);prev='para'
    elif k=='table':tnum+=1;add_table(v,tnum);prev='para'
    else:raise ValueError(k)
if not doc_refs:references_docx()

# Drop the template's sample images and links, which nothing in the new body references.
used={v for el in body.iter() for a,v in el.attrib.items() if a.startswith(R_NS)}
for rId,rel in list(doc.part.rels.items()):
    if rel.reltype in (RT.IMAGE,RT.HYPERLINK) and rId not in used:del doc.part.rels[rId]
cp=doc.core_properties
cp.title=title;cp.author=AUTHOR;cp.last_modified_by=AUTHOR;cp.language='en-US'
cp.subject='Research article prepared for ACM Transactions on Algorithms';cp.keywords=keywords
temp=ROOT/'working'/'generated.docx';doc.save(temp)
final=args.output
allowed={'word/document.xml','word/_rels/document.xml.rels','[Content_Types].xml','docProps/core.xml','word/footnotes.xml'}
def without_sample_author_note(xml):
    tree=E.fromstring(xml)
    for note in tree.xpath('w:footnote[not(@w:type)]',namespaces=NS):tree.remove(note)
    return E.tostring(tree,xml_declaration=True,encoding='UTF-8',standalone=True)
with zipfile.ZipFile(TEMPLATE) as tz,zipfile.ZipFile(temp) as gz,zipfile.ZipFile(final,'w',zipfile.ZIP_DEFLATED) as oz:
    baseline=set(tz.namelist())
    for name in gz.namelist():
        if name=='word/footnotes.xml':data=without_sample_author_note(tz.read(name))
        elif name in allowed or name not in baseline:data=gz.read(name)
        else:data=tz.read(name)
        oz.writestr(name,data)
with zipfile.ZipFile(TEMPLATE) as tz,zipfile.ZipFile(final) as fz:
    names=set(fz.namelist())
    removed=sorted(baseline-names)
    assert all(n.startswith('word/media/') for n in removed),removed
    altered=[n for n in tz.namelist() if n in names and tz.read(n)!=fz.read(n)]
    assert set(altered)<=allowed,altered
    # Exact template section geometry and footers; no template sample content survives.
    a=E.fromstring(tz.read('word/document.xml')).find('.//w:sectPr',NS)
    b=E.fromstring(fz.read('word/document.xml')).find('.//w:sectPr',NS)
    assert E.tostring(a)==E.tostring(b)
    xml=fz.read('word/document.xml').decode()+fz.read('word/footnotes.xml').decode()
    for sample_text in ('Submission Template for ACM Papers','Insert your first CCS term','xxxx@gmail.com','Woodstock','Lorem ipsum','Franklin','Place the footnote text'):
        assert sample_text not in xml,sample_text
manifest={'template_sha256':hashlib.sha256(TEMPLATE.read_bytes()).hexdigest(),'changed_template_parts':altered,'removed_template_parts':removed,'preserved_template_parts':len(baseline)-len(altered)-len(removed),'math_expressions':math_count,'figures':fnum,'tables':tnum,'citations':nums,'word_count':len(text.split())}
manifest.update({'mode':'docx-only' if args.docx_only else 'docx-and-tex','intended_venue':'ACM Transactions on Algorithms','abstract_words':len(abstract.split()),'affiliation_supplied':not AFFILIATION.startswith('[')})
(ROOT/'qa'/'build_manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
print(json.dumps(manifest,indent=2))
