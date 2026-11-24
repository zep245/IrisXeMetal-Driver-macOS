// Tiger Lake (Gen12) display register offsets used by IrisXeFB.
// Source: Intel public PRMs (Tiger Lake, Vol 2c) and the open-source Linux i915 driver.
#ifndef IRISXE_TGLREGS_H
#define IRISXE_TGLREGS_H

// Pipe A source size: bits 31:16 = width-1, bits 15:0 = height-1
#define TGL_PIPESRC_A        0x6001C

// Universal plane 1 on pipe A (the plane UEFI GOP scans out from)
#define TGL_PLANE_CTL_1_A    0x70180   // bit31 enable, bits 12:10 tiling (0 = linear)
#define TGL_PLANE_STRIDE_1_A 0x70188   // linear: stride in 64-byte units
#define TGL_PLANE_SIZE_1_A   0x70190   // bits 27:16 height-1, bits 12:0 width-1
#define TGL_PLANE_SURF_1_A   0x7019C   // GGTT address of surface, bits 31:12

#define TGL_PLANE_CTL_ENABLE      (1u << 31)
#define TGL_PLANE_CTL_TILING_MASK (7u << 10)

// South display backlight PWM (CNP/TGP PCH style), controller 0 - used in a later stage
#define TGL_BLC_PWM_CTL1     0xC8250
#define TGL_BLC_PWM_FREQ1    0xC8254
#define TGL_BLC_PWM_DUTY1    0xC8258

// DP AUX channel A (eDP panel). Source: Linux i915 intel_dp_aux_regs.h
#define TGL_DP_AUX_CH_CTL_A        0x64010
#define TGL_DP_AUX_CH_DATA_A(i)    (0x64014 + (i) * 4)   // 5 registers, 20 bytes
#define TGL_AUX_CTL_SEND_BUSY      (1u << 31)
#define TGL_AUX_CTL_DONE           (1u << 30)
#define TGL_AUX_CTL_TIME_OUT_ERROR (1u << 28)
#define TGL_AUX_CTL_TIME_OUT_MAX   (3u << 26)            // 1600us
#define TGL_AUX_CTL_RECEIVE_ERROR  (1u << 25)
#define TGL_AUX_CTL_MSG_SIZE_SHIFT 20
#define TGL_AUX_CTL_MSG_SIZE_MASK  (0x1fu << 20)
#define TGL_AUX_CTL_FW_SYNC(c)     ((((c) - 1) & 0x1f) << 5)
#define TGL_AUX_CTL_SYNC(c)        (((c) - 1) & 0x1f)

// PCI config offsets
#define TGL_PCI_BAR0_GTTMMADR 0x10     // MMIO + GTT
#define TGL_PCI_BAR2_GMADR    0x18     // graphics aperture (CPU view of GGTT)

#endif
