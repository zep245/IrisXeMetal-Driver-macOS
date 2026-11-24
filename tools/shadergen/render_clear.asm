mov(8)          g120<1>UD       g2<0,1,0>UD                     { align1 1Q };
mov(8)          g121<1>UD       g2.1<0,1,0>UD                   { align1 1Q };
mov(8)          g122<1>UD       g2.2<0,1,0>UD                   { align1 1Q };
mov(8)          g123<1>UD       g2.3<0,1,0>UD                   { align1 1Q @1 };
sendc(8)        nullUD          g120UD          nullUD          0x08031400              0x00000000
                            render MsgDesc: RT write SIMD8 LastRT Surface = 0 mlen 4 ex_mlen 0 rlen 0 { align1 1Q @1 EOT };
