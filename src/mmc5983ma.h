/* MMC5983MA register map (subset). */
#ifndef MMC5983MA_H
#define MMC5983MA_H

#define MMC_REG_XOUT0 0x00u
#define MMC_REG_STATUS 0x08u
#define MMC_REG_CTRL0 0x09u
#define MMC_REG_CTRL1 0x0Au
#define MMC_REG_CTRL2 0x0Bu
#define MMC_REG_CTRL3 0x0Cu
#define MMC_REG_PRODUCT_ID 0x2Fu

#define MMC_PRODUCT_ID 0x30u

/* Internal Control 0 */
#define MMC_CTRL0_TM_M (1u << 0)
#define MMC_CTRL0_TM_T (1u << 1)
#define MMC_CTRL0_INT_DONE_EN (1u << 2)
#define MMC_CTRL0_SET (1u << 3)
#define MMC_CTRL0_RESET (1u << 4)
#define MMC_CTRL0_AUTO_SR_EN (1u << 5)

/* Internal Control 1: BW[1:0]. 0b11 = 800 Hz, 0.5 ms measurement time. */
#define MMC_CTRL1_BW_800HZ 0x03u
#define MMC_CTRL1_SW_RST (1u << 7) /* software reset; 10 ms power-on time */

/* Status */
#define MMC_STATUS_MEAS_M_DONE (1u << 0)

/* Field bytes per sample: Xout0, Xout1, Yout0, Yout1, Zout0, Zout1, XYZout2 */
#define MMC_SAMPLE_BYTES 7u
/* Bytes read per transaction (M-7, Rev 2.8): 0x00..0x08, i.e. the field
 * bytes, Tout (0x07, not updated: TM_T is never used) and Status (0x08). */
#define MMC_READ_BYTES 9u

#endif /* MMC5983MA_H */
