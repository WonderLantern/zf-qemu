/* Original validator40f2b2cc..40f2b344 compares the two armed readout
 * latches: A = ch12 rising (u64 a03cc5068), B = ch11 rising (u64 a03cc5070),
 * and reports 0x330c unless (A-B)/25 >= *(u32)a03cc4ef0. At
 * 100MHz/(divider+1) = 25MHz one microsecond is 25 ticks, so the two inputs
 * must be separated by at least the configured minimum (500 = 500us in the
 * observed runs). A shared edge makes A-B <= 0 and is rejected by the same
 * check. This wrapper therefore offsets the second channel by a declared
 * 655.36us (16384 ticks): a functional timing hypothesis, not a measured
 * sensor wire. The measured operands are retained as model constants. */
#define ZF_MIO_PAIR_SEPARATION_NS 655360
