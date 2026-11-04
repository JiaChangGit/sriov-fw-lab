#ifndef LAB_RING_H
#define LAB_RING_H
#include <stdint.h>
/* entries 已由 CONFIG 路徑驗證為 16..256 的二次冪。 */
static inline uint32_t lab_ring_index(uint32_t counter,uint32_t entries)
{ return counter&(entries-1u); }
/* 初始記憶體為零，第一圈使用 phase=1；每繞一圈反轉，區分新 completion 與舊內容。 */
static inline uint8_t lab_ring_phase(uint32_t counter,uint32_t entries)
{ return (uint8_t)(1u^((counter/entries)&1u)); }
/* 回繞差值由 unsigned subtraction 計算；容量檢查由各呼叫點負責。 */
static inline uint32_t lab_ring_used(uint32_t tail,uint32_t head)
{ return tail-head; }
#endif
