#!/usr/bin/env python3
# Copyright 2022 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import pathlib
import stat
import unittest
import tempfile
import os

from common.common_arguments import build_common_argument_parser
from common.benchmark_config import BenchmarkConfig, TraceCaptureConfig


class BenchmarkConfigTest(unittest.TestCase):

  def setUp(self):
    self.build_dir = tempfile.TemporaryDirectory()
    self.tmp_dir = tempfile.TemporaryDirectory()
    self.build_dir_path = pathlib.Path(self.build_dir.name).resolve()
    self.normal_tool_dir = self.build_dir_path / "normal_tool"
    self.normal_tool_dir.mkdir()
    self.traced_tool_dir = self.build_dir_path / "traced_tool"
    self.traced_tool_dir.mkdir()
    self.trace_capture_tool = tempfile.NamedTemporaryFile()
    os.chmod(self.trace_capture_tool.name, stat.S_IEXEC)

  def tearDown(self):
    self.trace_capture_tool.close()
    self.tmp_dir.cleanup()
    self.build_dir.cleanup()

  def test_build_from_args(self):
    args = build_common_argument_parser().parse_args([
        f"--tmp_dir={self.tmp_dir.name}",
        f"--normal_benchmark_tool_dir={self.normal_tool_dir}",
        f"--traced_benchmark_tool_dir={self.traced_tool_dir}",
        f"--trace_capture_tool={self.trace_capture_tool.name}",
        f"--capture_tarball=capture.tar", f"--driver_filter_regex=a",
        f"--model_name_regex=b", f"--mode_regex=c", f"--keep_going",
        f"--benchmark_min_time=10", self.build_dir.name
    ])

    config = BenchmarkConfig.build_from_args(args=args, git_commit_hash="abcd")

    per_commit_tmp_dir = pathlib.Path(self.tmp_dir.name).resolve() / "abcd"
    expected_trace_capture_config = TraceCaptureConfig(
        traced_benchmark_tool_dir=self.traced_tool_dir,
        trace_capture_tool=pathlib.Path(self.trace_capture_tool.name).resolve(),
        capture_tarball=pathlib.Path("capture.tar").resolve(),
        capture_tmp_dir=per_commit_tmp_dir / "captures")
    expected_config = BenchmarkConfig(
        root_benchmark_dir=self.build_dir_path / "benchmark_suites",
        benchmark_results_dir=per_commit_tmp_dir / "benchmark-results",
        git_commit_hash="abcd",
        normal_benchmark_tool_dir=self.normal_tool_dir,
        trace_capture_config=expected_trace_capture_config,
        driver_filter="a",
        model_name_filter="b",
        mode_filter="c",
        keep_going=True,
        benchmark_min_time=10)
    self.assertEqual(config, expected_config)

  def test_build_from_args_benchmark_only(self):
    args = build_common_argument_parser().parse_args([
        f"--tmp_dir={self.tmp_dir.name}",
        f"--normal_benchmark_tool_dir={self.normal_tool_dir}",
        self.build_dir.name
    ])

    config = BenchmarkConfig.build_from_args(args=args, git_commit_hash="abcd")

    self.assertIsNone(config.trace_capture_config)

  def test_build_from_args_invalid_capture_args(self):
    args = build_common_argument_parser().parse_args([
        f"--tmp_dir={self.tmp_dir.name}",
        f"--normal_benchmark_tool_dir={self.normal_tool_dir}",
        f"--traced_benchmark_tool_dir={self.traced_tool_dir}",
        self.build_dir.name
    ])

    self.assertRaises(
        ValueError,
        lambda: BenchmarkConfig.build_from_args(args=args,
                                                git_commit_hash="abcd"))


if __name__ == "__main__":
  unittest.main()
