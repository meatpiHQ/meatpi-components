# partition_migrate host test

Unity tests on the pure core (`partition_migrate_core.c`): the parser and
the migrate / foreign rule, against the two real WiCAN Pro tables as byte
vectors (the v4.51p release's `partition-table.bin` and the v6 build's,
MD5 entry included).

Run on the bench Pi with the IDF linux target:

```
.\test.ps1 host partition_migrate
```

or by hand:

```
idf.py --preview set-target linux
idf.py build
./build/partition_migrate_host_test.elf
```

Expected output (the tail):

```
test_core.c:...:test_parse_the_real_tables:PASS
test_core.c:...:test_same_table_is_ours:PASS
test_core.c:...:test_legacy_pro_table_migrates:PASS
test_core.c:...:test_moved_app_slot_is_foreign:PASS
test_core.c:...:test_changed_nvs_or_otadata_is_foreign:PASS
test_core.c:...:test_missing_phy_init_is_foreign:PASS
test_core.c:...:test_blank_flash_is_foreign:PASS
test_core.c:...:test_garbage_is_foreign:PASS
test_core.c:...:test_extra_data_partition_migrates:PASS
test_core.c:...:test_md5_entry_ends_the_table:PASS
test_core.c:...:test_too_many_entries_is_foreign:PASS
test_core.c:...:test_unparsable_own_table_is_foreign:PASS

-----------------------
12 Tests 0 Failures 0 Ignored
OK
```
