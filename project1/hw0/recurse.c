#include <stdio.h>

int recur(int i) {
    volatile int j = i;
    printf("recur call %d: stack@ %p\n", i, (void *)&j);
    if (i > 0) {
        recur(i - 1);
    }
    return i;
}
