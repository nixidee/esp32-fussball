#!/usr/bin/env python3
"""Wire integrity/bounds needed by configuration, names and operational guards."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
UNIT = r'''
#include <array>
#include <cassert>
#include <iostream>
#include "binary_codec.h"
int main(){
const std::array<uint8_t,9> bytes{'1','2','3','4','5','6','7','8','9'};
assert(binary::crc32(bytes)==0xcbf43926);
for(size_t split=0;split<=bytes.size();++split)assert(binary::crc32(std::span(bytes).subspan(split),binary::crc32(std::span(bytes).first(split)))==0xcbf43926);
std::array<uint8_t,17> wire{};binary::Writer out(std::span(wire).subspan(1));
assert(out.u8(0xab)&&out.u16(0xcdef)&&out.u32(0x12345678)&&out.u64(0xfedcba9876543210));assert(out.position()==15&&out.remaining()==1);
const auto before=wire;assert(!out.u16(42)&&!out.good()&&out.position()==15&&wire==before);
binary::Reader in(std::span<const uint8_t>(wire).subspan(1,15));assert(in.u8()==0xab&&in.u16()==0xcdef&&in.u32()==0x12345678&&in.u64()==0xfedcba9876543210&&in.complete());
assert(in.u8()==0&&!in.good()&&in.position()==15);
assert(wire[1]==0xab&&wire[2]==0xef&&wire[3]==0xcd&&wire[4]==0x78&&wire[7]==0x12);
binary::Cursor<const uint8_t> empty({});assert(empty.take(0)==nullptr&&empty.complete());assert(!empty.take(1)&&!empty.good());
std::cout<<"shared wire primitives: IEEE CRC/chained frames, unaligned little endian, exact boundaries and no partial field mutation PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='football-binary-') as temp:
    source=Path(temp)/'wire.cpp'
    source.write_text(UNIT)
    binary=Path(temp)/'wire'
    subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT/'include'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
