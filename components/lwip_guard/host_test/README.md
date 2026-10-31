# lwip_guard host test

Unity tests on the pure core (`lwip_guard_core.c`): the refusal counter and
the log rate limit (the first refusal is logged, then one per 10 s, every
refusal counted, a wrapping millisecond clock).

Run on the bench Pi with the IDF linux target:

```
.\test.ps1 host lwip_guard
```

or by hand:

```
idf.py --preview set-target linux
idf.py build
./build/lwip_guard_host_test.elf
```

Expected output (the tail):

```
test_main.c:...:test_first_refusal_is_logged:PASS
test_main.c:...:test_a_second_refusal_inside_the_period_is_counted_not_logged:PASS
test_main.c:...:test_one_line_per_period:PASS
test_main.c:...:test_every_refusal_counts:PASS
test_main.c:...:test_wrapping_clock:PASS

-----------------------
5 Tests 0 Failures 0 Ignored
OK
```
