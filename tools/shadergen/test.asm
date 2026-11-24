mov(8)          g127<1>UW  0x76543210V          { align1 WE_all 1Q };
mov(8)          g1<1>UD    g127<8,8,1>UW        { align1 WE_all 1Q @1 };
add(8)          g2<1>UD    g1<8,8,1>UD   0xc0de0000UD { align1 1Q @1 };
mul(8)          g127<1>UD  g1<8;8,1>UD   0x4UW  { align1 @1 1Q };
add(8)          g127<1>UD  g127<8;8,1>UD 0x20001000UD { align1 @1 1Q };
send(8)         nullUD     g127UD        g2UD   0x2026efd   0x00000040
    hdc1 MsgDesc: (DC untyped surface write, Surface = 253, SIMD8, Mask = 0xe) mlen 1 ex_mlen 1 rlen 0 { align1 1Q @1 $1 };
sync nop(8)  null<0,1,0>UD  { align1 WE_all 1H @1 $1.dst };
mov(8)          g127<1>UD  g0<8;8,1>UD  { align1 WE_all 1Q };
send(8)         nullUD     g127UD       nullUD  0x02000000  0x00000000
    ts/btd MsgDesc:  mlen 1 ex_mlen 0 rlen 0 { align1 WE_all 1Q @1 EOT };
