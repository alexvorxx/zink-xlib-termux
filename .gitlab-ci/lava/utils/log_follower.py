#!/usr/bin/env python3
#
# Copyright (C) 2022 Collabora Limited
# Author: Guilherme Gallo <guilherme.gallo@collabora.com>
#
# SPDX-License-Identifier: MIT

"""
Some utilities to analyse logs, create gitlab sections and other quality of life
improvements
"""

import logging
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import Optional, Union

from lava.exceptions import MesaCITimeoutError
from lava.utils.console_format import CONSOLE_LOG
from lava.utils.gitlab_section import GitlabSection
from lava.utils.lava_log_hints import LAVALogHints
from lava.utils.log_section import (
    DEFAULT_GITLAB_SECTION_TIMEOUTS,
    FALLBACK_GITLAB_SECTION_TIMEOUT,
    LOG_SECTIONS,
    LogSectionType,
)


@dataclass
class LogFollower:
    current_section: Optional[GitlabSection] = None
    timeout_durations: dict[LogSectionType, timedelta] = field(
        default_factory=lambda: DEFAULT_GITLAB_SECTION_TIMEOUTS,
    )
    fallback_timeout: timedelta = FALLBACK_GITLAB_SECTION_TIMEOUT
    _buffer: list[str] = field(default_factory=list, init=False)
    log_hints: LAVALogHints = field(init=False)

    def __post_init__(self):
        section_is_created = bool(self.current_section)
        section_has_started = bool(
            self.current_section and self.current_section.has_started
        )
        self.log_hints = LAVALogHints(self)
        assert (
            section_is_created == section_has_started
        ), "Can't follow logs beginning from uninitialized GitLab sections."

        # Initialize fix_lava_gitlab_section_log generator
        self.gl_section_fix_gen = fix_lava_gitlab_section_log()
        next(self.gl_section_fix_gen)

    @property
    def phase(self) -> LogSectionType:
        return (
            self.current_section.type
            if self.current_section
            else LogSectionType.UNKNOWN
        )

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """Cleanup existing buffer if this object gets out from the context"""
        self.clear_current_section()
        last_lines = self.flush()
        for line in last_lines:
            print(line)

    def watchdog(self):
        if not self.current_section:
            return

        timeout_duration = self.timeout_durations.get(
            self.current_section.type, self.fallback_timeout
        )

        if self.current_section.delta_time() > timeout_duration:
            raise MesaCITimeoutError(
                f"Gitlab Section {self.current_section} has timed out",
                timeout_duration=timeout_duration,
            )

    def clear_current_section(self):
        if self.current_section and not self.current_section.has_finished:
            self._buffer.append(self.current_section.end())
            self.current_section = None

    def update_section(self, new_section: GitlabSection):
        # Sections can have redundant regex to find them to mitigate LAVA
        # interleaving kmsg and stderr/stdout issue.
        if self.current_section and self.current_section.id == new_section.id:
            return
        self.clear_current_section()
        self.current_section = new_section
        self._buffer.append(new_section.start())

    def manage_gl_sections(self, line):
        if isinstance(line["msg"], list):
            logging.debug("Ignoring messages as list. Kernel dumps.")
            return

        for log_section in LOG_SECTIONS:
            if new_section := log_section.from_log_line_to_section(line):
                self.update_section(new_section)

    def detect_kernel_dump_line(self, line: dict[str, Union[str, list]]) -> bool:
        # line["msg"] can be a list[str] when there is a kernel dump
        if isinstance(line["msg"], list):
            return line["lvl"] == "debug"

        # result level has dict line["msg"]
        if not isinstance(line["msg"], str):
            return False

        # we have a line, check if it is a kernel message
        if re.search(r"\[[\d\s]{5}\.[\d\s]{6}\] +\S{2,}", line["msg"]):
            print_log(f"{CONSOLE_LOG['BOLD']}{line['msg']}{CONSOLE_LOG['RESET']}")
            return True

        return False

    def feed(self, new_lines: list[dict[str, str]]) -> bool:
        """Input data to be processed by LogFollower instance
        Returns true if the DUT (device under test) seems to be alive.
        """

        self.watchdog()

        # No signal of job health in the log
        is_job_healthy = False

        for line in new_lines:
            if self.detect_kernel_dump_line(line):
                continue

            # At least we are fed with a non-kernel dump log, it seems that the
            # job is progressing
            is_job_healthy = True
            self.manage_gl_sections(line)
            if parsed_line := self.parse_lava_line(line):
                self._buffer.append(parsed_line)

        self.log_hints.detect_failure(new_lines)

        return is_job_healthy

    def flush(self) -> list[str]:
        buffer = self._buffer
        self._buffer = []
        return buffer

    def parse_lava_line(self, line) -> Optional[str]:
        prefix = ""
        suffix = ""

        if line["lvl"] in ["results", "feedback", "debug"]:
            return
        elif line["lvl"] in ["warning", "error"]:
            prefix = CONSOLE_LOG["FG_RED"]
            suffix = CONSOLE_LOG["RESET"]
        elif line["lvl"] == "input":
            prefix = "$ "
            suffix = ""
        elif line["lvl"] == "target":
            # gl_section_fix_gen will output the stored line if it can't find a
            # match for the first split line
            # So we can recover it and put it back to the buffer
            if recovered_first_line := self.gl_section_fix_gen.send(line):
                self._buffer.append(recovered_first_line)

        return f'{prefix}{line["msg"]}{suffix}'

def fix_lava_gitlab_section_log():
    """This function is a temporary solution for the Gitlab section markers
    splitting problem. Gitlab parses the following lines to define a collapsible
    gitlab section in their log:
    - \x1b[0Ksection_start:timestamp:section_id[collapsible=true/false]\r\x1b[0Ksection_header
    - \x1b[0Ksection_end:timestamp:section_id\r\x1b[0K
    There is some problem in message passing between the LAVA dispatcher and the
    device under test (DUT), that replaces \r control characters into \n. When
    this problem is fixed on the LAVA side, one should remove this function.
    """
    while True:
        line = yield False
        first_line = None
        split_line_pattern = re.compile(r"\x1b\[0K(section_\w+):(\d+):([^\s\r]+)$")
        second_line_pattern = re.compile(r"\x1b\[0K([\S ]+)?")

        if not re.search(split_line_pattern, line["msg"]):
            continue

        first_line = line["msg"]
        # Delete the current line and hold this log line stream to be able to
        # possibly merge it with the next line.
        line["msg"] = ""
        line = yield False

        # This code reached when we detect a possible first split line
        if re.search(second_line_pattern, line["msg"]):
            assert first_line
            line["msg"] = f"{first_line}\r{line['msg']}"
        else:
            # The current line doesn't match with the previous one, send back the
            # latter to give the user the chance to recover it.
            yield first_line



def print_log(msg):
    # Reset color from timestamp, since `msg` can tint the terminal color
    print(f"{CONSOLE_LOG['RESET']}{datetime.now()}: {msg}")


def fatal_err(msg):
    colored_msg = f"{CONSOLE_LOG['FG_RED']}"
    f"{msg}"
    f"{CONSOLE_LOG['RESET']}"
    print_log(colored_msg)
    sys.exit(1)


def hide_sensitive_data(yaml_data, hide_tag="HIDEME"):
    return "".join(line for line in yaml_data.splitlines(True) if hide_tag not in line)
