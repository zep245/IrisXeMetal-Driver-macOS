mov(8)          g127<1>UW  0x76543210V          { align1 WE_all 1Q };
mov(8)          g3<1>UD    g127<8,8,1>UW        { align1 WE_all 1Q @1 };
mul(1)          g4<1>UD    g0.1<0,1,0>UD  g1.14<0,1,0>UW { align1 WE_all 1N @1 };
add(1)          g4<1>UD    g4<0,1,0>UD    g2<0,1,0>UD    { align1 WE_all 1N @1 };
add(8)          g3<1>UD    g3<8,8,1>UD    g4<0,1,0>UD    { align1 1Q @1 };
cmp.l.f0.0(8)   null<1>UD  g3<8,8,1>UD    g1.6<0,1,0>UD  { align1 1Q @1 };
shl(8)          g5<1>UD    g3<8,8,1>UD    0x2UD          { align1 1Q @1 };
add(8)          g6<1>UD    g5<8,8,1>UD    g1<0,1,0>UD    { align1 1Q @1 };
add(8)          g7<1>UD    g5<8,8,1>UD    g1.1<0,1,0>UD  { align1 1Q @2 };
(+f0.0) send(8) g8UD       g6UD           nullUD  0x02106efd  0x00000000
    hdc1 MsgDesc: (DC untyped surface read, Surface = 253, SIMD8, Mask = 0xe) mlen 1 ex_mlen 0 rlen 1 { align1 1Q @2 $0 };
add(8)          g9<1>UD    g8<8,8,1>UD    0x1UD          { align1 1Q $0.dst };
(+f0.0) send(8) nullUD     g7UD           g9UD    0x02026efd  0x00000040
    hdc1 MsgDesc: (DC untyped surface write, Surface = 253, SIMD8, Mask = 0xe) mlen 1 ex_mlen 1 rlen 0 { align1 1Q @1 $1 };
sync nop(8)     null<0,1,0>UD  { align1 WE_all 1H @1 $1.dst };
mov(8)          g127<1>UD  g0<8;8,1>UD  { align1 WE_all 1Q };
send(8)         nullUD     g127UD       nullUD  0x02000000  0x00000000
    ts/btd MsgDesc:  mlen 1 ex_mlen 0 rlen 0 { align1 WE_all 1Q @1 EOT };
