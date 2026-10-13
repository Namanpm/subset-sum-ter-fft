"""Small, explicit TeX-to-MathML parser for this article, transformed by Word's XSL.

Unsupported commands raise, rather than disappearing from the document.
"""
import re
from lxml import etree as E

NS='http://www.w3.org/1998/Math/MathML'
def el(tag,text=None,*children,**attrs):
    x=E.Element('{'+NS+'}'+tag,**attrs)
    if text is not None: x.text=text
    x.extend(children)
    return x

SYMBOLS={'alpha':'α','beta':'β','gamma':'γ','Pi':'Π','Theta':'Θ','ell':'ℓ','kappa':'κ','tau':'τ','cdot':'·','equiv':'≡','geq':'≥','leq':'≤','in':'∈','ldots':'…','lceil':'⌈','rceil':'⌉','lfloor':'⌊','rfloor':'⌋','rho':'ρ','approx':'≈','mid':'∣'}
OPS={'sum':'∑','prod':'∏'}
FUNCS={'min','max','log','exp','Pr'}

SCRIPTED=('msub','msup','msubsup','munder','mover','munderover')
# An n-ary operand ends at a relation, an additive operator or punctuation, as in TeX's reading.
OPERAND_END={'=','≤','≥','≡','<','>','≈','+','−',',','.',';'}

def is_large_op(x):
    if E.QName(x).localname in SCRIPTED:x=x[0]
    return E.QName(x).localname=='mo' and x.get('largeop')=='true'

def bind_large_ops(xs):
    """Group each large operator's operand into one mrow.

    Word's MathML transform takes only the next sibling as an n-ary operand, so without
    this grouping a sum over a product would bind only its first factor."""
    xs=list(xs)
    for i in range(len(xs)-1,-1,-1):
        if not is_large_op(xs[i]):continue
        j=i+1
        while j<len(xs) and not (E.QName(xs[j]).localname=='mo' and xs[j].text in OPERAND_END):j+=1
        if j>i+2:xs[i+1:j]=[el('mrow',None,*xs[i+1:j])]
    return xs

class Parser:
    def __init__(self,s,display=False):
        self.tokens=re.findall(r'\\text\{[^{}]*\}|\\[A-Za-z]+|\\.|[0-9]+|[A-Za-z]|[^\s]',s)
        self.i=0
        # Display equations put large-operator limits under the operator, as TeX does.
        self.display=display
    def pop(self):
        t=self.tokens[self.i];self.i+=1;return t
    def group(self):
        if self.i>=len(self.tokens): raise ValueError('Missing TeX argument')
        if self.tokens[self.i]=='{':
            self.i+=1
            x=self.row('}')
            if self.i>=len(self.tokens) or self.pop()!='}':raise ValueError('Unclosed group')
            return x
        return self.atom()
    def row(self,until=None):
        xs=[]
        while self.i<len(self.tokens) and not (self.tokens[self.i] in until if isinstance(until,tuple) else self.tokens[self.i]==until):
            if self.tokens[self.i] in ('_','^'):
                op=self.pop(); arg=self.group()
                if not xs: raise ValueError('Script without base')
                base=xs.pop()
                name=E.QName(base).localname
                under=self.display and is_large_op(base)
                if op=='^' and name in ('msub','munder'):xs.append(el('munderover' if under else 'msubsup',None,*list(base),arg))
                elif op=='_' and name in ('msup','mover'):xs.append(el('munderover' if under else 'msubsup',None,base[0],arg,base[1]))
                elif under:xs.append(el('munder' if op=='_' else 'mover',None,base,arg))
                else:xs.append(el('msub' if op=='_' else 'msup',None,base,arg))
                continue
            x=self.atom()
            if x is not None:xs.append(x)
        return el('mrow',None,*bind_large_ops(xs))
    def atom(self):
        t=self.pop()
        if t.startswith(r'\text{'):return el('mtext',t[6:-1])
        delimiters={'(':')','[':']','|':'|',r'\lceil':r'\rceil',r'\lfloor':r'\rfloor',r'\{':r'\}'}
        if t in delimiters:
            close=delimiters[t]
            allowed=(')',']') if t in ('(','[') else (close,)
            inside=self.row(allowed)
            if self.i>=len(self.tokens):raise ValueError('Unclosed delimiter '+t)
            close=self.pop()
            if close not in allowed:raise ValueError('Mismatched delimiter '+t)
            chars={r'\lceil':'⌈',r'\rceil':'⌉',r'\lfloor':'⌊',r'\rfloor':'⌋',r'\{':'{',r'\}':'}'}
            return el('mfenced',None,inside,open=chars.get(t,t),close=chars.get(close,close),separators='')
        if t=='{':
            x=self.row('}')
            if self.pop()!='}':raise ValueError('Group')
            return x
        if t.startswith('\\'):
            cmd=t[1:]
            if cmd in ('left','right','Big','big'):return None
            if cmd=='binom':return el('mfenced',None,el('mfrac',None,self.group(),self.group(),linethickness='0'),open='(',close=')',separators='')
            if cmd in ('quad','qquad',',',';','!',' '):return el('mspace',width='0.4em' if cmd in ('quad','qquad') else '0.15em')
            if cmd=='frac':return el('mfrac',None,self.group(),self.group())
            if cmd=='text':
                x=self.group();return el('mtext',''.join(x.itertext()).replace('bytefilterpasses','byte filter passes'))
            if cmd=='operatorname':
                x=self.group();return el('mi',''.join(x.itertext()),mathvariant='normal')
            if cmd=='rm':
                x=self.row('}')
                for a in x.iter():
                    if E.QName(a).localname=='mi':a.set('mathvariant','normal')
                return x
            if cmd=='mathbb':
                x=self.group()
                for a in x.iter():
                    if E.QName(a).localname=='mi':a.set('mathvariant','double-struck')
                return x
            if cmd=='pmod':return el('mrow',None,el('mo','('),el('mi','mod',mathvariant='normal'),el('mspace',width='0.2em'),self.group(),el('mo',')'))
            if cmd=='bmod':return el('mi','mod',mathvariant='normal')
            if cmd in SYMBOLS:return el('mi' if cmd in ('alpha','beta','gamma','Pi','Theta','ell','kappa','tau','rho') else 'mo',SYMBOLS[cmd])
            if cmd in OPS:return el('mo',OPS[cmd],largeop='true',movablelimits='true')
            if cmd in FUNCS:return el('mi',cmd,mathvariant='normal')
            if cmd in ('{','}','_','%'):return el('mo',cmd)
            raise ValueError('Unsupported TeX command '+t)
        if t.isdigit():return el('mn',t)
        if t.isalpha():return el('mi',t)
        return el('mo',{'-':'−','*':'∗'}.get(t,t))

XSL=r'C:\Program Files\Microsoft Office\root\Office16\MML2OMML.XSL'
transform=E.XSLT(E.parse(XSL))
MNS='http://schemas.openxmlformats.org/officeDocument/2006/math'
def mathml(s,display=False):
    """MathML for one TeX expression; a display \\tag{N} becomes a trailing (N)."""
    tag=re.search(r'\\tag\{(\d+)\}',s)
    p=Parser(re.sub(r'\\tag\{\d+\}','',s),display);body=p.row()
    if p.i!=len(p.tokens):raise ValueError('Unconsumed math')
    if tag:
        if not display:raise ValueError('\\tag in inline math')
        body.append(el('mtext','  ('+tag[1]+')'))
    return el('math',None,body,display='block' if display else 'inline')

def m(tag):return '{%s}%s'%(MNS,tag)
def omml(s,display=False):
    """Native Word math: an oMathPara for display equations, an oMath for inline ones."""
    result=transform(mathml(s,display)).getroot()
    for node in list(result.iter(m('t'))):
        if node.text=='mod':
            run=node.getparent();e=run.getparent()
            if E.QName(e).localname=='e' and e.index(run)==0 and E.QName(e.getparent()).localname=='d':
                # \pmod: "x (mod m)", spaced before the parenthesis rather than inside it.
                node.text='mod '
                gap=E.Element(m('r'));E.SubElement(gap,m('t')).text=' '
                d=e.getparent();d.addprevious(gap)
            else:node.text=' mod '
    for node in result.iter(m('t')):
        # Leading spaces (binary mod, the spaced equation number) survive in every renderer.
        if node.text and node.text[0] in '   ':
            node.set('{http://www.w3.org/XML/1998/namespace}space','preserve')
    # LibreOffice's OMML import draws floor and ceiling delimiter objects as slashes, so
    # those brackets are written as plain characters around the unchanged content.
    for d in list(result.iter(m('d'))):
        beg=d.find(m('dPr')+'/'+m('begChr'))
        if beg is None or beg.get(m('val')) not in ('⌈','⌊'):continue
        end=d.find(m('dPr')+'/'+m('endChr'))
        def bracket(ch):
            r=E.Element(m('r'));E.SubElement(r,m('t')).text=ch;return r
        parent=d.getparent();at=parent.index(d)
        parent[at:at+1]=[bracket(beg.get(m('val')))]+list(d.find(m('e')))+[bracket(end.get(m('val')))]
    para=E.QName(result).localname=='oMathPara'
    if display and not para:
        wrapper=E.Element('{%s}oMathPara'%MNS);wrapper.append(result);return wrapper
    if not display and para:return result[0]
    return result
