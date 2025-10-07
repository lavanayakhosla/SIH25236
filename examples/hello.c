#include <stdio.h>
const char* msg = "Hello, world!";

int sum(int a, int b) {
    return a + b;
}

int main() {
    printf("%s\n", msg);
    int x = sum(10, 20);
    printf("sum=%d\n", x);
    return 0;
}
