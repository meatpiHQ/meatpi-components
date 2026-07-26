# log_sinks host tests

Host-side (linux target) Unity tests for the pure core
(`log_sinks_core.c`): the drop-oldest record ring, the whole-record
batcher, and the file-rotation planner. No lwip/FreeRTOS/VFS.

Run on the bench Pi via the repo harness:

    .\test.ps1 host log_sinks

or directly:

    idf.py --preview set-target linux && idf.py build
    ./build/log_sinks_host_test.elf
