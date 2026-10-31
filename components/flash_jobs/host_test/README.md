# flash_jobs host test

Unity tests on the pure core (`flash_jobs_core.c`): where a call runs (an
internal stack runs in line), the first publish asks for a worker, a live
worker needs no spawn, a failed spawn withdraws the job, a pending job runs
on the signal and still runs when the worker's wait ran out first, an idle
period ends the worker and the next caller creates the next one, the exit
keeps the lowest watermark, the counters tell the two paths apart, refusals
are counted and logged once per period on a wrapping clock.

Run on the bench Pi with the IDF linux target:

```
.\test.ps1 host flash_jobs
```

or by hand:

```
idf.py --preview set-target linux
idf.py build
./build/flash_jobs_host_test.elf
```

Expected output (the tail):

```
test_main.c:...:test_an_internal_stack_runs_in_line:PASS
test_main.c:...:test_the_first_publish_asks_for_a_worker:PASS
test_main.c:...:test_a_publish_with_a_live_worker_needs_no_spawn:PASS
test_main.c:...:test_a_failed_spawn_withdraws_the_job:PASS
test_main.c:...:test_the_worker_runs_a_pending_job_on_its_signal:PASS
test_main.c:...:test_a_job_published_at_the_timeout_still_runs:PASS
test_main.c:...:test_an_idle_period_ends_the_worker:PASS
test_main.c:...:test_the_exit_keeps_the_lowest_watermark:PASS
test_main.c:...:test_the_counters_tell_the_two_paths_apart:PASS
test_main.c:...:test_refusals_are_counted_and_logged_once_per_period:PASS

-----------------------
10 Tests 0 Failures 0 Ignored
OK
```
