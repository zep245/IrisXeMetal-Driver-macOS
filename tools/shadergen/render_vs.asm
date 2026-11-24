mov(8)          g20<1>F         0x0F                            { align1 1Q };
mov(8)          g21<1>F         0x0F                            { align1 1Q };
mov(8)          g22<1>F         0x0F                            { align1 1Q };
mov(8)          g23<1>F         0x0F                            { align1 1Q };
mov(8)          g24<1>F         g2<8,8,1>F                      { align1 1Q };
mov(8)          g25<1>F         g3<8,8,1>F                      { align1 1Q };
mov(8)          g26<1>F         g4<8,8,1>F                      { align1 1Q };
mov(8)          g27<1>F         g5<8,8,1>F                      { align1 1Q };
mov(8)          g112<1>F        g6<8,8,1>F                      { align1 1Q };
mov(8)          g113<1>F        g7<8,8,1>F                      { align1 1Q };
mov(8)          g114<1>F        g8<8,8,1>F                      { align1 1Q };
mov(8)          g115<1>F        g9<8,8,1>F                      { align1 1Q };
mov(8)          g116<1>F        0x0F                            { align1 1Q };
mov(8)          g117<1>F        0x0F                            { align1 1Q };
mov(8)          g118<1>F        0x0F                            { align1 1Q };
mov(8)          g119<1>F        0x0F                            { align1 1Q };
mov(8)          g126<1>UD       g1<8,8,1>UD                     { align1 WE_all 1Q };
send(8)         nullUD          g1UD            g20UD           0x02080007                0x00000200
                            urb MsgDesc: offset 0 SIMD8 write  mlen 1 ex_mlen 8 rlen 0 { align1 1Q @1 $0 };
sync nop(8)     null<0,1,0>UD                                   { align1 WE_all 1Q $0.src };
send(8)         nullUD          g126UD          g112UD          0x02080027                0x00000200
                            urb MsgDesc: offset 2 SIMD8 write  mlen 1 ex_mlen 8 rlen 0 { align1 1Q @1 EOT };
