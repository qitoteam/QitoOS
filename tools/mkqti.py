#!/usr/bin/env python3
"""
mkqti.py - build QTI icon files (Qito Icons)

QTI stores real pixel data, not ASCII art – old generator drew from char grids;
QTI stores actual pixel data with hex colour values.

Five sizes: 16,32,64,128,256 default 64 (third). Built-in apps can switch default size;
external apps may ship all five or subset.

Header (32B little-endian): magic "QTI1", version, frame_count <=5, payload_size, checksum, flags, name[12]
Entry 16B: width,height,encoding,palette_size,reserved,offset,size
Encodings: 0 RAW (BGRA), 1 RLE (5-byte runs: count 1-255, B,G,R,A), 2 INDEX (palette + 1 byte/pixel)
Store frames largest-first.

Usage:
  mkqti.py --input icon.png --name myicon --output myicon.qti
  mkqti.py --input icon.rgba --width 64 --height 64 --name icon --output icon.qti
  mkqti.py --builtin --output-dir rootfs/usr/share/icons
"""

from __future__ import annotations
import argparse
import os
import struct
import sys
from typing import List, Tuple, Dict

MAGIC = b"QTI1"
VERSION = 1
HEADER_FMT = "<4sHHIII12s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
ENTRY_FMT = "<HHBBHII"
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)

QTI_RAW, QTI_RLE, QTI_INDEX = 0, 1, 2

assert HEADER_SIZE == 32
assert ENTRY_SIZE == 16

# Default 5 sizes
DEFAULT_SIZES = [256, 128, 64, 32, 16]  # largest-first

def encode_raw(pixels: List[int]) -> bytes:
    out = bytearray()
    for p in pixels:
        out += bytes([p & 0xFF, (p>>8)&0xFF, (p>>16)&0xFF, (p>>24)&0xFF])
    return bytes(out)

def encode_rle(pixels: List[int]) -> bytes:
    out = bytearray()
    i=0
    while i < len(pixels):
        pix = pixels[i]
        run=1
        while i+run < len(pixels) and pixels[i+run]==pix and run<255:
            run+=1
        out += bytes([run, pix &0xFF, (pix>>8)&0xFF, (pix>>16)&0xFF, (pix>>24)&0xFF])
        i+=run
    return bytes(out)

def encode_indexed(pixels: List[int]):
    palette=[]
    lookup={}
    for pix in pixels:
        if pix not in lookup:
            if len(palette)>=256:
                return None
            lookup[pix]=len(palette)
            palette.append(pix)
    out=bytearray()
    for col in palette:
        out += bytes([col &0xFF, (col>>8)&0xFF, (col>>16)&0xFF, (col>>24)&0xFF])
    for pix in pixels:
        out.append(lookup[pix])
    return bytes(out), len(palette)

def best_encoding(pixels: List[int]):
    candidates=[]
    raw=encode_raw(pixels)
    candidates.append((len(raw), QTI_RAW, raw, 0))
    rle=encode_rle(pixels)
    candidates.append((len(rle), QTI_RLE, rle, 0))
    indexed=encode_indexed(pixels)
    if indexed is not None:
        data,psz=indexed
        candidates.append((len(data), QTI_INDEX, data, psz % 256))
    candidates.sort(key=lambda x: x[0])
    return candidates[0][1], candidates[0][2], candidates[0][3]

def scale(pixels: List[int], src: int, dst: int) -> List[int]:
    if src==dst:
        return list(pixels)
    out=[]
    if dst < src:
        factor = src/dst
        for y in range(dst):
            for x in range(dst):
                x0=int(x*factor)
                x1=max(int((x+1)*factor), x0+1)
                y0=int(y*factor)
                y1=max(int((y+1)*factor), y0+1)
                a=r=g=b=count=0
                for sy in range(y0, min(y1,src)):
                    for sx in range(x0, min(x1,src)):
                        pix=pixels[sy*src+sx]
                        pa = (pix>>24)&0xFF
                        a+=pa
                        r+=((pix>>16)&0xFF)*pa
                        g+=((pix>>8)&0xFF)*pa
                        b+=((pix)&0xFF)*pa
                        count+=1
                if count==0 or a==0:
                    out.append(0)
                else:
                    out.append(((a//count)<<24)|((r//a)<<16)|((g//a)<<8)|(b//a))
    else:
        for y in range(dst):
            for x in range(dst):
                out.append(pixels[(y*src//dst)*src + (x*src//dst)])
    return out

def build(frames: Dict[int, List[int]], name: str) -> bytes:
    sizes=sorted(frames.keys(), reverse=True)
    if len(sizes)>5:
        sizes=sizes[:5]
    entries=bytearray()
    payload=bytearray()
    for size in sizes:
        enc,data,psz=best_encoding(frames[size])
        entries+=struct.pack(ENTRY_FMT, size,size,enc,psz,0,len(payload),len(data))
        payload+=data
    checksum=sum(payload)&0xFFFFFFFF
    header=struct.pack(HEADER_FMT, MAGIC, VERSION, len(sizes), len(payload), checksum, 0, name.encode('ascii','ignore')[:12])
    return bytes(header+entries+payload)

def load_png(path: str) -> Tuple[List[int], int, int]:
    try:
        from PIL import Image
        im=Image.open(path).convert('RGBA')
        w,h=im.size
        pixels=[]
        for y in range(h):
            for x in range(w):
                r,g,b,a=im.getpixel((x,y))
                pixels.append((a<<24)|(r<<16)|(g<<8)|b)
        return pixels,w,h
    except ImportError:
        raise SystemExit("Pillow not installed: pip install Pillow or use --raw RGBA input")
    except Exception as e:
        raise SystemExit(f"Failed to load PNG {path}: {e}")

def load_raw_rgba(path: str, width: int, height: int) -> Tuple[List[int], int, int]:
    with open(path,'rb') as f:
        data=f.read()
    expected=width*height*4
    if len(data)<expected:
        raise SystemExit(f"Raw file too small: got {len(data)}, expected {expected}")
    pixels=[]
    for i in range(0,expected,4):
        r=data[i]; g=data[i+1]; b=data[i+2]; a=data[i+3]
        pixels.append((a<<24)|(r<<16)|(g<<8)|b)
    return pixels,width,height

# Builtin icons – convert previous ASCII art palette to real pixels but keep same artwork
# For minimal, we provide a few simple fallback icons generated as solid colors if PNG not available
PALETTE={
    ".": 0x00000000,
    "K": 0xFF1A1D28,
    "W": 0xFFF2F5FA,
    "G": 0xFF8A93A8,
    "D": 0xFF3A4152,
    "B": 0xFF5AA0F0,
    "b": 0xFF2E6FC0,
    "C": 0xFF56D6D6,
    "c": 0xFF2E9EA8,
    "N": 0xFF78D278,
    "n": 0xFF3E9B4F,
    "Y": 0xFFF0C24A,
    "y": 0xFFC08820,
    "R": 0xFFE8635F,
    "P": 0xFFB07BE8,
    "p": 0xFF7A4FB0,
    "O": 0xFFF08A3C,
}

ICONS_ASCII={
"terminal": [
"................................",
"................................",
"..KKKKKKKKKKKKKKKKKKKKKKKKKKKK..",
"..KDDDDDDDDDDDDDDDDDDDDDDDDDDK..",
"..KDGGGGGGGGGGGGGGGGGGGGGGGGDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKNNKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKNNKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKNNKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKNNKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKNNKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKNNKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKNNKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKNNNNNNNNNNKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKWWWWWWKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDKKKKKKKKKKKKKKKKKKKKKKKKDK..",
"..KDDDDDDDDDDDDDDDDDDDDDDDDDDK..",
"..KKKKKKKKKKKKKKKKKKKKKKKKKKKK..",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"files": [
"................................",
"................................",
"................................",
".....YYYYYYY....................",
"....YyyyyyyyY...................",
"...YyYYYYYYYyYYYYYYYYYYYYYYYY...",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYWWWWWWWWWWWWWWWWWWYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYWWWWWWWWWWWWWWWWWWYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYWWWWWWWWWWWWWWWWWWYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYWWWWWWWWWWWWWWWWWWYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYWWWWWWWWWWWWWWWWWWYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyYYYYYYYYYYYYYYYYYYYYYYYyY..",
"...YyyyyyyyyyyyyyyyyyyyyyyyyyY..",
"....YYYYYYYYYYYYYYYYYYYYYYYYY...",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"editor": [
"................................",
"................................",
"....WWWWWWWWWWWWWWWWWWWWWW......",
"....WGGGGGGGGGGGGGGGGGGGGW......",
"....WGWWWWWWWWWWWWWWWWWWGW......",
"....WGWKKKKKKKKKKKKKKWWWGW......",
"....WGWWWWWWWWWWWWWWWWWWGW......",
"....WGWKKKKKKKKKKKKKKKKWGW......",
"....WGWWWWWWWWWWWWWWWWWWGW......",
"....WGWKKKKKKKKKKKWWWWWWGW......",
"....WGWWWWWWWWWWWWWWWWWWGW......",
"....WGWKKKKKKKKKKKKKKKKWGW......",
"....WGWWWWWWWWWWWWWWWWWWGW......",
"....WGWKKKKKKKKKWWWWWWWWGW......",
"....WGWWWWWWWWWWWWWWWWWWGW......",
"....WGWKKKKKKKKKKKKKKKKWGW......",
"....WGWWWWWWWWWWWWWWWWWWGW......",
"....WGWKKKKKKKKKKKKWWWWWGW......",
"....WGWWWWWWWWWWWWWWWWWWGW.OO...",
"....WGWKKKKKKKKKKKKKKKKWGW.OyO..",
"....WGWWWWWWWWWWWWWWWWWWGWOyyO..",
"....WGWKKKKKKKKKWWWWWWWWGOyyO...",
"....WGWWWWWWWWWWWWWWWWWWOyyOW...",
"....WGWKKKKKKKKKKKKKKKKOyyOGW...",
"....WGWWWWWWWWWWWWWWWWOyyOWGW...",
"....WGGGGGGGGGGGGGGGGOyyOGGGW...",
"....WWWWWWWWWWWWWWWWOyyOWWWWW...",
"...................OyyO.........",
"..................OyyO..........",
"..................OOO...........",
"................................",
"................................",
],
"settings": [
"................................",
"................................",
"..............PP................",
".............PppP...............",
"............PppppP..............",
"...PPP......PppppP......PPP.....",
"..PpppP.....PppppP.....PpppP....",
"..PppppP....PppppP....PppppP....",
"...PppppP...PppppP...PppppP.....",
"....PppppPPPPppppPPPPppppP......",
".....PppppppppppppppppppP.......",
"......PpppppppppppppppP.........",
".......PpppppKKKKppppP..........",
".PPPPPPPppppKKKKKKppppPPPPPPP...",
".PppppppppppKKKKKKppppppppppP...",
".PppppppppppKKKKKKppppppppppP...",
".PppppppppppKKKKKKppppppppppP...",
".PPPPPPPppppKKKKKKppppPPPPPPP...",
".......PpppppKKKKppppP..........",
"......PpppppppppppppppP.........",
".....PppppppppppppppppppP.......",
"....PppppPPPPppppPPPPppppP......",
"...PppppP...PppppP...PppppP.....",
"..PppppP....PppppP....PppppP....",
"..PpppP.....PppppP.....PpppP....",
"...PPP......PppppP......PPP.....",
"............PppppP..............",
".............PppP...............",
"..............PP................",
"................................",
"................................",
"................................",
],
"browser": [
"................................",
"................................",
"..........BBBBBBBBBB............",
".......BBBbbbbbbbbbbBBB.........",
".....BBbbbbbbbbbbbbbbbbBB.......",
"....BbbbbbWWbbbbbbWWbbbbbB......",
"...BbbbbWWbbbbbbbbbbWWbbbbB.....",
"..BbbbbWbbbbbbbbbbbbbbWbbbbB....",
"..BbbbWbbbbWWWWWWWWbbbbWbbbB....",
".BbbbWbbbWWbbbbbbbbWWbbbWbbbB...",
".BbbbWbbWbbbbbbbbbbbbWbbWbbbB...",
".BbbWbbbWbbbbbbbbbbbbWbbbWbbB...",
"BbbbWbbWbbbbbbbbbbbbbbWbbWbbbB..",
"BWWWWWWWWWWWWWWWWWWWWWWWWWWWWB..",
"BbbbWbbWbbbbbbbbbbbbbbWbbWbbbB..",
".BbbWbbbWbbbbbbbbbbbbWbbbWbbB...",
".BbbbWbbWbbbbbbbbbbbbWbbWbbbB...",
".BbbbWbbbWWbbbbbbbbWWbbbWbbbB...",
"..BbbbWbbbbWWWWWWWWbbbbWbbbB....",
"..BbbbbWbbbbbbbbbbbbbbWbbbbB....",
"...BbbbbWWbbbbbbbbbbWWbbbbB.....",
"....BbbbbbWWbbbbbbWWbbbbbB......",
".....BBbbbbbbbbbbbbbbbbBB.......",
".......BBBbbbbbbbbbbBBB.........",
"..........BBBBBBBBBB............",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"monitor": [
"................................",
"................................",
"..RRRRRRRRRRRRRRRRRRRRRRRRRRRR..",
"..RKKKKKKKKKKKKKKKKKKKKKKKKKKR..",
"..RKKKKKKKKKKKKKKKKKKKKKKKKKKR..",
"..RKKKKKKKKKKKKKKKKKKKKKKKKKKR..",
"..RKKKKKKKKKKKKKKKKKKKKKKKKKKR..",
"..RKKKKKKKKKKKKKKKKKKKNKKKKKKR..",
"..RKKKKKKKKKKKKKKKKKKNNNKKKKKR..",
"..RKKKKKKKKKNKKKKKKKKNNNKKKKKR..",
"..RKKKKKKKKNNNKKKKKKNNNNNKKKKR..",
"..RKKKNKKKKNNNKKKKKKNNNNNKKKKR..",
"..RKKNNNKKNNNNNKKKKNNNNNNNKKKR..",
"..RKKNNNKKNNNNNKKKKNNNNNNNKNKR..",
"..RKNNNNNNNNNNNKKKNNNNNNNNNNNR..",
"..RKNNNNNNNNNNNNKKNNNNNNNNNNNR..",
"..RNNNNNNNNNNNNNNNNNNNNNNNNNNR..",
"..RKKKKKKKKKKKKKKKKKKKKKKKKKKR..",
"..RRRRRRRRRRRRRRRRRRRRRRRRRRRR..",
"....RRRRRRRRRRRRRRRRRRRRRRRR....",
".......RRRRRRRRRRRRRRRRRR.......",
"..........RRRRRRRRRRRR..........",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"calculator": [
"................................",
"................................",
"....OOOOOOOOOOOOOOOOOOOOOO......",
"....OyyyyyyyyyyyyyyyyyyyyO......",
"....OyKKKKKKKKKKKKKKKKKKyO......",
"....OyKWWWWWWWWWWWWWWWWKyO......",
"....OyKKKKKKKKKKKKKKKKKKyO......",
"....OyyyyyyyyyyyyyyyyyyyyO......",
"....OyWWWWyWWWWyWWWWyWWWWO......",
"....OyWWWWyWWWWyWWWWyWWWWO......",
"....OyWWWWyWWWWyWWWWyWWWWO......",
"....OyyyyyyyyyyyyyyyyyyyyO......",
"....OyWWWWyWWWWyWWWWyWWWWO......",
"....OyWWWWyWWWWyWWWWyWWWWO......",
"....OyWWWWyWWWWyWWWWyWWWWO......",
"....OyyyyyyyyyyyyyyyyyyyyO......",
"....OyWWWWyWWWWyWWWWyBBBBO......",
"....OyWWWWyWWWWyWWWWyBBBBO......",
"....OyWWWWyWWWWyWWWWyBBBBO......",
"....OyyyyyyyyyyyyyyyyBBBBO......",
"....OyWWWWWWWWWyWWWWyBBBBO......",
"....OyWWWWWWWWWyWWWWyBBBBO......",
"....OyWWWWWWWWWyWWWWyBBBBO......",
"....OyyyyyyyyyyyyyyyyyyyyO......",
"....OOOOOOOOOOOOOOOOOOOOOO......",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"package": [
"................................",
"................................",
"..............KK................",
"............KKyyKK..............",
"..........KKyyyyyyKK............",
"........KKyyyyyyyyyyKK..........",
"......KKyyyyyyyyyyyyyyKK........",
"....KKyyyyyyyyyyyyyyyyyyKK......",
"..KKyyyyyyyyyyyyyyyyyyyyyyKK....",
".KYyyyyyyyyyyyyyyyyyyyyyyyyYK...",
".KYYyyyyyyyyyyyyyyyyyyyyyyYYK...",
".KYyYYyyyyyyyyyyyyyyyyyyYYyYK...",
".KYyyyYYyyyyyyyyyyyyyyYYyyyYK...",
".KYyyyyyYYyyyyyyyyyyYYyyyyyYK...",
".KYyyyyyyyYYyyyyyyYYyyyyyyyYK...",
".KYyyyyyyyyyYYyyYYyyyyyyyyyYK...",
".KYyyyyyyyyyyyYYyyyyyyyyyyyYK...",
".KYyyyyyyyyyyyyyyyyyyyyyyyyYK...",
".KYyyyyyyyyyyyyyyyyyyyyyyyyYK...",
".KYyyyyyyyyyyyyyyyyyyyyyyyyYK...",
".KYyyyyyyyyyyyyyyyyyyyyyyyyYK...",
".KYyyyyyyyyyyyyyyyyyyyyyyyyYK...",
".KYyyyyyyyyyyyyyyyyyyyyyyyyYK...",
".KYyyyyyyyyyyyyyyyyyyyyyyyyYK...",
".KYYYYYYYYYYYYYYYYYYYYYYYYYYK...",
"..KKKKKKKKKKKKKKKKKKKKKKKKKK....",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"network": [
"................................",
"................................",
"..........CCCCCCCCCC............",
".......CCCcccccccccCCC..........",
".....CCcccccccccccccccCC........",
"....CccccccccccccccccccCC.......",
"...CccccccccccccccccccccC.......",
"..CccccccccccccccccccccccC......",
"..CccccccccccccccccccccccC......",
".CcccccccccccccccccccccccC......",
".CcccccccccccccccccccccccC......",
".CcccccccccccccccccccccccC......",
"..CccccccccccccccccccccccC......",
"..CccccccccccccccccccccccC......",
"...CccccccccccccccccccccC.......",
"....CccccccccccccccccccC........",
".....CCcccccccccccccccC.........",
".......CCCcccccccccCCC..........",
"..........CCCCCCCCCC............",
"..............CC................",
"..............CC................",
".........CCCCCCCCCCCC...........",
"........CccccccccccccC..........",
"........CcWWcWWcWWcccC..........",
"........CccccccccccccC..........",
"........CCCCCCCCCCCCCC..........",
"................................",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"clock": [
"................................",
"................................",
"..........KKKKKKKKKK............",
".......KKKWWWWWWWWWWKKK.........",
".....KKWWWWWWWWWWWWWWWWKK.......",
"....KWWWWWWWWKKWWWWWWWWWWK......",
"...KWWWWWWWWWKKWWWWWWWWWWWK.....",
"..KWWWWWWWWWWKKWWWWWWWWWWWWK....",
"..KWWWWWWWWWWKKWWWWWWWWWWWWK....",
".KWWWWWWWWWWWKKWWWWWWWWWWWWWK...",
".KWWWWWWWWWWWKKWWWWWWWWWWWWWK...",
".KWWKKWWWWWWWKKWWWWWWWWKKWWWK...",
".KWWKKWWWWWWWKKWWWWWWWWKKWWWK...",
".KWWWWWWWWWWWKKWWWWWWWWWWWWWK...",
".KWWWWWWWWWWWKBBBBBBBWWWWWWWK...",
".KWWWWWWWWWWWWBBBBBBBWWWWWWWK...",
".KWWWWWWWWWWWWWWWWWWWWWWWWWWK...",
".KWWWWWWWWWWWWWWWWWWWWWWWWWWK...",
".KWWWWWWWWWWWWWWWWWWWWWWWWWWK...",
".KWWWWWWWWWWWWWWWWWWWWWWWWWWK...",
"..KWWWWWWWWWWWWWWWWWWWWWWWWK....",
"..KWWWWWWWWWWWWWWWWWWWWWWWWK....",
"...KWWWWWWWWWWWWWWWWWWWWWWK.....",
"....KWWWWWWWWWWWWWWWWWWWWK......",
".....KKWWWWWWWWWWWWWWWWKK.......",
".......KKKWWWWWWWWWWKKK.........",
"..........KKKKKKKKKK............",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"help": [
"................................",
"................................",
"..........NNNNNNNNNN............",
".......NNNnnnnnnnnnnNNN.........",
".....NNnnnnnnnnnnnnnnnnNN.......",
"....NnnnnnnnnnnnnnnnnnnnnN......",
"...NnnnnnnnWWWWWWWnnnnnnnnN.....",
"..NnnnnnnWWWWWWWWWWWnnnnnnnN....",
"..NnnnnnWWWnnnnnnnWWWnnnnnnN....",
".NnnnnnWWWnnnnnnnnnWWWnnnnnnN...",
".NnnnnnWWWnnnnnnnnnWWWnnnnnnN...",
".NnnnnnnnnnnnnnnnnWWWnnnnnnnN...",
".NnnnnnnnnnnnnnnWWWWnnnnnnnnN...",
".NnnnnnnnnnnnnWWWWnnnnnnnnnnN...",
".NnnnnnnnnnnWWWWnnnnnnnnnnnnN...",
".NnnnnnnnnnWWWnnnnnnnnnnnnnnN...",
".NnnnnnnnnnWWWnnnnnnnnnnnnnnN...",
".NnnnnnnnnnnnnnnnnnnnnnnnnnnN...",
".NnnnnnnnnnWWWnnnnnnnnnnnnnnN...",
".NnnnnnnnnWWWWWnnnnnnnnnnnnnN...",
"..NnnnnnnnWWWWWnnnnnnnnnnnnN....",
"..NnnnnnnnnWWWnnnnnnnnnnnnnN....",
"...NnnnnnnnnnnnnnnnnnnnnnnN.....",
"....NnnnnnnnnnnnnnnnnnnnnN......",
".....NNnnnnnnnnnnnnnnnnNN.......",
".......NNNnnnnnnnnnnNNN.........",
"..........NNNNNNNNNN............",
"................................",
"................................",
"................................",
"................................",
"................................",
],
"qito": [
"................................",
"................................",
"..........BBBBBBBBBB............",
".......BBBBbbbbbbbbBBBB.........",
".....BBbbbbbbbbbbbbbbbbBB.......",
"....BbbbbbbbbbbbbbbbbbbbbB......",
"...BbbbbbCCCCCCCCCCCCbbbbbB.....",
"..BbbbbCCCCCCCCCCCCCCCCbbbbB....",
"..BbbbCCCCbbbbbbbbbbCCCCbbbB....",
".BbbbCCCCbbbbbbbbbbbbCCCCbbbB...",
".BbbbCCCbbbbbbbbbbbbbbCCCbbbB...",
".BbbCCCCbbbbbbbbbbbbbbCCCCbbB...",
".BbbCCCbbbbbbbbbbbbbbbbCCCbbB...",
".BbbCCCbbbbbbbbbbbbbbbbCCCbbB...",
".BbbCCCbbbbbbbbbbbbbbbbCCCbbB...",
".BbbCCCbbbbbbbbbbbbbbbbCCCbbB...",
".BbbCCCbbbbbbbbbbbbbbbbCCCbbB...",
".BbbCCCCbbbbbbbbbbbbCCCCCCbbB...",
".BbbbCCCbbbbbbbbbbbCCCCCbbbbB...",
".BbbbCCCCbbbbbbbbCCCCCCbbbbB....",
"..BbbbCCCCbbbbbCCCCCCCCbbbB.....",
"..BbbbbCCCCCCCCCCCbbCCCCbbB.....",
"...BbbbbbCCCCCCCbbbbbCCCCbB.....",
"....BbbbbbbbbbbbbbbbbbCCCCB.....",
".....BBbbbbbbbbbbbbbbbbCCCC.....",
".......BBBbbbbbbbbbbBBBBCCC.....",
"..........BBBBBBBBBB.....CC.....",
"................................",
"................................",
"................................",
"................................",
"................................",
],
}


def art_to_pixels(rows):
    pixels=[]
    for row in rows:
        for ch in row:
            pixels.append(PALETTE.get(ch,0))
    return pixels

def main():
    parser=argparse.ArgumentParser(description="Build QTI icons")
    parser.add_argument("--input", help="PNG or raw RGBA file")
    parser.add_argument("--width", type=int, default=0)
    parser.add_argument("--height", type=int, default=0)
    parser.add_argument("--raw", action="store_true", help="Input is raw RGBA, not PNG")
    parser.add_argument("--name", default="icon")
    parser.add_argument("--output", default=None)
    parser.add_argument("--sizes", default="256,128,64,32,16", help="comma separated sizes")
    parser.add_argument("--builtin", action="store_true")
    parser.add_argument("--output-dir", default=None)
    args=parser.parse_args()

    sizes=[int(v) for v in args.sizes.split(",") if v.strip()]
    sizes=sorted(sizes, reverse=True)
    # Ensure largest-first

    if args.builtin:
        if not args.output_dir:
            raise SystemExit("--builtin needs --output-dir")
        os.makedirs(args.output_dir, exist_ok=True)
        total=0
        for name, rows in ICONS_ASCII.items():
            pix32=art_to_pixels(rows)
            frames={sz: scale(pix32,32,sz) for sz in sizes}
            data=build(frames,name)
            path=os.path.join(args.output_dir,f"{name}.qti")
            with open(path,'wb') as h:
                h.write(data)
            print(f"  {name:12s} {len(data):6d} bytes ({len(sizes)} frames)")
            total+=len(data)
        print(f"{len(ICONS_ASCII)} icons, {total} bytes")
        return 0

    if not args.input:
        parser.print_help()
        return 1

    if args.raw:
        if args.width<=0 or args.height<=0:
            raise SystemExit("--raw needs --width and --height")
        pixels,w,h=load_raw_rgba(args.input,args.width,args.height)
    else:
        pixels,w,h=load_png(args.input)
        # If PNG not square, take min dimension as base? For simplicity require square or scale to largest size
        # We will scale base to largest requested size
        base_size=max(w,h)
        # For non-square, we will center crop? Simplify: if not square, scale to largest and then crop/pad? For now just require square
        if w!=h:
            print(f"Warning: PNG {w}x{h} not square, scaling to {sizes[0]}x{sizes[0]} from {w}x{h}")
            # Scale original to largest size using simple nearest
            # If w!=h, we will scale using scale function that expects square source, so we need to convert to square first
            # For simplicity, if not square, we use w as src and scale to largest, but will still work if we treat as square with w as dimension (may distort)
            # We'll just treat pixels as w*h and scale via separate logic: use PIL resize if available
            try:
                from PIL import Image
                im=Image.open(args.input).convert('RGBA').resize((sizes[0],sizes[0]))
                pixels=[]
                for y in range(sizes[0]):
                    for x in range(sizes[0]):
                        r,g,b,a=im.getpixel((x,y))
                        pixels.append((a<<24)|(r<<16)|(g<<8)|b)
                w=h=sizes[0]
            except:
                pass

    # Now we have base image w x h – we will generate frames for each requested size
    # If base size != largest size, scale base to largest first
    if w!=sizes[0] or h!=sizes[0]:
        # Scale to largest
        # If original not same as largest, scale
        if w==h:
            base_pixels=scale(pixels,w,sizes[0])
        else:
            base_pixels=pixels  # already resized via PIL
    else:
        base_pixels=pixels

    frames={}
    for sz in sizes:
        if sz==sizes[0]:
            frames[sz]=base_pixels
        else:
            frames[sz]=scale(base_pixels,sizes[0],sz)

    data=build(frames,args.name)
    out=args.output or f"{args.name}.qti"
    with open(out,'wb') as f:
        f.write(data)
    print(f"Wrote {out} ({len(data)} bytes) {len(sizes)} frames, default {64}px")
    return 0

if __name__=="__main__":
    sys.exit(main())
