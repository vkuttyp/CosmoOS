#!/usr/bin/env python3
"""The user-mode suite's per-section timings
(docs/audit/next-subsystem-usertest-sections.md).

`process-user` is one SELFTEST line standing for the whole user-mode
suite, so when it is slow nothing says which part of it was. The suite
now times each section and the harness parses them; these are the
host-side checks on that parse, because the defect is entirely in what
the harness does with the lines and a proof that boots a guest would
take two minutes to demonstrate what a list of strings shows at once.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_boot_test import (summarize_sections, section_failures,  # noqa: E402
                           format_section_summary, USERTEST_SECTIONS)

FAILURES = []
CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    print(f"{'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        FAILURES.append(what)


def run(timings, total=None, extra=()):
    """A run's console lines: the suite's sections, its total, and
    whatever else the guest printed around them."""
    lines = list(extra)
    lines += [f"USERTEST: section {n} {ms} ms" for n, ms in timings]
    if total is not None:
        lines.append(f"USERTEST: sections {len(timings)}, total {total} ms")
    return lines


def full(slow=None, slow_ms=3000):
    """Every section, uniformly quick, with one optionally slow."""
    return [(n, slow_ms if n == slow else 40) for n in USERTEST_SECTIONS]


def test_sections_parsed():
    """Ten lines in, ten rows out, in the order printed."""
    timings = full()
    sections, declared, total = summarize_sections(run(timings, total=400))
    check(sections == timings, f"ten sections parse in order (got {len(sections)})")
    check(declared == 10, f"the declared count is read (got {declared})")
    check(total == 400, f"the total is read (got {total})")
    check(section_failures(sections, declared, total) == [],
          "a complete run is not a failure")


def test_lines_among_noise():
    """The suite's lines are found among everything else a boot prints."""
    noise = ["[ INFO] process: pid 178 'init' created",
             "usertest: sockets ok",
             "SELFTEST: process-user     ... ok (3711 ms)",
             "USERTEST: PASS"]
    sections, declared, total = summarize_sections(run(full(), total=400, extra=noise))
    check(len(sections) == 10, f"ten sections found among other output (got {len(sections)})")
    check(declared == 10 and total == 400, "the total line survives the noise")
    # `usertest: sockets ok` is prose and must not be mistaken for a section.
    check(all(n in USERTEST_SECTIONS for n, _ in sections),
          "the lowercase prose lines are not parsed as sections")


def test_slowest_named():
    """THE BUG-PROOF. Two runs whose totals are identical and whose
    only difference is WHICH section is slow must produce different
    summaries, each naming the right one.

    Today's single `SELFTEST: process-user ... (N ms)` line is the same
    string for both, which is the whole defect. The two runs differ
    only in the durations, so a formatter that names a section for
    being first in the list, last, or longest-named still fails this.
    """
    a, b = full(slow="svc"), full(slow="net")
    check(sum(ms for _, ms in a) == sum(ms for _, ms in b),
          "the two runs have identical totals")
    la = format_section_summary(a, sum(ms for _, ms in a))
    lb = format_section_summary(b, sum(ms for _, ms in b))
    check(la != lb, "the summaries differ")
    check("svc 3000 ms" in la and "svc" in la.split("slowest: ")[1].split(",")[0],
          f"the svc run names svc first ({la.split('slowest: ')[1][:30]})")
    check("net 3000 ms" in lb and "net" in lb.split("slowest: ")[1].split(",")[0],
          f"the net run names net first ({lb.split('slowest: ')[1][:30]})")
    # And neither names the other as its slowest.
    check(not la.split("slowest: ")[1].startswith("net "),
          "the svc run does not lead with net")
    check(not lb.split("slowest: ")[1].startswith("svc "),
          "the net run does not lead with svc")


def test_total_reconciled():
    """The sum and the kernel's own duration are both shown, and their
    difference is not silently dropped: if the sections account for
    half the line, the interesting half is the unmeasured one."""
    line = format_section_summary(full(), 3610, suite_ms=3711)
    check("3610 ms" in line, "the suite's own sum is shown")
    check("3711 ms" in line, "the process-user line is shown")
    check("101 ms is spawn and teardown" in line, f"the difference is named ({line[-46:]})")
    bare = format_section_summary(full(), 3610)
    check("spawn and teardown" not in bare,
          "with no process-user line there is no invented difference")


def test_truncated_run_parses():
    """A run that stopped part-way through the suite -- a hang -- parses
    to the sections that did finish, and says the total never came.
    This is the case whose output matters most, and it is why the
    parser tolerates a short list."""
    stopped = full()[:4]
    sections, declared, total = summarize_sections(run(stopped, total=None))
    check([n for n, _ in sections] == ["fs", "fsctl", "net", "proc"],
          f"the four that finished are named (got {[n for n, _ in sections]})")
    check(declared is None and total is None, "no total line was invented")
    fails = section_failures(sections, declared, total)
    check(len(fails) == 1 and "stopped part-way" in fails[0],
          f"the run is refused, saying it stopped part-way ({fails})")
    check("saw 4 section line(s)" in fails[0],
          f"and says how many did finish ({fails[0]})")


def test_a_truncated_run_reads_sensibly():
    """The summary for a run with no total must not print "None ms" --
    the truncated run is the case whose output matters most, so its
    line has to be the most readable, not the least."""
    stopped = full()[:4]
    line = format_section_summary(stopped, None)
    check("None" not in line, f"no None leaks into the line ({line})")
    check("unfinished" in line, f"the line says the suite did not finish ({line})")
    check("at least 160 ms" in line, f"and gives the floor it did reach ({line})")


def test_a_row_that_left_the_table():
    """Tolerating a truncated run must NOT mean an absent section can
    hide. A run that declares and prints nine, with `svc` gone, is
    refused by name."""
    nine = [(n, 40) for n in USERTEST_SECTIONS if n != "svc"]
    sections, declared, total = summarize_sections(run(nine, total=360))
    fails = section_failures(sections, declared, total)
    check(any("svc" in f for f in fails), f"the missing section is named ({fails})")


def test_declared_and_printed_must_agree():
    """The suite's own count is the check on a garbled stream: ten
    declared, nine printed, is a failure even though all nine parse."""
    lines = run(full()[:9], total=400)
    lines[-1] = "USERTEST: sections 10, total 400 ms"
    sections, declared, total = summarize_sections(lines)
    fails = section_failures(sections, declared, total)
    check(any("declared 10" in f and "printed 9" in f for f in fails),
          f"the mismatch is refused ({fails})")


def test_an_unknown_section_is_named():
    """A section added to the table in init.c and not here shows up as
    a name the harness does not know, rather than being counted
    silently."""
    extra = full() + [("brand-new", 40)]
    sections, declared, total = summarize_sections(run(extra, total=440))
    fails = section_failures(sections, declared, total)
    check(any("brand-new" in f for f in fails), f"the unknown section is named ({fails})")


def test_no_sections_is_not_an_error():
    """A release build runs no user-mode suite. It prints no section
    lines, and that is not a failure -- the caller does not even reach
    section_failures, so the parse must simply come back empty."""
    sections, declared, total = summarize_sections(
        ["[ INFO] boot complete; nothing more to do in this phase"])
    check(sections == [] and declared is None and total is None,
          "a run with no suite parses to nothing")


def test_a_zero_length_section_still_reports():
    """`trap_selftest` is an empty function on aarch64. The table calls
    it anyway, so it has a line, and 0 ms is a reading rather than an
    absence -- which is the difference between this and the prose
    markers it replaces."""
    timings = [(n, 0 if n == "trap" else 40) for n in USERTEST_SECTIONS]
    sections, declared, total = summarize_sections(run(timings, total=360))
    check(("trap", 0) in sections, "a zero-millisecond section is present, not missing")
    check(section_failures(sections, declared, total) == [],
          "and it is not a failure")


def main():
    for fn in (test_sections_parsed,
               test_lines_among_noise,
               test_slowest_named,
               test_total_reconciled,
               test_truncated_run_parses,
               test_a_truncated_run_reads_sensibly,
               test_a_row_that_left_the_table,
               test_declared_and_printed_must_agree,
               test_an_unknown_section_is_named,
               test_no_sections_is_not_an_error,
               test_a_zero_length_section_still_reports):
        fn()
    if FAILURES:
        print(f"usertest-sections: FAIL ({len(FAILURES)} of {CHECKS})")
        return 1
    print(f"usertest-sections: PASS ({CHECKS} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
