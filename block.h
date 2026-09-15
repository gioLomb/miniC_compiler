#ifndef BLOCK_H
#define BLOCK_H


/**
 * @file block.h
 * @brief Minimal basic-block range and CFG-successor descriptor types.
 * Reused wherever only instruction-range boundaries and up to two
 * successor edges are needed, e.g. by the scheduler and register allocator.
 */

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