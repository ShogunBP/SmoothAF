#include <stdio.h>
#include "../src/smoothaf_logger_fio.h"

int main(void)
{
    printf("size=0x%zx filename=0x%zx bytes=0x%zx max_file_size=0x%zx\n",
           sizeof(struct smoothaf_log_fio_backend),
           offsetof(struct smoothaf_log_fio_backend, current_filename),
           offsetof(struct smoothaf_log_fio_backend, bytes_written),
           offsetof(struct smoothaf_log_fio_backend, max_file_size_bytes));
    return 0;
}
