#!/bin/sh
#
# Copyright (c) 2020 Google LLC
#

test_description='reftable unittests'

. ./test-lib.sh

test_expect_success 'unittests' '
       test-tool reftable
'

test_done
