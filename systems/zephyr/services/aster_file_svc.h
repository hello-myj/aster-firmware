/* SPDX-License-Identifier: MIT
 * ASTER file-transfer service on the native USB-CDC (Serial-JTAG) port.
 *
 * V1 has no other bulk transport for .ast packages when the SD card is not
 * reachable from the PC (no card reader / WSL). A tiny line protocol on
 * /dev/ttyACMx complements the CH340 shell:
 *
 *   host -> device:  "ASTERPUT <abs-path> <len> <crc32-hex> [resume-off]\r\n"
 *                    (header alone; NO body yet)
 *   device -> host:  "ACK <off>\r\n"                   go-token: device is
 *                      ready; stream <len - off> raw bytes starting at byte
 *                      <off> (off >= the claimed resume-off, i.e. it reflects
 *                      the durable .part). This is the stop-and-wait gate that
 *                      replaces CDC flow control.
 *   host -> device:  one BUF_SZ chunk
 *   device -> host:  "ACK <cum>\r\n"                   after writing it, ask
 *                      for the next chunk (cum = bytes now on disk). The host
 *                      sends the next chunk only after this reply, so a slow
 *                      fs_write can never overflow the RX ring (dropped bytes
 *                      were the old CRC-mismatch root cause).
 *   final:           "OK\r\n"        whole-file crc32 matched, published
 *                    | "RESUME <n>\r\n"  mid-transfer stall/SD error: <n>
 *                      bytes are durable in <path>.part; PUT again with
 *                      resume-off=<n> and only <len-n> bytes follow.
 *                    | "ERR retry\r\n"   crc/length mismatch: .part discarded,
 *                      restart at resume-off=0 (whole file)
 *                    | "ERR <code>\r\n"  hard error (bad path, ENODEV, ...)
 *   host -> device:  "ASTERPING\r\n"
 *   device -> host:  "PONG sd=<0|1> product=<id> target=<id> abi=<n.n> ...\r\n"
 *
 * Because the host is gated on every ACK, the device never has to drain a
 * rejected or aborted body: on the gated path there is no pipelined binary in
 * the line buffer at all. A client that streams a body anyway (a retry after a
 * timeout, a host that died mid-transfer) is not drained either -- the device
 * cannot tell whether those bytes are coming -- but it cannot poison the
 * channel: the line reader accepts only buffers that can still become a
 * command and discards anything else as it arrives (cdc_read_line, CMD_PREFIX),
 * so the next command is answered normally instead of being glued to the
 * debris. Without that, "abcd" + "ASTERPING" arrived as one line, was answered
 * -EINVAL, and the reply the host wanted came up to LINE_TMO_MS (30 s) late.
 * The whole-file crc32 is recomputed
 * from the on-disk .part, so a transfer assembled across several resumed
 * sessions still verifies end-to-end. The file is renamed to <path> only after
 * that check passes, so an aborted/torn transfer never masquerades as an
 * installed package.
 */
#ifndef ASTER_FILE_SVC_H
#define ASTER_FILE_SVC_H

int aster_file_svc_start(void);

#endif
