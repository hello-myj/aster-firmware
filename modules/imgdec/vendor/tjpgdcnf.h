/* SPDX-License-Identifier: MIT
 * TJpgDec configuration for ASTER imgdec. Based on the LVGL vendored copy
 * (TJpgDec R0.03, (C)ChaN 2021), tuned for our use:
 *   JD_FORMAT=1 -> output native small-endian RGB565 (matches the ASTER
 *                  canvas / aster_plat_display_write_at pixel format).
 *   JD_USE_SCALE=1 -> enable the 1/2,1/4,1/8 hardware-free DCT scaling we
 *                  use to keep the decoded buffer small before the final
 *                  bilinear fit.
 *   JD_FASTDECODE=1 -> table-driven, modest RAM, good on Xtensa (2 needs
 *                  ~extra LUT DRAM for little gain here).
 */
#ifndef TJPGDCNF_H
#define TJPGDCNF_H

#define JD_SZBUF        512     /* stream input buffer (bytes) */
#define JD_FORMAT       1       /* 0:RGB888 1:RGB565(LE) 2:grayscale */
#define JD_USE_SCALE    1       /* allow partial (1/2,1/4,1/8) decode */
#define JD_TBLCLIP      1       /* use table for saturation conversion */
#define JD_FASTDECODE   1       /* 0:basic 1:table 2:table+cache */

#endif
