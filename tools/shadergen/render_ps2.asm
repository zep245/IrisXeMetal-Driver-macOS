mul(8)          g10<1>F        g2<8,8,1>F      g4.0<0,1,0>F  { align1 1Q };
mul(8)          g11<1>F      g3<8,8,1>F      g4.1<0,1,0>F  { align1 1Q };
add(8)          g10<1>F        g10<8,8,1>F    g11<8,8,1>F  { align1 1Q @1 };
add(8)          g120<1>F    g10<8,8,1>F    g4.3<0,1,0>F  { align1 1Q @1 };
mul(8)          g12<1>F        g2<8,8,1>F      g4.4<0,1,0>F  { align1 1Q };
mul(8)          g13<1>F      g3<8,8,1>F      g4.5<0,1,0>F  { align1 1Q };
add(8)          g12<1>F        g12<8,8,1>F    g13<8,8,1>F  { align1 1Q @1 };
add(8)          g121<1>F    g12<8,8,1>F    g4.7<0,1,0>F  { align1 1Q @1 };
mul(8)          g14<1>F        g2<8,8,1>F      g5.0<0,1,0>F  { align1 1Q };
mul(8)          g15<1>F      g3<8,8,1>F      g5.1<0,1,0>F  { align1 1Q };
add(8)          g14<1>F        g14<8,8,1>F    g15<8,8,1>F  { align1 1Q @1 };
add(8)          g122<1>F    g14<8,8,1>F    g5.3<0,1,0>F  { align1 1Q @1 };
mul(8)          g16<1>F        g2<8,8,1>F      g5.4<0,1,0>F  { align1 1Q };
mul(8)          g17<1>F      g3<8,8,1>F      g5.5<0,1,0>F  { align1 1Q };
add(8)          g16<1>F        g16<8,8,1>F    g17<8,8,1>F  { align1 1Q @1 };
add(8)          g123<1>F    g16<8,8,1>F    g5.7<0,1,0>F  { align1 1Q @1 };
sendc(8)        nullUD          g120UD          nullUD          0x08031400              0x00000000
                            render MsgDesc: RT write SIMD8 LastRT Surface = 0 mlen 4 ex_mlen 0 rlen 0 { align1 1Q @1 EOT };
