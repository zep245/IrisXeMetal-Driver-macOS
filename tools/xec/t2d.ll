; v0.51.17 test: every thread writes its positions: out[(gy * GW + gx) * 4 + k] = gx, gy, lx | ly << 8, li
define void @pos2d(ptr addrspace(1) %out, ptr addrspace(1) %dims, <2 x i32> %g, <2 x i32> %l, i32 %li) {
  %gw = load i32, ptr addrspace(1) %dims
  %gx = extractelement <2 x i32> %g, i32 0
  %gy = extractelement <2 x i32> %g, i32 1
  %lx = extractelement <2 x i32> %l, i32 0
  %ly = extractelement <2 x i32> %l, i32 1
  %row = mul i32 %gy, %gw
  %idx = add i32 %row, %gx
  %b = shl i32 %idx, 2
  %p0 = getelementptr i32, ptr addrspace(1) %out, i32 %b
  store i32 %gx, ptr addrspace(1) %p0
  %b1 = add i32 %b, 1
  %p1 = getelementptr i32, ptr addrspace(1) %out, i32 %b1
  store i32 %gy, ptr addrspace(1) %p1
  %lys = shl i32 %ly, 8
  %lp = or i32 %lx, %lys
  %b2 = add i32 %b, 2
  %p2 = getelementptr i32, ptr addrspace(1) %out, i32 %b2
  store i32 %lp, ptr addrspace(1) %p2
  %b3 = add i32 %b, 3
  %p3 = getelementptr i32, ptr addrspace(1) %out, i32 %b3
  store i32 %li, ptr addrspace(1) %p3
  ret void
}
!air.kernel = !{!0}
!0 = !{ptr @pos2d, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6, !7}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1}
!5 = !{i32 2, !"air.thread_position_in_grid"}
!6 = !{i32 3, !"air.thread_position_in_threadgroup"}
!7 = !{i32 4, !"air.thread_index_in_threadgroup"}
