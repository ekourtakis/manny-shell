#include <stdlib.h>
#include <string.h>

#include "parse.h"

char** parse(char* line, const char delim[]) {
    int n = 0;
    char* token;
    char** array = malloc(sizeof(char*));

    if (array == NULL) {
        return NULL;
    }
    array[0] = NULL;

    token = strtok(line, delim);
    while (token != NULL) {
        char** temp = realloc(array, (n + 2) * sizeof(char*));

        if (temp == NULL) {
            free(array);
            return NULL;
        }

        array = temp;
        array[n++] = token;
        array[n] = NULL;
        token = strtok(NULL, delim);
    }

    return array;
}
