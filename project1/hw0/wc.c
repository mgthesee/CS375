#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

int main(int argc, char *argv[]) {
    FILE *infile = NULL;

    // Determine input stream based on command line arguments
    if (argc == 1) {
        infile = stdin;
    } else if (argc == 2) {
        infile = fopen(argv[1], "r");
        if (infile == NULL) {
            perror("fopen");
            return 1;
        }
    } else {
        fprintf(stderr, "Usage: %s [FILE]\n", argv[0]);
        return 1;
    }

    int lines = 0;
    int words = 0;
    int chars = 0;
    int in_word = 0;
    int ch;

    // Read character by character until End of File
    while ((ch = fgetc(infile)) != EOF) {
        chars++;

        if (ch == '\n') {
            lines++;
        }

        if (isspace(ch)) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }

    // Print counts matching standard wc order: lines, words, characters
    if (argc == 2) {
        printf("%d %d %d %s\n", lines, words, chars, argv[1]);
        fclose(infile);
    } else {
        printf("%d %d %d\n", lines, words, chars);
    }

    return 0;
}
