/*
 * hh_mem.h - heap readings at named points, so RAM use can be attributed rather than guessed.
 *
 * Each mark logs internal free heap, the lowest it has been, the largest free block, and how
 * much the free figure fell since the previous mark:
 *     [MEM] after display: free 180 KB (-42), lowest 176 KB, largest block 110 KB
 * Allocations made by other tasks between two marks (the Wi-Fi driver joining, for example)
 * land in whichever interval they fall in, so a mark taken while the network is busy is
 * approximate. The `mem` console command prints the same line on demand.
 */
#pragma once

void hh_mem_mark(const char *stage);
