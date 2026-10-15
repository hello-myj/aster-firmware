/* SPDX-License-Identifier: MIT */
#ifndef ASTER_ERROR_H
#define ASTER_ERROR_H

/* ASTER error codes (design doc sec.56). Negative domain is stable ABI;
 * platform errnos must be mapped by the Platform Layer, never leaked. */
typedef enum {
	ASTER_OK = 0,
	ASTER_EINVAL = -1,     /* bad argument / failed bounds check */
	ASTER_ENOMEM = -2,     /* allocation / memory limit */
	ASTER_ENOTSUP = -3,    /* capability absent on this device */
	ASTER_EPERM = -4,      /* permission denied */
	ASTER_EBUSY = -5,
	ASTER_ENOENT = -6,
	ASTER_EIO = -7,
	ASTER_ETIMEDOUT = -8,
	ASTER_EEXIST = -9,
	ASTER_EBADF = -10,     /* invalid / foreign / closed handle */
	ASTER_ENOSPC = -11,
	ASTER_ESTATE = -12,    /* lifecycle state violation */
	ASTER_EABI = -13,      /* ABI version incompatible */
	ASTER_ECORRUPT = -14,  /* bad package */
	ASTER_EAGAIN = -15,
	ASTER_ENODEV = -16,    /* device / interface not present */
	ASTER_ESIGN = -17,     /* package signature invalid / alien key */
	ASTER_EUNSIGNED = -18, /* unsigned package under require-signature policy */
	ASTER_ECANCELLED = -19, /* operation cancelled */
	ASTER_EMSGSIZE = -20,   /* UDP capacity/packet size; input preserved */
	ASTER_EPIPE = -21,      /* TCP write after EOF */
	ASTER_EDNS = -22,       /* name lookup failed (timeout remains ETIMEDOUT) */
	ASTER_ECONNRESET = -23,
	ASTER_ECONNREFUSED = -24,
	ASTER_ENETUNREACH = -25,
} aster_error_t;

#endif /* ASTER_ERROR_H */
