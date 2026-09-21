#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern int lg_checks;
extern int lg_failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        lg_checks++;                                                                \
        if (!(cond)) {                                                              \
            lg_failures++;                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        lg_checks++;                                                                \
        long long lg_a_ = (long long)(a);                                           \
        long long lg_b_ = (long long)(b);                                           \
        if (lg_a_ != lg_b_) {                                                       \
            lg_failures++;                                                          \
            printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__,     \
                   #a, #b, lg_a_, lg_b_);                                           \
        }                                                                           \
    } while (0)

size_t hex2bin(const char *hex, uint8_t *out, size_t cap);

void test_core(void);
void test_crypto(void);
void test_messaging(void);
void test_lora(void);
void test_identity(void);
void test_gps_plan(void);
