from pathlib import Path
import re
from html import escape
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, Preformatted, KeepTogether
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from pypdf import PdfReader

root = Path(__file__).resolve().parents[2]
source = root / 'docs/SAMPLE-TIMING-DESIGN.md'
output = root / 'output/pdf/SAMPLE-TIMING-DESIGN.pdf'
output.parent.mkdir(parents=True, exist_ok=True)
for name, filename in [('Arial','arial.ttf'), ('Arial-Bold','arialbd.ttf'), ('Arial-Italic','ariali.ttf'), ('Consolas','consola.ttf')]:
    pdfmetrics.registerFont(TTFont(name, str(Path('C:/Windows/Fonts') / filename)))
pdfmetrics.registerFontFamily('Arial',normal='Arial',bold='Arial-Bold',italic='Arial-Italic',boldItalic='Arial-Bold')
styles = getSampleStyleSheet()
styles.add(ParagraphStyle(name='Text',fontName='Arial',fontSize=10,leading=14,spaceAfter=7))
styles.add(ParagraphStyle(name='TitleCustom',fontName='Arial-Bold',fontSize=22,leading=27,spaceAfter=16,textColor=colors.HexColor('#19364a')))
styles.add(ParagraphStyle(name='H2Custom',fontName='Arial-Bold',fontSize=15,leading=19,spaceBefore=15,spaceAfter=9,keepWithNext=True,textColor=colors.HexColor('#19364a')))
styles.add(ParagraphStyle(name='H3Custom',fontName='Arial-Bold',fontSize=11,leading=15,spaceBefore=10,spaceAfter=6,keepWithNext=True))
styles.add(ParagraphStyle(name='CellCustom',fontName='Arial',fontSize=8.5,leading=11,spaceAfter=0))
styles.add(ParagraphStyle(name='CodeCustom',fontName='Consolas',fontSize=7.3,leading=10,spaceAfter=9,spaceBefore=4,backColor=colors.HexColor('#f0f3f5'),borderPadding=7))
styles.add(ParagraphStyle(name='ListCustom',parent=styles['Text'],leftIndent=15,firstLineIndent=-11,spaceAfter=5))

def inline(text):
    text = text.replace('—',' - ').replace('–','-').replace('−','-').replace('\u2011','-')
    text = re.sub(r'\[([^\]]+)\]\(([^)]+)\)',r'\1',text)
    text = escape(text)
    text = re.sub(r'`([^`]+)`',r'<font name="Consolas">\1</font>',text)
    return re.sub(r'\*\*([^*]+)\*\*',r'<b>\1</b>',text)

lines=source.read_text(encoding='utf-8').splitlines()
story=[]
i=0
while i<len(lines):
    line=lines[i]
    if not line.strip():
        i+=1
    elif line.startswith('```'):
        i+=1
        code=[]
        while i<len(lines) and not lines[i].startswith('```'):
            code.append(lines[i].replace('−','-'))
            i+=1
        longest=max([pdfmetrics.stringWidth(x,'Consolas',7.3) for x in code] or [0])
        style=ParagraphStyle(name='fitCode',parent=styles['CodeCustom'],fontSize=min(7.3,7.3*490/max(longest,1)))
        story.append(KeepTogether([Preformatted('\n'.join(code),style)]))
        i+=1
    elif line.startswith('|'):
        rows=[]
        while i<len(lines) and lines[i].startswith('|'):
            cells=[x.strip() for x in lines[i].strip().strip('|').split('|')]
            if not all(re.fullmatch(r':?-+:?',x) for x in cells):
                rows.append(cells)
            i+=1
        n=len(rows[0])
        if n==4:
            widths=[42,42,106,314]
        elif n==3:
            widths=[105,195,204]
        else:
            widths=[153,351]
        content=[[Paragraph(('<b>'+inline(c)+'</b>') if j==0 else inline(c),styles['CellCustom']) for c in row] for j,row in enumerate(rows)]
        table=Table(content,colWidths=widths,repeatRows=1,hAlign='LEFT')
        table.setStyle(TableStyle([('BACKGROUND',(0,0),(-1,0),colors.HexColor('#e3ebef')),('VALIGN',(0,0),(-1,-1),'TOP'),('LINEBELOW',(0,0),(-1,0),0.6,colors.HexColor('#96aab7')),('LINEBELOW',(0,1),(-1,-1),0.3,colors.HexColor('#d3dce2')),('LEFTPADDING',(0,0),(-1,-1),6),('RIGHTPADDING',(0,0),(-1,-1),6),('TOPPADDING',(0,0),(-1,-1),6),('BOTTOMPADDING',(0,0),(-1,-1),6)]))
        story.extend([table,Spacer(1,9)])
    elif line.startswith('#'):
        level=len(line)-len(line.lstrip('#'))
        style='TitleCustom' if level==1 else 'H2Custom' if level==2 else 'H3Custom'
        story.append(Paragraph(inline(line[level:].strip()),styles[style]))
        i+=1
    else:
        numbered=re.match(r'^\d+\. ',line)
        bullet=line.startswith('- ')
        text=[line]
        i+=1
        while i<len(lines) and lines[i].strip() and not re.match(r'^(#|\||```|- |\d+\. )',lines[i]):
            text.append(lines[i]); i+=1
        paragraph=' '.join(text)
        if bullet:
            paragraph='• '+paragraph[2:]
        story.append(Paragraph(inline(paragraph),styles['ListCustom' if bullet or numbered else 'Text']))

def footer(canvas,doc):
    canvas.saveState()
    canvas.setStrokeColor(colors.HexColor('#c5d0d7'))
    canvas.line(54,42,558,42)
    canvas.setFont('Arial',8)
    canvas.setFillColor(colors.HexColor('#536775'))
    canvas.drawString(54,29,'ESP32 Digitizer | Sample timing design | November 7, 2026')
    canvas.drawRightString(558,29,str(doc.page))
    canvas.restoreState()

doc=SimpleDocTemplate(str(output),pagesize=(612,792),rightMargin=54,leftMargin=54,topMargin=45,bottomMargin=56,title='Sample timing design: FSYNC, PPS and recorded time',author='ESP32 Digitizer project')
doc.build(story,onFirstPage=footer,onLaterPages=footer)
reader=PdfReader(output)
text='\n'.join(p.extract_text() for p in reader.pages)
assert 'Absolute UTC accuracy remains unquantified' in text.replace('\n',' ')
assert '118.931375' in text
print(str(output))
print('Pages:',len(reader.pages))
