#ifndef _AQM_H_
#define _AQM_H_

/*
 * Bit manipulation (not used currently)
 * Will use just one variable of 4 byts to enclose the following:
 * 0-7:   locked or unlocked
 * 8-15:  shuffle leader or not
 * 16-31: shuffle count
 */
#define _AQ_MCS_SET_MASK(type)  (((1U << _AQ_MCS_ ## type ## _BITS) -1)\
                                 << _AQ_MCS_ ## type ## _OFFSET)
#define _AQ_MCS_GET_VAL(v, type)   (((v) & (_AQ_MCS_ ## type ## _MASK)) >>\
                                    (_AQ_MCS_ ## type ## _OFFSET))
#define _AQ_MCS_LOCKED_OFFSET   0
#define _AQ_MCS_LOCKED_BITS     8
#define _AQ_MCS_LOCKED_MASK     _AQ_MCS_SET_MASK(LOCKED)
#define _AQ_MCS_LOCKED_VAL(v)   _AQ_MCS_GET_VAL(v, LOCKED)

#define _AQ_MCS_SLEADER_OFFSET  (_AQ_MCS_LOCKED_OFFSET + _AQ_MCS_LOCKED_BITS)
#define _AQ_MCS_SLEADER_BITS    8
#define _AQ_MCS_SLEADER_MASK    _AQ_MCS_SET_MASK(SLEADER)
#define _AQ_MCS_SLEADER_VAL(v)  _AQ_MCS_GET_VAL(v, SLEADER)

#define _AQ_MCS_WCOUNT_OFFSET   (_AQ_MCS_SLEADER_OFFSET + _AQ_MCS_SLEADER_BITS)
#define _AQ_MCS_WCOUNT_BITS     16
#define _AQ_MCS_WCOUNT_MASK     _AQ_MCS_SET_MASK(WCOUNT)
#define _AQ_MCS_WCOUNT_VAL(v)   _AQ_MCS_GET_VAL(v, WCOUNT)

#define _AQ_MCS_NOSTEAL_VAL     (1U << (_AQ_MCS_LOCKED_OFFSET + _AQ_MCS_LOCKED_BITS))

#define _AQ_MCS_STATUS_PARKED   0 /* node's status is changed to park */
#define _AQ_MCS_STATUS_PWAIT    1 /* starting point for everyone */
#define _AQ_MCS_STATUS_UNPWAIT  2 /* waiter is never scheduled out in this state */
#define _AQ_MCS_STATUS_LOCKED   4 /* node is now going to be the lock holder */
#define _AQ_MAX_LOCK_COUNT      256u

#endif /* _AQM_H_ */

