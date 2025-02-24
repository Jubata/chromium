#!/usr/bin/env python
# Copyright 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import json
import sys


def MergeJson(output_json, jsons_to_merge, print_input=True):
  """Merge the contents of one or more results JSONs into a single JSON.

  Args:
    output_json: A path to a JSON file to which the merged results should be
      written.
    jsons_to_merge: A list of paths to JSON files that should be merged.
  """
  shard_results_list = []
  for j in jsons_to_merge:
    with open(j) as f:
      shard_results_list.append(json.load(f))

  if print_input:
    print "Outputing the shard_results_list:"
    print shard_results_list

  merged_results = shard_results_list[0]

  with open(output_json, 'w') as f:
    json.dump(merged_results, f)

  return 0


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('-o', '--output-json', required=True)
  parser.add_argument('--build-properties', help=argparse.SUPPRESS)
  parser.add_argument('--summary-json', help=argparse.SUPPRESS)
  parser.add_argument('--verbose', help=argparse.SUPPRESS)
  parser.add_argument('jsons_to_merge', nargs='*')

  args = parser.parse_args()
  return MergeJson(args.output_json, args.jsons_to_merge)


if __name__ == '__main__':
  sys.exit(main())
