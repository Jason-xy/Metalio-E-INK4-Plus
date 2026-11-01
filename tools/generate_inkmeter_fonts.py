"""Derive the panel's numeric font from the existing MiSans bitmap asset."""
import argparse
import re
import struct
from pathlib import Path
from PIL import Image, ImageDraw

def generate(source, output, size=112, symbol="inkmeter_digits_font"):
    text = Path(source).read_text(encoding="utf-8")
    raw = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", raw))
    entries = []
    for row in re.findall(r"\{\s*\.bitmap_index\s*=.*?\}", text, re.S):
        entries.append({k: int(v) for k, v in re.findall(r"\.(\w+)\s*=\s*(-?\d+)", row)})
    scale = size / 160
    data, descriptors = [], ["{.bitmap_index=0,.adv_w=0,.box_w=0,.box_h=0,.ofs_x=0,.ofs_y=0}"]
    for ch in "-.0123456789":
        if ch in "-.":
            ratio = size / 112
            image = Image.new("L", (max(1, round(42*ratio)), max(1, round(8*ratio))) if ch == "-" else (max(1, round(11*ratio)), max(1, round(11*ratio))), 255)
            advance, ox, oy = (round(54*16*ratio), round(3*ratio), round(42*ratio)) if ch == "-" else (round(24*16*ratio), round(4*ratio), 0)
        else:
            glyph = entries[1 + int(ch)]
            width, height = glyph["box_w"], glyph["box_h"]
            index = glyph["bitmap_index"]
            pixels = [((bitmap[index + i // 4] >> (6 - 2 * (i % 4))) & 3) * 85 for i in range(width * height)]
            image = Image.new("L", (width, height)); image.putdata(pixels)
            image = image.resize((round(width * scale), round(height * scale)), Image.Resampling.LANCZOS)
            advance, ox, oy = round(glyph["adv_w"] * scale), round(glyph["ofs_x"] * scale), round(glyph["ofs_y"] * scale)
        width, height = image.size
        index = len(data)
        pixels = [min(3, (p + 42) // 85) for p in image.getdata()]
        pixels += [0] * (-len(pixels) % 4)
        data += [sum(pixels[i + j] << (6 - 2 * j) for j in range(4)) for i in range(0, len(pixels), 4)]
        descriptors.append(f"{{.bitmap_index={index},.adv_w={advance},.box_w={width},.box_h={height},.ofs_x={ox},.ofs_y={oy}}}")
    lines = ['// Generated from font_misans_regular_160_2.c; retains its font attribution.', '#include <lvgl.h>', 'static const uint8_t bitmap[] = {']
    lines += [','.join(f'0x{v:02x}' for v in data[i:i+32])+',' for i in range(0,len(data),32)]
    lines += ['};', 'static const lv_font_fmt_txt_glyph_dsc_t glyphs[] = {', ',\n'.join(descriptors), '};',
        'static const uint16_t codes[]={0,1,3,4,5,6,7,8,9,10,11,12};',
        'static const lv_font_fmt_txt_cmap_t cmap[]={ {.range_start=45,.range_length=13,.glyph_id_start=1,.unicode_list=codes,.glyph_id_ofs_list=NULL,.list_length=12,.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY} };',
        'static const lv_font_fmt_txt_dsc_t descriptor={.glyph_bitmap=bitmap,.glyph_dsc=glyphs,.cmaps=cmap,.kern_dsc=NULL,.kern_scale=0,.cmap_num=1,.bpp=2,.kern_classes=0,.bitmap_format=0};',
        f'const lv_font_t {symbol}={{.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height={round(150*size/112)},.base_line={round(32*size/112)},.subpx=LV_FONT_SUBPX_NONE,.underline_position=-1,.underline_thickness=1,.dsc=&descriptor,.fallback=NULL,.user_data=NULL}};']
    Path(output).write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(symbol, 'bitmap:',len(data),'bytes')

def labels(fontpack, output, sources):
    raw=Path(fontpack).read_bytes()
    magic,version,flags,count,index,data,reserved=struct.unpack_from('<4sHHIQQI',raw)
    if magic!=b'EFNT' or version!=1: raise ValueError('Unsupported source fontpack')
    lookup={struct.unpack_from('<Q',raw,index+i*16)[0]:struct.unpack_from('<I',raw,index+i*16+8)[0] for i in range(count)}
    chars=set(chr(i) for i in range(32,127))
    for source in sources: chars.update(c for c in Path(source).read_text(encoding='utf-8') if 127<ord(c)<65536)
    bitmap=[];entries=['{.bitmap_index=0,.adv_w=0,.box_w=0,.box_h=0,.ofs_x=0,.ofs_y=0}'];codes=[]
    for ch in sorted(chars,key=ord):
        size=25;offset=lookup.get((ord(ch)<<32)|(size<<16)|2)
        if offset is None:
            size=30;offset=lookup.get((ord(ch)<<32)|(size<<16)|2)
        if offset is None: continue
        w,h,ox,oy,advance=struct.unpack_from('<HHhhH',raw,offset);scale=18/size;begin=len(bitmap)
        if w and h:
            pixels=[((raw[offset+10+i//4]>>(6-2*(i%4)))&3)*85 for i in range(w*h)]
            image=Image.new('L',(w,h));image.putdata(pixels);image=image.resize((max(1,round(w*scale)),max(1,round(h*scale))),Image.Resampling.LANCZOS)
            w,h=image.size;pixels=[min(3,(p+42)//85) for p in image.getdata()];pixels += [0]*(-len(pixels)%4)
            bitmap += [sum(pixels[i+j]<<(6-2*j) for j in range(4)) for i in range(0,len(pixels),4)]
        entries.append(f'{{.bitmap_index={begin},.adv_w={round(advance*scale*16)},.box_w={w},.box_h={h},.ofs_x={round(ox*scale)},.ofs_y={round(oy*scale)}}}')
        codes.append(ord(ch)-32)
    lines=['// Generated from the existing MiSans fontpack; retains its font attribution.','#include <lvgl.h>','static const uint8_t bitmap[]={']
    lines += [','.join(f'0x{v:02x}' for v in bitmap[i:i+32])+',' for i in range(0,len(bitmap),32)]
    lines += ['};','static const lv_font_fmt_txt_glyph_dsc_t glyphs[]={',',\n'.join(entries),'};','static const uint16_t codes[]={'+','.join(map(str,codes))+'};',f'static const lv_font_fmt_txt_cmap_t cmap[]={{ {{.range_start=32,.range_length={max(codes)+1},.glyph_id_start=1,.unicode_list=codes,.glyph_id_ofs_list=NULL,.list_length={len(codes)},.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}} }};','static const lv_font_fmt_txt_dsc_t descriptor={.glyph_bitmap=bitmap,.glyph_dsc=glyphs,.cmaps=cmap,.kern_dsc=NULL,.kern_scale=0,.cmap_num=1,.bpp=2,.kern_classes=0,.bitmap_format=0};','const lv_font_t inkmeter_labels_font={.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height=28,.base_line=6,.subpx=LV_FONT_SUBPX_NONE,.underline_position=-1,.underline_thickness=1,.dsc=&descriptor,.fallback=NULL,.user_data=NULL};']
    Path(output).write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print('InkMeter label bitmap:',len(codes),'glyphs,',len(bitmap),'bytes')

if __name__ == '__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--source',required=True);parser.add_argument('--output',required=True)
    parser.add_argument('--token-output');parser.add_argument('--fontpack');parser.add_argument('--labels-output');parser.add_argument('--labels-sources',nargs='*',default=[])
    args=parser.parse_args();generate(args.source,args.output)
    if args.token_output:generate(args.source,args.token_output,52,'inkmeter_tokens_font')
    if args.fontpack:labels(args.fontpack,args.labels_output,args.labels_sources)
