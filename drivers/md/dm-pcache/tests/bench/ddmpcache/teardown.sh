#!/bin/bash
set -u
dmsetup remove pcache-bench 2>/dev/null || true
rmmod dm_pcache 2>/dev/null || true
