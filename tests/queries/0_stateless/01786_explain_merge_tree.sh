#!/usr/bin/env bash

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

$CLICKHOUSE_CLIENT -q "create table test_index (x UInt32, y UInt32, z UInt32, t UInt32, index t_minmax t % 20 TYPE minmax GRANULARITY 2, index t_set t % 19 type set(4) granularity 2) engine = MergeTree order by (x, y) partition by (y, bitAnd(z, 3), intDiv(t, 15)) settings index_granularity = 2"
$CLICKHOUSE_CLIENT -q "insert into test_index select number, number > 3 ? 3 : number, number = 1 ? 1 : 0, number from numbers(20)"

$CLICKHOUSE_CLIENT -q "
    explain actions = 1 select *, _part from test_index where t % 19 = 16 and y > 0 and bitAnd(z, 3) != 1 and x > 10 and t % 20 > 14;
    " | grep -A 100 "ReadFromMergeTree"

echo "-----------------"

$CLICKHOUSE_CLIENT -q "
    explain actions = 1 select x from test_index where x > 10 order by x;
    " | grep -A 100 "ReadFromMergeTree"

echo "-----------------"

$CLICKHOUSE_CLIENT -q "
    explain actions = 1 select x from test_index where x > 10 order by x desc;
    " | grep -A 100 "ReadFromMergeTree"

