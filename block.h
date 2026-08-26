#ifndef BLOCK_H
#define BLOCK_H

/**
 * @brief Minimal [start, end) instruction range, reusable standalone
 *        wherever only block boundaries are needed (no CFG edges) —
 *        e.g. the scheduler's per-basic-block partitioning in sched.c.
 */
typedef struct {
    int start;   /**< index of first instruction in the block (inclusive) */
    int end;     /**< index one past the last instruction (exclusive)     */
} BlockRange;

/*
 * BasicBlock: campi comuni a tutti i blocchi base del compilatore.
 *
 * range e' un campo NOMINATO (non anonimo): ogni accesso a start/end passa
 * per bb.range.start / bb.range.end. succ[] resta un campo diretto di
 * BasicBlock, non dentro range.
 */
typedef struct {
    BlockRange range;
    int succ[2];    /* successori nel CFG; -1 = assente */
} BasicBlock;

#endif /* BLOCK_H */