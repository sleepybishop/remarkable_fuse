#!/usr/bin/env python3
import sys
import gzip
import struct
import xml.etree.ElementTree as ET

def write_varuint(val):
    res = bytearray()
    while True:
        b = val & 0x7F
        val >>= 7
        if val > 0:
            res.append(b | 0x80)
        else:
            res.append(b)
            break
    return res

def write_crdt_id(p1, p2):
    return struct.pack('<B', p1) + write_varuint(p2)

def write_tag(index, typ):
    val = (index << 4) | typ
    return write_varuint(val)

def generate_rm(strokes, out_path):
    # Header for version 6
    out = bytearray(b"reMarkable .lines file, version=6          ")
    
    crdt_seq = 1
    
    for st in strokes:
        block_body = bytearray()
        
        for i in range(1, 5):
            block_body += write_tag(i, 0xF) # TAG_TYPE_ID
            block_body += write_crdt_id(0, crdt_seq)
            crdt_seq += 1
            
        block_body += write_tag(5, 0x4) # TAG_TYPE_BYTE4
        block_body += struct.pack('<I', 0)
        
        subblock = bytearray()
        subblock.append(0x03) # item_type = line item
        
        tool_id = 1 # Pen
        subblock += write_tag(1, 0x4)
        subblock += struct.pack('<I', tool_id)
        
        color_id = 0 # Black
        subblock += write_tag(2, 0x4)
        subblock += struct.pack('<I', color_id)
        
        subblock += write_tag(3, 0x8)
        subblock += struct.pack('<d', st['width'])
        
        subblock += write_tag(4, 0x4)
        subblock += struct.pack('<f', 0.0)
        
        points_data = bytearray()
        for pt in st['points']:
            points_data += struct.pack('<ffHHBB', pt[0], pt[1], 0, 4, 0, 127)
        
        subblock += write_tag(5, 0xC) # TAG_TYPE_LENGTH4
        subblock += struct.pack('<I', len(points_data))
        subblock += points_data
        
        subblock += write_tag(6, 0xF)
        subblock += write_crdt_id(0, crdt_seq)
        crdt_seq += 1
        
        block_body += write_tag(6, 0xC) # TAG_TYPE_LENGTH4
        block_body += struct.pack('<I', len(subblock))
        block_body += subblock
        
        block_hdr = struct.pack('<IBBBB', len(block_body), 0, 2, 2, 0x05)
        out += block_hdr + block_body
        
    with open(out_path, 'wb') as f:
        f.write(out)

def parse_xoj_xopp(in_path):
    try:
        # Try opening as a gzip-compressed file
        with gzip.open(in_path, 'rt', encoding='utf-8') as f:
            tree = ET.parse(f)
    except (gzip.BadGzipFile, OSError):
        # Fallback to plain XML (uncompressed)
        with open(in_path, 'r', encoding='utf-8') as f:
            tree = ET.parse(f)
            
    root = tree.getroot()
    strokes = []
    
    for page in root.findall('.//page'):
        w = float(page.attrib.get('width', 1404))
        h = float(page.attrib.get('height', 1872))
        
        # Scale to remarkable 1404x1872
        scale_x = 1404.0 / w
        scale_y = 1872.0 / h
        
        for stroke in page.findall('.//stroke'):
            st = {}
            st['width'] = float(stroke.attrib.get('width', 2.0)) * scale_x / 2.0
            st['points'] = []
            
            coords = stroke.text.strip().split()
            for i in range(0, len(coords), 2):
                x = (float(coords[i]) * scale_x) - (1404.0 / 2.0)
                y = float(coords[i+1]) * scale_y
                st['points'].append((x, y))
                
            strokes.append(st)
    return strokes

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: python xoj2rm.py <input.xopp | input.xoj> <output.rm>")
        sys.exit(1)
    strokes = parse_xoj_xopp(sys.argv[1])
    generate_rm(strokes, sys.argv[2])
    print(f"Generated {sys.argv[2]} with {len(strokes)} strokes.")
