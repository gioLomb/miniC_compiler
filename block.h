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

typedef struct {
    BlockRange range;
    int succ[2];    /* successors in CFG; -1 if absent */
} BasicBlock;

#endif /* BLOCK_H */