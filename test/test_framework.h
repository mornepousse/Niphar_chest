/* Harnais minimal pour les tests hôte du coffre Niphar.
 *
 * Repris de KeSp_firmware/test/test_framework.h, allégé des constantes propres
 * au clavier. Garder les deux compatibles : les tests de sec_confirm et, plus
 * tard, ceux de la pile CCID, se portent d'un dépôt à l'autre sans retouche.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compteurs partagés — définis dans test_main.c */
extern int _test_pass_count;
extern int _test_fail_count;

#define TEST_SUITE(name) \
    do { printf("\n--- %s ---\n", name); } while (0)

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        _test_fail_count++; \
    } else { \
        _test_pass_count++; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s: attendu %lld, obtenu %lld (%s:%d)\n", \
               msg, _b, _a, __FILE__, __LINE__); \
        _test_fail_count++; \
    } else { \
        _test_pass_count++; \
    } \
} while (0)

#define TEST_RUN(fn) do { \
    printf("  [%s] ", #fn); \
    fn(); \
    printf("OK\n"); \
} while (0)
