#include <stdio.h>
int main()
{
    int ch;
    while ((ch = getchar()) != EOF) putchar(ch ^ 0x84);
    return 0;
}
