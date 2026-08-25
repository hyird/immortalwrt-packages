#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge migration test failed: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static char *read_script(void) {
    FILE *input = fopen(EDGENODE_MIGRATION_SCRIPT, "rb");
    require(input != NULL, "cannot open migration script");
    require(fseek(input, 0, SEEK_END) == 0, "cannot size migration script");
    const long length = ftell(input);
    require(length >= 0, "cannot read migration script size");
    require(fseek(input, 0, SEEK_SET) == 0, "cannot rewind migration script");

    char *contents = malloc((size_t)length + 1U);
    require(contents != NULL, "cannot allocate migration script buffer");
    require(fread(contents, 1U, (size_t)length, input) == (size_t)length,
            "cannot read migration script");
    contents[length] = '\0';
    fclose(input);
    return contents;
}

int main(void) {
    char *script = read_script();
    const char *remove_credentials = strstr(script, "rm -rf -- /etc/edgenode/credentials");
    const char *remove_option =
        strstr(script, "uci -q delete \"edgenode.$section.enrollment_token_file\"");
    const char *existing_platform_exit =
        strstr(script, "[ \"$platform_count\" -ne 0 ] && exit 0");

    require(remove_credentials != NULL, "legacy credential directory is not removed");
    require(remove_option != NULL, "legacy enrollment token option is not removed");
    require(existing_platform_exit != NULL, "existing platform guard is missing");
    require(remove_credentials < existing_platform_exit,
            "credential cleanup runs after existing platform guard");
    require(remove_option < existing_platform_exit,
            "legacy option cleanup runs after existing platform guard");
    require(strstr(script, "uci -q commit edgenode") != NULL,
            "legacy option cleanup is not committed");

    free(script);
    puts("edge migration tests passed");
    return EXIT_SUCCESS;
}
